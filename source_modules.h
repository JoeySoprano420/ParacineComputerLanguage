#pragma once

namespace source_modules {
namespace fs = std::filesystem;

inline std::string utf8Path(const fs::path& path) {
	const auto text = path.generic_u8string();
	return std::string(text.begin(), text.end());
}

inline bool within(const fs::path& path, const fs::path& boundary) {
	const auto relative = path.lexically_relative(boundary);
	return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

inline std::vector<std::string> components(const std::string& name) {
	std::vector<std::string> result;
	for (std::size_t start = 0; start < name.size();) {
		const auto end = name.find("::", start);
		auto part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (part.empty() || part == "." || part == ".." || part.find_first_of("/\\: \t\r\n") != std::string::npos || part.back() == '.')
			throw std::runtime_error("E0522 invalid portable import component: " + part);
		auto upper = part;
		for (auto& c : upper) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
		if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
			(upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '1' && upper[3] <= '9'))
			throw std::runtime_error("E0522 reserved portable import component: " + part);
		result.push_back(std::move(part));
		if (end == std::string::npos) break;
		start = end + 2;
		if (start == name.size()) throw std::runtime_error("E0522 trailing import separator");
	}
	if (result.empty()) throw std::runtime_error("E0522 empty import path");
	return result;
}

inline std::string symbol(const std::string& identity, const std::string& name) {
	static constexpr char hex[] = "0123456789abcdef";
	std::string result = "__pcn_module_v1_";
	for (unsigned char c : identity) { result += hex[c >> 4]; result += hex[c & 15]; }
	result += '_'; result += name;
	return result;
}

struct Import { std::size_t target; std::string spelling, alias; bool reexport; SourceLocation location; };
struct Module {
	fs::path file, boundary;
	std::string identity, packageIdentity, source;
	Program program;
	bool explicitIdentity = false;
	std::vector<Import> imports;
};
struct Graph {
	std::vector<Module> modules;
	std::map<fs::path, std::string> inputs;
	fs::path projectRoot;
	std::size_t parsedModules = 0, reusedModules = 0;
	std::vector<SemanticDiagnostic> diagnostics;
};

inline Program parseCached(const fs::path& file, const std::string& source, Graph& graph, bool recover = false) {
	static std::mutex mutex;
	static std::map<fs::path, std::pair<std::string, Program>> cache;
	static std::size_t bytes = 0;
	{
		std::lock_guard lock(mutex);
		const auto found = cache.find(file);
		if (found != cache.end() && found->second.first == source) { ++graph.reusedModules; return found->second.second; }
	}
	std::vector<Token> tokens;
	try { tokens = Lexer(source, utf8Path(file)).tokenize(); }
	catch (const std::runtime_error& error) {
		if (!recover || !std::string_view(error.what()).starts_with("E01")) throw;
		graph.diagnostics.push_back(diagnosticFromException(error.what(), utf8Path(file)));
		return {};
	}
	auto parsed = Parser(std::move(tokens)).parseRecovering({}); ++graph.parsedModules;
	if (!parsed.succeeded()) {
		if (!recover) throw std::runtime_error(parsed.diagnostics.front().originalMessage);
		for (const auto& error : parsed.diagnostics) {
			SemanticDiagnostic diagnostic{ error.location, error.code + " " + error.message };
			diagnostic.phase = "parser"; graph.diagnostics.push_back(std::move(diagnostic));
		}
		return std::move(parsed.program);
	}
	auto program = std::move(parsed.program);
	{
		std::lock_guard lock(mutex);
		if (const auto found = cache.find(file); found != cache.end()) { bytes -= found->second.first.size(); cache.erase(found); }
		while (!cache.empty() && (cache.size() >= 128 || bytes + source.size() > 32 * 1024 * 1024)) {
			bytes -= cache.begin()->second.first.size(); cache.erase(cache.begin());
		}
		cache.emplace(file, std::pair{ source, program }); bytes += source.size();
	}
	return program;
}

inline void requirePortableSpelling(const fs::path& file, const fs::path& boundary) {
	auto cursor = boundary;
	for (const auto& component : file.lexically_relative(boundary)) {
		bool exact = false;
		for (const auto& entry : fs::directory_iterator(cursor)) if (entry.path().filename() == component) { exact = true; break; }
		if (!exact) throw std::runtime_error("E0527 source path case differs from directory entry: " + utf8Path(file));
		cursor /= component;
	}
}

inline Graph load(const Program& root, const fs::path& sourceFile, const std::string& source, bool recover = false) {
	Graph graph;
	const auto rootFile = fs::canonical(sourceFile);
	graph.projectRoot = rootFile.parent_path();
	for (auto cursor = graph.projectRoot;; cursor = cursor.parent_path()) {
		if (fs::is_regular_file(packageManifestPath(cursor))) { graph.projectRoot = cursor; break; }
		if (cursor.parent_path() == cursor) break;
	}
	std::map<fs::path, std::size_t> files;
	std::map<std::string, fs::path> identities;
	std::size_t inputBytes = source.size(), edges = 0;
	if (source.size() > 8 * 1024 * 1024) throw std::runtime_error("E0526 root source exceeds 8 MiB");
	graph.inputs.emplace(rootFile, source);
	auto capture = [&](const fs::path& file) {
		const auto canonical = fs::canonical(file);
		if (!within(canonical, graph.projectRoot)) throw std::runtime_error("E0527 dependency escapes project boundary: " + utf8Path(file));
		if (const auto found = graph.inputs.find(canonical); found != graph.inputs.end()) return found->second;
		if (fs::file_size(canonical) > 8 * 1024 * 1024) throw std::runtime_error("E0526 import source exceeds 8 MiB: " + utf8Path(file));
		auto data = readSourceFile(canonical);
		if (data.size() > 8 * 1024 * 1024 || inputBytes + data.size() > 64 * 1024 * 1024) throw std::runtime_error("E0526 module graph exceeds source byte budget");
		inputBytes += data.size(); graph.inputs.emplace(canonical, data);
		return data;
	};
	auto manifestAt = [&](const fs::path& boundary) -> std::optional<PackageManifest> {
		if (!fs::is_regular_file(packageManifestPath(boundary))) return std::nullopt;
		capture(packageManifestPath(boundary));
		const auto manifest = readPackageManifest(boundary);
		if (fs::is_regular_file(packageLockPath(boundary))) {
			capture(packageLockPath(boundary));
			const auto lock = readPackageLock(boundary);
			if (lock.size() != manifest.dependencies.size()) throw std::runtime_error("E0530 manifest and lock dependency sets differ");
			for (const auto& dependency : manifest.dependencies) {
				const auto pinned = std::find_if(lock.begin(), lock.end(), [&](const auto& entry) { return entry.name == dependency.name; });
				if (pinned == lock.end() || pinned->version != dependency.version || (!dependency.source.empty() && pinned->source != dependency.source))
					throw std::runtime_error("E0530 manifest and lock dependency identity differ: " + dependency.name);
			}
		}
		return manifest;
	};
	const auto projectManifest = manifestAt(graph.projectRoot);
	std::function<std::size_t(const fs::path&, const fs::path&, const std::string&, std::optional<Program>, std::size_t)> visit;
	visit = [&](const fs::path& file, const fs::path& boundary, const std::string& packageIdentity, std::optional<Program> parsed, std::size_t depth) -> std::size_t {
		if (const auto found = files.find(file); found != files.end()) {
			const auto& existing = graph.modules[found->second];
			if (existing.boundary != boundary || existing.packageIdentity != packageIdentity) throw std::runtime_error("E0528 canonical module reached through conflicting package identities");
			return found->second;
		}
		if (depth > 64) throw std::runtime_error("E0520 import nesting exceeds 64 files");
		if (files.size() >= 1024) throw std::runtime_error("E0524 source import graph exceeds 1024 files");
		Module module; module.file = file; module.boundary = boundary; module.packageIdentity = packageIdentity;
		module.source = file == rootFile ? source : capture(file);
		module.program = parsed ? std::move(*parsed) : parseCached(file, module.source, graph, recover);
		auto relative = file.lexically_relative(boundary); relative.replace_extension();
		std::string logical = utf8Path(relative);
		for (std::size_t pos = 0; (pos = logical.find('/', pos)) != std::string::npos; pos += 2) logical.replace(pos, 1, "::");
		for (const auto& declaration : module.program.declarations) if (const auto* header = std::get_if<ModuleDecl>(&declaration.node)) {
			logical = header->name; module.explicitIdentity = true;
		}
		(void)components(logical);
		module.identity = packageIdentity.empty() ? logical : packageIdentity + "::" + logical;
		if (module.identity.size() > 512) throw std::runtime_error("E0528 module identity exceeds 512 bytes");
		const auto [identity, inserted] = identities.emplace(module.identity, file);
		if (!inserted && identity->second != file) throw std::runtime_error("E0528 duplicate module identity: " + module.identity + " (" + utf8Path(identity->second) + ", " + utf8Path(file) + ")");
		const auto index = graph.modules.size(); files.emplace(file, index); graph.modules.push_back(std::move(module));
		const auto manifest = boundary == graph.projectRoot ? projectManifest : manifestAt(boundary);
		auto declarations = graph.modules[index].program.declarations;
		std::stable_sort(declarations.begin(), declarations.end(), [](const auto& a, const auto& b) {
			const auto* left = std::get_if<UseDecl>(&a.node); const auto* right = std::get_if<UseDecl>(&b.node);
			if (!left || !right) return left && !right;
			return std::tie(left->path, left->alias) < std::tie(right->path, right->alias);
		});
		for (const auto& declaration : declarations) {
			const auto* use = std::get_if<UseDecl>(&declaration.node);
			if (!use) continue;
			if (++edges > 8192) throw std::runtime_error("E0524 module graph exceeds 8192 import edges");
			if (use->path.size() != 1) throw std::runtime_error("E0522 malformed source import");
			const auto& name = use->path.front();
			if (name == "std::io" || name == "std::fs" || name == "std::process") {
				if (!use->alias.empty() || use->isPublic) throw std::runtime_error("E0521 built-in imports cannot be aliased or re-exported");
				continue;
			}
			const auto parts = components(name);
			fs::path targetBoundary = boundary;
			std::string targetPackage = packageIdentity;
			std::optional<PackageDependency> dependency;
			if (manifest) for (const auto& dep : manifest->dependencies) if (dep.name == parts.front()) dependency = dep;
			fs::path candidate;
			if (dependency) {
				if (parts.size() != 1) throw std::runtime_error("E0569 multi-file packages require an authenticated file inventory; only locked package.pcn is supported");
				targetBoundary = fs::weakly_canonical(boundary / "vendor" / dependency->name / dependency->version);
				if (!within(targetBoundary, boundary)) throw std::runtime_error("E0527 vendor package escapes package boundary");
				targetPackage = dependency->name + "@" + dependency->version;
				fs::path nested;
				for (std::size_t n = 1; n < parts.size(); ++n) nested /= filesystemPathFromUtf8(parts[n]);
				candidate = targetBoundary / (parts.size() == 1 ? fs::path("package.pcn") : fs::path(nested.string() + ".pcn"));
				const auto lock = readPackageLock(boundary);
				const auto pinned = std::find_if(lock.begin(), lock.end(), [&](const auto& entry) { return entry.name == dependency->name; });
				if (pinned == lock.end() || pinned->version != dependency->version || (!dependency->source.empty() && pinned->source != dependency->source))
					throw std::runtime_error("E0530 dependency requires a matching exact lock entry: " + dependency->name);
				const auto packageFile = fs::canonical(targetBoundary / "package.pcn");
				if (!within(packageFile, targetBoundary)) throw std::runtime_error("E0527 package source escapes vendor boundary");
				if (packageDigestHex(capture(packageFile)) != pinned->digest)
					throw std::runtime_error("E0530 installed dependency differs from lock: " + dependency->name);
				if (const auto nestedManifest = manifestAt(targetBoundary); nestedManifest) {
					if (nestedManifest->name != dependency->name || nestedManifest->version != dependency->version)
						throw std::runtime_error("E0530 vendored manifest identity differs from dependency");
					if (!nestedManifest->dependencies.empty()) throw std::runtime_error("E0569 vendored dependency manifests require authenticated file inventories");
				}
			}
			else {
				if (boundary != graph.projectRoot) throw std::runtime_error("E0569 vendored local imports are not covered by the package lock; declare an authenticated package dependency");
				fs::path relativePath;
				for (const auto& part : parts) relativePath /= filesystemPathFromUtf8(part);
				candidate = file.parent_path() / fs::path(relativePath.string() + ".pcn");
			}
			if (!fs::is_regular_file(candidate)) throw std::runtime_error("E0525 missing source import: " + name + " from " + graph.modules[index].identity);
			const auto target = fs::canonical(candidate);
			if (!within(target, targetBoundary)) throw std::runtime_error("E0527 canonical source import escapes package boundary: " + name);
			if (!dependency) {
				if (within(target, fs::weakly_canonical(boundary / "vendor"))) throw std::runtime_error("E0527 local import bypasses a vendor dependency: " + name);
				for (auto parent = target.parent_path(); parent != boundary; parent = parent.parent_path())
					if (fs::is_regular_file(packageManifestPath(parent))) throw std::runtime_error("E0527 local import crosses a nested package boundary: " + name);
			}
			requirePortableSpelling(candidate, targetBoundary);
			std::size_t targetIndex;
			try { targetIndex = visit(target, targetBoundary, targetPackage, std::nullopt, depth + 1); }
			catch (const std::runtime_error& error) { throw std::runtime_error(std::string(error.what()) + "\n  imported from " + graph.modules[index].identity + " at " + std::to_string(use->location.line) + ":" + std::to_string(use->location.column)); }
			graph.modules[index].imports.push_back({ targetIndex, name, use->alias, use->isPublic, use->location });
		}
		return index;
	};
	visit(rootFile, graph.projectRoot, projectManifest ? projectManifest->name + "@" + projectManifest->version : "", root, 0);
	return graph;
}

using Bindings = std::map<std::string, std::set<std::string>>;

class Rewriter {
public:
	Rewriter(const Module& module, const Bindings& bindings, const std::set<std::string>& known,
		const std::set<std::string>& typeNames, const std::set<std::string>& namespaces, std::size_t& closureId)
		: module_(module), bindings_(bindings), known_(known), typeNames_(typeNames), namespaces_(namespaces), closureId_(closureId) {}

	std::string resolve(const std::string& name, SourceLocation location, bool local = true) const {
		if (name.starts_with("__pcn_module_v1_")) fail(location, "E0532 compiler module linkage names cannot be written in source");
		if (local) for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) if (it->contains(name)) return name;
		if (const auto found = bindings_.find(name); found != bindings_.end()) {
			if (found->second.size() != 1) fail(location, "E0531 ambiguous imported name: " + name);
			return *found->second.begin();
		}
		const auto separator = name.find("::");
		if (known_.contains(name) || (separator != std::string::npos && namespaces_.contains(name.substr(0, separator))))
			fail(location, "E0529 declaration is private or not directly imported: " + name);
		return name;
	}
	std::string type(const std::string& text, SourceLocation location) const {
		if (text.empty()) return text;
		auto parsed = CanonicalTypeParser(text).parse();
		std::function<void(CanonicalTypeExpr&)> rewrite = [&](auto& node) {
			if (!node.tuple && !generics_.contains(node.head) && !isBuiltinTypeName(node.head) &&
				(typeNames_.contains(node.head) || node.head.find("::") != std::string::npos || node.head.starts_with("__pcn_module_v1_")))
				node.head = resolve(node.head, location, false);
			for (auto& arg : node.args) rewrite(arg);
		};
		rewrite(parsed); return canonicalTypeText(parsed);
	}
	Routine routine(Routine value) {
		value.name = resolve(value.name, value.location, false); value.specializationOwner = module_.identity;
		generics_.clear(); for (const auto& parameter : value.genericParameters) generics_.insert(parameter.name);
		locals_.push_back({});
		for (auto& parameter : value.parameters) { parameter.typeName = type(parameter.typeName, parameter.location); locals_.back().insert(parameter.name); }
		value.returnType = type(value.returnType, value.location);
		for (auto& constraint : value.constraints) {
			constraint.subject = type(constraint.subject, constraint.location);
			constraint.capability = resolve(constraint.capability, constraint.location, false);
			for (auto& argument : constraint.arguments) argument = type(argument, constraint.location);
		}
		value.body = statements(value.body); locals_.pop_back(); generics_.clear(); return value;
	}
	Declaration declaration(Declaration value) {
		std::visit([&](auto& node) {
			using T = std::decay_t<decltype(node)>;
			if constexpr (std::is_same_v<T, Routine>) node = routine(std::move(node));
			else if constexpr (std::is_same_v<T, ForeignDecl>) {
				for (auto& parameter : node.routine.parameters) parameter.typeName = type(parameter.typeName, parameter.location);
				node.routine.returnType = type(node.routine.returnType, node.location);
			}
			else if constexpr (!std::is_same_v<T, UseDecl> && !std::is_same_v<T, ModuleDecl>) {
				node.name = resolve(node.name, node.location, false);
				if constexpr (std::is_same_v<T, RecordDecl> || std::is_same_v<T, UnionDecl>) for (auto& field : node.fields) field.typeName = type(field.typeName, field.location);
				else if constexpr (std::is_same_v<T, VariantDecl>) for (auto& item : node.cases) for (auto& field : item.fields) field.typeName = type(field.typeName, field.location);
				else if constexpr (std::is_same_v<T, EnumDecl>) node.underlyingType = type(node.underlyingType, node.location);
				else if constexpr (std::is_same_v<T, AliasDecl>) node.targetType = type(node.targetType, node.location);
				else if constexpr (std::is_same_v<T, ConstDecl>) { node.typeName = type(node.typeName, node.location); node.value = expression(node.value); }
				else if constexpr (std::is_same_v<T, CapabilityDecl>) {
					for (const auto& parameter : node.genericParameters) generics_.insert(parameter.name);
					for (auto& member : node.members) { for (auto& parameter : member.parameters) parameter.typeName = type(parameter.typeName, parameter.location); member.returnType = type(member.returnType, member.location); }
					generics_.clear();
				}
			}
		}, value.node);
		return value;
	}
private:
	[[noreturn]] void fail(SourceLocation location, const std::string& message) const {
		throw std::runtime_error(message + " in " + module_.identity + " at " + std::to_string(location.line) + ":" + std::to_string(location.column));
	}
	ExprPtr expression(const ExprPtr& value) {
		if (!value) return {};
		return std::visit([&](const auto& original) -> ExprPtr {
			using T = std::decay_t<decltype(original)>; auto node = original;
			if constexpr (std::is_same_v<T, VariableExpr>) node.name = resolve(node.name, node.location);
			else if constexpr (std::is_same_v<T, UnaryExpr> || std::is_same_v<T, TryExpr>) node.operand = expression(node.operand);
			else if constexpr (std::is_same_v<T, BinaryExpr>) { node.left = expression(node.left); node.right = expression(node.right); }
			else if constexpr (std::is_same_v<T, PipelineExpr>) { node.input = expression(node.input); for (auto& stage : node.stages) stage = expression(stage); }
			else if constexpr (std::is_same_v<T, RangeExpr>) { node.start = expression(node.start); node.end = expression(node.end); node.step = expression(node.step); }
			else if constexpr (std::is_same_v<T, ChooseExpr>) {
				node.subject = expression(node.subject);
				for (auto& item : node.cases) { item.condition = expression(item.condition); locals_.push_back({}); for (const auto& binding : item.bindings) locals_.back().insert(binding); item.value = expression(item.value); locals_.pop_back(); }
				node.otherwiseValue = expression(node.otherwiseValue);
			}
			else if constexpr (std::is_same_v<T, CallExpr>) { node.callee = expression(node.callee); for (auto& arg : node.arguments) arg = expression(arg); for (auto& arg : node.genericArguments) arg = type(arg, node.location); }
			else if constexpr (std::is_same_v<T, ClosureExpr>) {
				node.closureId = ++closureId_; locals_.push_back({});
				for (auto& parameter : node.parameters) { parameter.typeName = type(parameter.typeName, parameter.location); locals_.back().insert(parameter.name); }
				node.returnType = type(node.returnType, node.location); node.expressionBody = expression(node.expressionBody); node.blockBody = statements(node.blockBody); locals_.pop_back();
			}
			else if constexpr (std::is_same_v<T, GroupExpr>) node.expression = expression(node.expression);
			else if constexpr (std::is_same_v<T, MemberExpr>) {
				if (const auto name = qualifiedCalleeName(value); name) {
					auto qualified = *name;
					for (std::size_t pos = 0; (pos = qualified.find('.', pos)) != std::string::npos; pos += 2) qualified.replace(pos, 1, "::");
					const auto root = qualified.substr(0, qualified.find("::"));
					bool local = false; for (const auto& scope : locals_) local = local || scope.contains(root);
					if (!local && bindings_.contains(qualified))
						return AstFactory::makeExpr(VariableExpr{ resolve(qualified, node.location), value->span, node.location }, value->span);
					if (!local && namespaces_.contains(root) && !bindings_.contains(qualified.substr(0, qualified.rfind("::"))))
						(void)resolve(qualified, node.location);
				}
				node.object = expression(node.object);
			}
			else if constexpr (std::is_same_v<T, ConstructExpr>) { node.typeName = type(node.typeName, node.location); for (auto& field : node.fields) field.value = expression(field.value); }
			else if constexpr (std::is_same_v<T, ArrayExpr>) for (auto& element : node.elements) element = expression(element);
			else if constexpr (std::is_same_v<T, IndexExpr>) { node.object = expression(node.object); node.index = expression(node.index); }
			return AstFactory::makeExpr(std::move(node), value->span);
		}, value->node);
	}
	std::vector<StmtPtr> statements(const std::vector<StmtPtr>& values) {
		locals_.push_back({}); std::vector<StmtPtr> result; result.reserve(values.size());
		for (const auto& value : values) result.push_back(std::visit([&](const auto& original) -> StmtPtr {
			using T = std::decay_t<decltype(original)>; auto node = original;
			if constexpr (std::is_same_v<T, LetStmt> || std::is_same_v<T, VarStmt>) {
				node.typeName = type(node.typeName, node.location); node.initializer = expression(node.initializer); locals_.back().insert(node.name);
			}
			else if constexpr (std::is_same_v<T, AssignStmt> || std::is_same_v<T, RecordSetStmt> || std::is_same_v<T, ArraySetStmt>) {
				node.name = resolve(node.name, node.location); node.value = expression(node.value);
				if constexpr (std::is_same_v<T, ArraySetStmt>) node.index = expression(node.index);
			}
			else if constexpr (std::is_same_v<T, ExprStmt> || std::is_same_v<T, DeferStmt> || std::is_same_v<T, FailStmt>) node.expression = expression(node.expression);
			else if constexpr (std::is_same_v<T, ReturnStmt> || std::is_same_v<T, GiveStmt>) node.value = expression(node.value);
			else if constexpr (std::is_same_v<T, IfStmt>) { node.condition = expression(node.condition); node.thenBranch = statements(node.thenBranch); node.elseBranch = statements(node.elseBranch); }
			else if constexpr (std::is_same_v<T, WhileStmt> || std::is_same_v<T, ContinueUntilStmt>) { node.condition = expression(node.condition); node.body = statements(node.body); }
			else if constexpr (std::is_same_v<T, EachStmt>) { node.range = expression(node.range); locals_.push_back({}); for (const auto& binding : node.bindings) locals_.back().insert(binding); node.guard = expression(node.guard); node.body = statements(node.body); locals_.pop_back(); }
			else if constexpr (std::is_same_v<T, FromStmt>) { node.first = expression(node.first); node.last = expression(node.last); node.typeName = type(node.typeName, node.location); locals_.push_back({ node.binding }); node.body = statements(node.body); locals_.pop_back(); }
			else if constexpr (std::is_same_v<T, UnsafeStmt>) node.body = statements(node.body);
			else if constexpr (std::is_same_v<T, BlockStmt>) node.statements = statements(node.statements);
			return AstFactory::makeStmt(std::move(node), value->span);
		}, value->node));
		locals_.pop_back(); return result;
	}
	const Module& module_;
	const Bindings& bindings_;
	const std::set<std::string>& known_;
	const std::set<std::string>& typeNames_;
	const std::set<std::string>& namespaces_;
	std::size_t& closureId_;
	std::vector<std::set<std::string>> locals_;
	std::set<std::string> generics_;
};

inline void diagnosticContext(const Graph& graph, Program& result) {
	for (const auto& module : graph.modules) result.sourceFiles.emplace(utf8Path(module.file), Lexer::normalizeSource(module.source));
	if (graph.modules.empty()) return;
	std::set<std::size_t> visited{ 0 };
	std::vector<std::size_t> queue{ 0 };
	for (std::size_t at = 0; at < queue.size(); ++at) {
		const auto index = queue[at];
		for (const auto& import : graph.modules[index].imports) if (visited.insert(import.target).second) {
			auto chain = result.importChains[utf8Path(graph.modules[index].file)];
			chain.emplace_back(import.location, "import " + import.spelling);
			result.importChains.emplace(utf8Path(graph.modules[import.target].file), std::move(chain)); queue.push_back(import.target);
		}
	}
}

inline Program lower(const Graph& graph) {
	const auto size = graph.modules.size();
	std::vector<Bindings> own(size), exports(size);
	std::set<std::string> known, typeNames, namespaces;
	for (std::size_t index = 0; index < size; ++index) {
		const auto& module = graph.modules[index];
		auto add = [&](const std::string& name, bool isPublic, bool foreign = false) {
			if (name.starts_with("__pcn_module_v1_")) throw std::runtime_error("E0532 reserved module linkage prefix: " + name);
			const auto linked = index == 0 || foreign ? name : symbol(module.identity, name);
			own[index][name].insert(linked); known.insert(name);
			if (isPublic || !module.explicitIdentity) exports[index][name].insert(linked);
		};
		for (const auto& routine : module.program.routines) add(routine.name, routine.isPublic);
		for (const auto& declaration : module.program.declarations) std::visit([&](const auto& node) {
			using T = std::decay_t<decltype(node)>;
			if constexpr (std::is_same_v<T, ForeignDecl>) add(node.routine.name, node.routine.isPublic, true);
			else if constexpr (!std::is_same_v<T, UseDecl> && !std::is_same_v<T, ModuleDecl>) {
				add(node.name, node.isPublic);
				if constexpr (!std::is_same_v<T, Routine>) typeNames.insert(node.name);
			}
		}, declaration.node);
		for (const auto& import : module.imports) {
			const auto qualifier = import.alias.empty() ? import.spelling : import.alias;
			namespaces.insert(qualifier.substr(0, qualifier.find("::")));
		}
	}
	bool changed = true;
	while (changed) {
		changed = false;
		for (std::size_t index = 0; index < size; ++index) for (const auto& import : graph.modules[index].imports) if (import.reexport) {
			if (!import.alias.empty()) throw std::runtime_error("E0521 re-export aliases are not supported; re-export the module without an alias");
			for (const auto& [name, targets] : exports[import.target]) if (!own[index].contains(name)) {
				auto& target = exports[index][name]; const auto before = target.size(); target.insert(targets.begin(), targets.end()); changed = changed || before != target.size();
			}
		}
	}
	std::vector<std::size_t> order(size); std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin() + 1, order.end(), [&](auto a, auto b) { return graph.modules[a].identity < graph.modules[b].identity; });
	Program result; std::size_t closureId = 0;
	diagnosticContext(graph, result);
	for (auto index : order) {
		const auto& module = graph.modules[index]; Bindings visible = own[index];
		std::map<std::string, std::size_t> namespaceTargets;
		for (const auto& import : module.imports) {
			const auto qualifier = import.alias.empty() ? import.spelling : import.alias;
			const auto [found, inserted] = namespaceTargets.emplace(qualifier, import.target);
			if ((!inserted && found->second != import.target) || own[index].contains(qualifier))
				throw std::runtime_error("E0531 import namespace conflicts with a declaration or module: " + qualifier + " in " + module.identity);
		}
		auto checkInterfaceType = [&](const std::string& text, SourceLocation location, const std::set<std::string>& generics = {}) {
			if (text.empty() || !module.explicitIdentity) return;
			const auto parsed = CanonicalTypeParser(text).parse();
			std::function<void(const CanonicalTypeExpr&)> check = [&](const auto& node) {
				if (!node.tuple && !generics.contains(node.head) && !isBuiltinTypeName(node.head) && typeNames.contains(node.head) && own[index].contains(node.head) && !exports[index].contains(node.head))
					throw std::runtime_error("E0534 public interface exposes private type: " + node.head + " in " + module.identity + " at " + std::to_string(location.line) + ":" + std::to_string(location.column));
				for (const auto& argument : node.args) check(argument);
			};
			check(parsed);
		};
		auto checkRoutine = [&](const Routine& routine) {
			if (!routine.isPublic) return;
			std::set<std::string> generics;
			for (const auto& parameter : routine.genericParameters) generics.insert(parameter.name);
			checkInterfaceType(routine.returnType, routine.location, generics);
			for (const auto& parameter : routine.parameters) checkInterfaceType(parameter.typeName, parameter.location, generics);
			for (const auto& constraint : routine.constraints) checkInterfaceType(constraint.capability, constraint.location, generics);
		};
		for (const auto& routine : module.program.routines) checkRoutine(routine);
		for (const auto& declaration : module.program.declarations) std::visit([&](const auto& node) {
			using T = std::decay_t<decltype(node)>;
			if constexpr (std::is_same_v<T, Routine>) checkRoutine(node);
			else if constexpr (std::is_same_v<T, ForeignDecl>) checkRoutine(node.routine);
			else if constexpr (std::is_same_v<T, RecordDecl> || std::is_same_v<T, UnionDecl>) { if (node.isPublic) for (const auto& field : node.fields) checkInterfaceType(field.typeName, field.location); }
			else if constexpr (std::is_same_v<T, VariantDecl>) { if (node.isPublic) for (const auto& item : node.cases) for (const auto& field : item.fields) checkInterfaceType(field.typeName, field.location); }
			else if constexpr (std::is_same_v<T, AliasDecl>) { if (node.isPublic) checkInterfaceType(node.targetType, node.location); }
			else if constexpr (std::is_same_v<T, ConstDecl>) { if (node.isPublic) checkInterfaceType(node.typeName, node.location); }
		}, declaration.node);
		for (const auto& import : module.imports) for (const auto& [name, targets] : exports[import.target]) {
			const auto qualifier = import.alias.empty() ? import.spelling : import.alias;
			visible[qualifier + "::" + name].insert(targets.begin(), targets.end());
			if (import.alias.empty() && !own[index].contains(name)) visible[name].insert(targets.begin(), targets.end());
		}
		Rewriter rewriter(module, visible, known, typeNames, namespaces, closureId);
		for (const auto& routine : module.program.routines) result.routines.push_back(rewriter.routine(routine));
		for (const auto& declaration : module.program.declarations) {
			if (std::holds_alternative<UseDecl>(declaration.node)) continue;
			if (std::holds_alternative<ModuleDecl>(declaration.node)) { if (index == 0) result.declarations.push_back(declaration); continue; }
			result.declarations.push_back(rewriter.declaration(declaration));
		}
	}
	return result;
}
} // namespace source_modules
