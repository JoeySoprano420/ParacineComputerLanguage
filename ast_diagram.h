// Included inside namespace pcn after the AST definitions.
struct AstDiagramNode {
	std::string role;
	std::string kind;
	std::string detail;
	std::optional<SourceLocation> location;
	std::vector<AstDiagramNode> children;
};

class AstDiagramBuilder {
	template <typename T> static constexpr bool unhandled = false;

	static AstDiagramNode node(std::string role, std::string kind, std::string detail = {},
							   std::optional<SourceLocation> location = std::nullopt) {
		return { std::move(role), std::move(kind), std::move(detail), location, {} };
	}
	static void attribute(AstDiagramNode& parent, const std::string& role, const std::string& value) {
		if (!value.empty()) parent.children.push_back(node(role, "attribute", value));
	}
	static void flag(AstDiagramNode& parent, const std::string& role, bool value) {
		if (value) attribute(parent, role, "true");
	}
	static void parameter(AstDiagramNode& parent, const std::string& role, const Parameter& p) {
		auto child = node(role, "parameter", p.name, p.location);
		attribute(child, "type", p.typeName);
		attribute(child, "mode", p.mode);
		parent.children.push_back(std::move(child));
	}
	static void generic(AstDiagramNode& parent, const GenericParameter& p) {
		auto child = node("generic parameter", "parameter", p.name, p.location);
		flag(child, "const", p.isConst);
		attribute(child, "type", p.constType);
		parent.children.push_back(std::move(child));
	}
	static void expr(AstDiagramNode& parent, const std::string& role, const ExprPtr& value) {
		if (value) parent.children.push_back(expression(role, *value));
	}
	static void statements(AstDiagramNode& parent, const std::string& role, const std::vector<StmtPtr>& body) {
		auto block = node(role, "block");
		for (const auto& stmt : body) if (stmt) block.children.push_back(statement("statement", *stmt));
		parent.children.push_back(std::move(block));
	}
	static AstDiagramNode expression(const std::string& role, const Expr& e) {
		return std::visit([&](const auto& x) -> AstDiagramNode {
			using T = std::decay_t<decltype(x)>;
			auto n = node(role, "expression", {}, e.location);
			if constexpr (std::is_same_v<T, NumberExpr>) { n.kind = "number"; n.detail = x.value; }
			else if constexpr (std::is_same_v<T, CharExpr>) { n.kind = "character"; n.detail = std::to_string(x.codepoint); }
			else if constexpr (std::is_same_v<T, StringExpr>) { n.kind = "text"; n.detail = x.value; }
			else if constexpr (std::is_same_v<T, BoolExpr>) { n.kind = "boolean"; n.detail = x.value ? "true" : "false"; }
			else if constexpr (std::is_same_v<T, NullExpr>) { n.kind = "null"; }
			else if constexpr (std::is_same_v<T, VariableExpr>) { n.kind = "variable"; n.detail = x.name; }
			else if constexpr (std::is_same_v<T, UnaryExpr>) { n.kind = "unary"; n.detail = x.op; expr(n, "operand", x.operand); }
			else if constexpr (std::is_same_v<T, BinaryExpr>) { n.kind = "binary"; n.detail = x.op; expr(n, "left operand", x.left); expr(n, "right operand", x.right); }
			else if constexpr (std::is_same_v<T, PipelineExpr>) { n.kind = "pipeline"; expr(n, "input", x.input); for (const auto& stage : x.stages) expr(n, "stage", stage); }
			else if constexpr (std::is_same_v<T, RangeExpr>) { n.kind = "range"; n.detail = x.inclusive ? "inclusive" : "exclusive"; expr(n, "start", x.start); expr(n, "end", x.end); expr(n, "step", x.step); }
			else if constexpr (std::is_same_v<T, ChooseExpr>) {
				n.kind = "choose"; expr(n, "subject", x.subject);
				for (const auto& c : x.cases) { auto item = node("case", "case", c.variantName, c.location); for (const auto& b : c.bindings) attribute(item, "binding", b); expr(item, "condition", c.condition); expr(item, "value", c.value); n.children.push_back(std::move(item)); }
				if (x.hasOtherwise) expr(n, "otherwise", x.otherwiseValue);
			}
			else if constexpr (std::is_same_v<T, CallExpr>) { n.kind = "call"; expr(n, "callee", x.callee); for (const auto& arg : x.genericArguments) attribute(n, "type argument", arg); for (const auto& arg : x.arguments) expr(n, "argument", arg); }
			else if constexpr (std::is_same_v<T, TryExpr>) { n.kind = "try"; expr(n, "operand", x.operand); }
			else if constexpr (std::is_same_v<T, ClosureExpr>) {
				n.kind = "closure"; flag(n, "move capture", x.moveCapture); for (const auto& p : x.parameters) parameter(n, "parameter", p);
				attribute(n, "return type", x.returnType); for (const auto& effect : x.effects) attribute(n, "effect", effect);
				if (x.hasExpressionBody) expr(n, "body", x.expressionBody); else statements(n, "body", x.blockBody);
			}
			else if constexpr (std::is_same_v<T, GroupExpr>) { n.kind = "group"; expr(n, "value", x.expression); }
			else if constexpr (std::is_same_v<T, MemberExpr>) { n.kind = "member"; n.detail = x.member; expr(n, "object", x.object); }
			else if constexpr (std::is_same_v<T, ArrayExpr>) { n.kind = "array"; for (const auto& item : x.elements) expr(n, "element", item); }
			else if constexpr (std::is_same_v<T, IndexExpr>) { n.kind = "index"; expr(n, "object", x.object); expr(n, "index", x.index); }
			else if constexpr (std::is_same_v<T, ConstructExpr>) { n.kind = "construct"; n.detail = x.typeName; for (const auto& f : x.fields) { auto field = node("field", "field", f.name, f.location); expr(field, "value", f.value); n.children.push_back(std::move(field)); } }
			else static_assert(unhandled<T>, "Unhandled AST expression");
			return n;
		}, e.node);
	}
	static AstDiagramNode statement(const std::string& role, const Stmt& s) {
		return std::visit([&](const auto& x) -> AstDiagramNode {
			using T = std::decay_t<decltype(x)>;
			auto n = node(role, "statement", {}, s.location);
			if constexpr (std::is_same_v<T, LetStmt> || std::is_same_v<T, VarStmt>) { n.kind = std::is_same_v<T, LetStmt> ? "let" : "var"; n.detail = x.name; attribute(n, "type", x.typeName); expr(n, "initializer", x.initializer); }
			else if constexpr (std::is_same_v<T, AssignStmt>) { n.kind = "assign"; n.detail = x.name; expr(n, "value", x.value); }
			else if constexpr (std::is_same_v<T, ArraySetStmt>) { n.kind = "array assignment"; n.detail = x.name; expr(n, "index", x.index); expr(n, "value", x.value); }
			else if constexpr (std::is_same_v<T, RecordSetStmt>) { n.kind = "field assignment"; n.detail = x.name + "." + x.field; expr(n, "value", x.value); }
			else if constexpr (std::is_same_v<T, ExprStmt>) { n.kind = "expression statement"; expr(n, "expression", x.expression); }
			else if constexpr (std::is_same_v<T, ReturnStmt>) { n.kind = "return"; if (x.hasValue) expr(n, "value", x.value); }
			else if constexpr (std::is_same_v<T, GiveStmt>) { n.kind = "give"; expr(n, "value", x.value); }
			else if constexpr (std::is_same_v<T, IfStmt>) { n.kind = "if"; expr(n, "condition", x.condition); statements(n, "then", x.thenBranch); if (x.hasElse) statements(n, "else", x.elseBranch); }
			else if constexpr (std::is_same_v<T, EachStmt>) { n.kind = "for"; attribute(n, "label", x.loopLabel); flag(n, "mutable bindings", x.bindingsMutable); for (const auto& b : x.bindings) attribute(n, "binding", b); expr(n, "range", x.range); expr(n, "guard", x.guard); statements(n, "body", x.body); }
			else if constexpr (std::is_same_v<T, FromStmt>) { n.kind = "from"; n.detail = x.binding; attribute(n, "label", x.loopLabel); attribute(n, "type", x.typeName); expr(n, "first", x.first); expr(n, "last", x.last); statements(n, "body", x.body); }
			else if constexpr (std::is_same_v<T, ContinueUntilStmt>) { n.kind = "continue until"; attribute(n, "label", x.loopLabel); expr(n, "condition", x.condition); statements(n, "body", x.body); }
			else if constexpr (std::is_same_v<T, UnsafeStmt>) { n.kind = "unsafe"; statements(n, "body", x.body); }
			else if constexpr (std::is_same_v<T, DeferStmt> || std::is_same_v<T, FailStmt>) { n.kind = std::is_same_v<T, DeferStmt> ? "defer" : "fail"; expr(n, "expression", x.expression); }
			else if constexpr (std::is_same_v<T, WhileStmt>) { n.kind = "while"; attribute(n, "label", x.loopLabel); expr(n, "condition", x.condition); statements(n, "body", x.body); }
			else if constexpr (std::is_same_v<T, BreakStmt> || std::is_same_v<T, ContinueStmt>) { n.kind = std::is_same_v<T, BreakStmt> ? "break" : "continue"; attribute(n, "label", x.label); }
			else if constexpr (std::is_same_v<T, BlockStmt>) { n.kind = "block"; statements(n, "body", x.statements); }
			else static_assert(unhandled<T>, "Unhandled AST statement");
			return n;
		}, s.node);
	}
	static AstDiagramNode routine(const std::string& role, const Routine& r) {
		auto n = node(role, "routine", r.name, r.location);
		flag(n, "public", r.isPublic); flag(n, "compile", r.isCompileTime); flag(n, "foreign", r.isForeign); attribute(n, "ABI", r.foreignAbi);
		for (const auto& g : r.genericParameters) generic(n, g);
		for (const auto& p : r.parameters) parameter(n, "parameter", p);
		attribute(n, "return type", r.returnType);
		for (const auto& c : r.constraints) { auto item = node("constraint", "capability", c.capability, c.location); attribute(item, "subject", c.subject); for (const auto& arg : c.arguments) attribute(item, "argument", arg); n.children.push_back(std::move(item)); }
		for (const auto& effect : r.effects) attribute(n, "effect", effect);
		if (!r.isForeign) statements(n, "body", r.body);
		return n;
	}
	static AstDiagramNode declaration(const Declaration& d) {
		return std::visit([&](const auto& x) -> AstDiagramNode {
			using T = std::decay_t<decltype(x)>;
			if constexpr (std::is_same_v<T, Routine>) return routine("declaration", x);
			else if constexpr (std::is_same_v<T, ForeignDecl>) { auto n = routine("declaration", x.routine); n.kind = "foreign routine"; attribute(n, "declaration ABI", x.abi); return n; }
			else {
				auto n = node("declaration", "declaration", {}, d.location);
				if constexpr (std::is_same_v<T, RecordDecl> || std::is_same_v<T, UnionDecl>) {
					n.kind = std::is_same_v<T, RecordDecl> ? "record" : "union"; n.detail = x.name; flag(n, "public", x.isPublic); attribute(n, "layout", x.layout);
					if constexpr (std::is_same_v<T, RecordDecl>) flag(n, "compile", x.isCompileTime);
					for (const auto& f : x.fields) { auto field = node("field", "field", f.name, f.location); attribute(field, "type", f.typeName); n.children.push_back(std::move(field)); }
				}
				else if constexpr (std::is_same_v<T, EnumDecl>) { n.kind = "enum"; n.detail = x.name; flag(n, "public", x.isPublic); attribute(n, "underlying type", x.underlyingType); for (const auto& m : x.members) { auto member = node("member", "member", m.first); if (m.second) attribute(member, "value", *m.second); n.children.push_back(std::move(member)); } }
				else if constexpr (std::is_same_v<T, VariantDecl>) { n.kind = "variant"; n.detail = x.name; flag(n, "public", x.isPublic); for (const auto& c : x.cases) { auto item = node("case", "case", c.name, c.location); for (const auto& p : c.fields) parameter(item, "payload", p); n.children.push_back(std::move(item)); } }
				else if constexpr (std::is_same_v<T, ModuleDecl>) { n.kind = "module"; n.detail = x.name; flag(n, "public", x.isPublic); for (const auto& child : x.declarations) n.children.push_back(declaration(child)); }
				else if constexpr (std::is_same_v<T, UseDecl>) { n.kind = "import"; for (const auto& part : x.path) attribute(n, "path", part); attribute(n, "alias", x.alias); }
				else if constexpr (std::is_same_v<T, AliasDecl>) { n.kind = "alias"; n.detail = x.name; flag(n, "public", x.isPublic); attribute(n, "target", x.targetType); }
				else if constexpr (std::is_same_v<T, ConstDecl>) { n.kind = "const"; n.detail = x.name; flag(n, "public", x.isPublic); flag(n, "compile", x.isCompileTime); attribute(n, "type", x.typeName); expr(n, "value", x.value); }
				else if constexpr (std::is_same_v<T, CapabilityDecl>) { n.kind = "capability"; n.detail = x.name; flag(n, "public", x.isPublic); for (const auto& g : x.genericParameters) generic(n, g); for (const auto& m : x.members) { auto member = node("member", "routine signature", m.name, m.location); for (const auto& p : m.parameters) parameter(member, "parameter", p); attribute(member, "return type", m.returnType); for (const auto& effect : m.effects) attribute(member, "effect", effect); n.children.push_back(std::move(member)); } }
				else static_assert(unhandled<T>, "Unhandled AST declaration");
				return n;
			}
		}, d.node);
	}
public:
	static AstDiagramNode build(const Program& p) {
		auto root = node("root", "program");
		for (const auto& d : p.declarations) root.children.push_back(declaration(d));
		for (const auto& r : p.routines) root.children.push_back(routine("routine", r));
		std::stable_sort(root.children.begin(), root.children.end(), [](const auto& a, const auto& b) {
			if (!a.location || !b.location) return bool(a.location) > bool(b.location);
			return std::tie(a.location->line, a.location->column) < std::tie(b.location->line, b.location->column);
		});
		return root;
	}
};

static std::string astDiagramQuote(const std::string& text) {
	std::ostringstream out;
	out << '"';
	for (unsigned char c : text) {
		if (c == '\\' || c == '"') out << '\\' << static_cast<char>(c);
		else if (c == '\n') out << "\\n";
		else if (c == '\r') out << "\\r";
		else if (c == '\t') out << "\\t";
		else if (c < 32 || c == 127) { const char* hex = "0123456789ABCDEF"; out << "\\x" << hex[c >> 4] << hex[c & 15]; }
		else out << static_cast<char>(c);
	}
	out << '"';
	return out.str();
}

static std::string astDiagram(const Program& program) {
	const auto root = AstDiagramBuilder::build(program);
	std::ostringstream tree;
	std::vector<std::vector<std::size_t>> edges;
	std::function<std::size_t(const AstDiagramNode&, const std::string&, bool, bool)> render =
		[&](const AstDiagramNode& n, const std::string& prefix, bool last, bool isRoot) {
			const auto id = edges.size(); edges.emplace_back();
			if (!isRoot) tree << prefix << (last ? "`-- " : "|-- ");
			tree << "#" << id << " " << n.role << ": " << n.kind;
			if (!n.detail.empty()) tree << " " << astDiagramQuote(n.detail);
			if (n.location) tree << " @" << n.location->line << ":" << n.location->column;
			tree << "\n";
			const auto next = prefix + (isRoot ? "" : last ? "    " : "|   ");
			for (std::size_t i = 0; i < n.children.size(); ++i) {
				const auto child = render(n.children[i], next, i + 1 == n.children.size(), false);
				edges[id].push_back(child);
			}
			return id;
		};
	render(root, "", true, true);
	std::ostringstream out;
	out << "AST diagram (parsed, before semantic analysis)\n" << tree.str()
		<< "\nSyntax dependency lattice\n"
		<< "Elements: descendant-closed sets of diagram node IDs; order: subset.\n"
		<< "bottom = {}; top = D(#0); meet = intersection; join = union.\n"
		<< "D(#n) = {#n} union the D sets of its immediate children.\n";
	for (std::size_t i = 0; i < edges.size(); ++i) {
		out << "D(#" << i << ") = {#" << i << "}";
		for (const auto child : edges[i]) out << " union D(#" << child << ")";
		out << "\n";
	}
	return out.str();
}
