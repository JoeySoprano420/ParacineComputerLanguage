#pragma once

// Included after the process runner, inside namespace pcn.
namespace native_debug {
using unix_output::Bytes;
struct Data : Bytes {
	void u(std::uint64_t value, unsigned width) { integer(value, width); }
	void leb(std::uint64_t value) {
		do { const auto byte = static_cast<std::uint8_t>(value & 127); value >>= 7; u(byte | (value ? 128 : 0), 1); } while (value);
	}
	void sleb(std::int64_t value) {
		bool more;
		do { auto byte = static_cast<std::uint8_t>(value & 127); value >>= 7; more = !((value == 0 && !(byte & 64)) || (value == -1 && (byte & 64))); u(byte | (more ? 128 : 0), 1); } while (more);
	}
	void append(const std::vector<std::uint8_t>& bytes) { data.insert(data.end(), bytes.begin(), bytes.end()); }
	void text(const std::string& value) { string(value); }
};
struct Relocation { std::size_t offset; std::string symbol; unsigned kind; std::int64_t addend = 0; };
struct Section {
	std::string name;
	Data bytes;
	std::vector<Relocation> relocations;
	std::uint32_t flags = 0;
};
struct Symbol { std::string name; unsigned section = 0; std::uint32_t offset = 0, size = 0; bool function = false; };
struct Row { std::uint32_t address, size; SourceLocation location; };
struct Model {
	const CompilationArtifacts& artifacts;
	const CompilerOptions& options;
	RichABIResolver abi;
	std::vector<std::string> files;
	std::map<std::string, unsigned> fileIds;
	std::map<std::string, std::array<std::uint8_t, 32>> sourceHashes;
	std::vector<Row> rows;
	std::map<std::uint32_t, const PMIRFunction*> functions;
	Model(const CompilationArtifacts& input, const CompilerOptions& settings) : artifacts(input), options(settings), abi(input.pcir) {
		const auto errors = MachineVerifier{}.verify(input.machine);
		if (!errors.empty()) throw std::runtime_error("E0580 debug metadata requires verified machine code: " + errors.front());
		for (const auto& [file, source] : input.program.sourceFiles) {
			if (file.empty()) continue;
			const auto original = readSourceFile(filesystemPathFromUtf8(file));
			if (Lexer::normalizeSource(original) != source) throw std::runtime_error("E0580 source changed while generating debug information: " + file);
			sourceHashes[file] = PackageSHA256::digest(original);
			fileIds[file] = static_cast<unsigned>(files.size() + 1); files.push_back(file);
		}
		if (files.empty()) throw std::runtime_error("E0580 debug metadata requires named source snapshots");
		for (const auto& fn : input.pmir.functions) functions.emplace(input.machine.functionOffsets.at(fn.name), &fn);
		for (const auto& line : input.machine.listing) {
			if (line.bytes.empty() || line.sourceSpans.empty()) continue;
			const auto& location = line.sourceSpans.front().start;
			if (!fileIds.contains(location.file)) throw std::runtime_error("E0580 machine source location has no source snapshot");
			if (!location.line || location.line > 0xffffff || !location.column || location.column > UINT16_MAX)
				throw std::runtime_error("E0580 source coordinate exceeds debug format limits");
			rows.push_back({ static_cast<std::uint32_t>(line.offset), static_cast<std::uint32_t>(line.bytes.size()), location });
		}
	}
	const IRFunction& source(const std::string& name) const {
		for (const auto& fn : artifacts.pcir.functions) if (fn.name == name) return fn;
		throw std::runtime_error("E0580 missing source function: " + name);
	}
	unsigned file(const SourceLocation& location) const {
		auto found = fileIds.find(location.file);
		if (found == fileIds.end()) throw std::runtime_error("E0580 declaration has no source snapshot");
		return found->second;
	}
	std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges(const PMIRFunction& fn, const DebugVariable& variable) const {
		std::vector<std::pair<std::uint32_t, std::uint32_t>> result;
		const auto slot = fn.slots.find(static_cast<std::uint32_t>(variable.value));
		if (options.optimizationLevel != 0 || slot == fn.slots.end() || slot->second.rich) return result;
		if (variable.parameter) {
			const auto parameter = std::find(fn.parameterSlots.begin(), fn.parameterSlots.end(), static_cast<std::uint32_t>(variable.value));
			if (parameter == fn.parameterSlots.end() || fn.parameterModes.at(static_cast<std::size_t>(parameter - fn.parameterSlots.begin())) != "shared") return result;
		}
		const auto begin = artifacts.machine.functionOffsets.at(fn.name), end = begin + artifacts.machine.functionSizes.at(fn.name);
		for (const auto& row : rows) {
			if (row.address < begin || row.address >= end) continue;
			if (!variable.parameter) {
				const auto& point = row.location;
				if (point.file != variable.declaration.start.file ||
					std::tie(point.line, point.column) < std::tie(variable.declaration.end.line, variable.declaration.end.column) ||
					std::tie(point.line, point.column) >= std::tie(variable.scope.end.line, variable.scope.end.column)) continue;
			}
			// Return epilogues restore RBP. Never advertise its old frame as a variable location there.
			std::uint32_t validEnd = row.address + row.size;
			const auto& text = artifacts.machine.text;
			if (row.size >= 9 && text[validEnd - 9] == 0x48 && text[validEnd - 8] == 0x81 && text[validEnd - 7] == 0xc4 && text[validEnd - 2] == 0x5d && text[validEnd - 1] == 0xc3)
				validEnd -= 9;
			if (row.address == validEnd) continue;
			if (!result.empty() && result.back().second == row.address) result.back().second = validEnd;
			else result.emplace_back(row.address, validEnd);
		}
		return result;
	}
};
inline std::vector<ABIFieldLayout> fields(const ABITypeLayout& layout) {
	auto result = layout.fields;
	if (layout.kind == ABIRepresentationKind::OptionTagged) result = { { "$tag", "ubyte", layout.tagOffset, false }, { "value", layout.valueType, layout.payloadOffset, false } };
	else if (layout.kind == ABIRepresentationKind::Result) result = { { "$tag", "ubyte", layout.tagOffset, false }, { "ok", layout.okType, layout.payloadOffset, false }, { "error", layout.errType, layout.payloadOffset, false } };
	else if (layout.kind == ABIRepresentationKind::OptionPointerNiche) return { { "value", layout.valueType, 0, false } };
	return result;
}
inline std::vector<Symbol> symbols(const Model& model, const std::vector<Section>& sections) {
	std::vector<Symbol> result;
	for (unsigned n = 0; n < sections.size(); ++n) result.push_back({ sections[n].name, n + 1 });
	const auto& machine = model.artifacts.machine;
	for (const auto& [name, offset] : std::map<std::string, std::uint32_t>(machine.functionOffsets.begin(), machine.functionOffsets.end()))
		result.push_back({ name, 1, offset, machine.functionSizes.at(name), true });
	std::set<std::string> undefined;
	for (const auto& call : machine.externalCalls) undefined.insert(call.symbol);
	for (const auto& call : machine.imports) undefined.insert("__imp_" + call.symbol);
	for (const auto& name : undefined) result.push_back({ name });
	return result;
}
inline Section textSection(const Model& model, bool coff) {
	Section text{ ".text" }; text.flags = 0x60500020; text.bytes.data = model.artifacts.machine.text;
	for (const auto& call : model.artifacts.machine.externalCalls) text.relocations.push_back({ call.dispPos, call.symbol, 4, coff ? 0 : -4 });
	for (const auto& call : model.artifacts.machine.imports) text.relocations.push_back({ call.dispPos, "__imp_" + call.symbol, 4 });
	return text;
}
inline Section abiSection(const Model& model) {
	Section section{ ".pcnabi" }; section.flags = 0x40000040;
	const auto text = native_abi::manifest(model.artifacts.pcir, model.artifacts.machine.systemV);
	section.bytes.data.assign(text.begin(), text.end()); return section;
}
inline std::vector<std::uint8_t> coff(const Model& model, std::vector<Section> sections) {
	const auto inventory = symbols(model, sections);
	std::map<std::string, std::uint32_t> indexes;
	for (std::uint32_t n = 0; n < inventory.size(); ++n) indexes.emplace(inventory[n].name, n);
	Data out, strings; strings.u(0, 4);
	out.u(0x8664, 2); out.u(sections.size(), 2); out.u(0, 4); out.u(0, 4); out.u(inventory.size(), 4); out.u(0, 4);
	const auto header = out.data.size(); out.data.resize(header + sections.size() * 40);
	for (std::size_t n = 0; n < sections.size(); ++n) {
		auto& section = sections[n];
		if (section.name.size() > 8 || section.relocations.size() > UINT16_MAX) throw std::runtime_error("E0580 COFF debug section exceeds format limits");
		const auto h = header + n * 40;
		std::copy(section.name.begin(), section.name.end(), out.data.begin() + static_cast<std::ptrdiff_t>(h));
		out.align(4); out.patch(h + 16, section.bytes.data.size(), 4); out.patch(h + 20, out.data.size(), 4); out.append(section.bytes.data);
		if (!section.relocations.empty()) {
			out.align(4); out.patch(h + 24, out.data.size(), 4); out.patch(h + 32, section.relocations.size(), 2);
			for (const auto& relocation : section.relocations) {
				if (relocation.offset > section.bytes.data.size() || section.bytes.data.size() - relocation.offset < (relocation.kind == 0xa ? 2u : 4u))
					throw std::runtime_error("E0580 COFF debug relocation outside section");
				out.u(relocation.offset, 4); out.u(indexes.at(relocation.symbol), 4); out.u(relocation.kind, 2);
			}
		}
		out.patch(h + 36, section.flags, 4);
	}
	out.align(4); out.patch(8, out.data.size(), 4);
	for (const auto& symbol : inventory) {
		if (symbol.name.size() <= 8) out.name(symbol.name, 8);
		else { out.u(0, 4); out.u(strings.string(symbol.name), 4); }
		out.u(symbol.offset, 4); out.u(symbol.section, 2); out.u(symbol.function ? 0x20 : 0, 2); out.u(symbol.function || !symbol.section ? 2 : 3, 1); out.u(0, 1);
	}
	strings.patch(0, strings.data.size(), 4); out.append(strings.data); return out.data;
}
inline std::size_t cvRecord(Data& stream, unsigned kind, Data payload, bool type = false) {
	Data record; record.u(kind, 2); record.append(payload.data);
	while ((record.data.size() + 2) % 4) record.u(type ? 0xf0 + (4 - (record.data.size() + 2) % 4) : 0, 1);
	if (record.data.size() > UINT16_MAX) throw std::runtime_error("E0580 CodeView record exceeds 64 KiB");
	const auto start = stream.data.size(); stream.u(record.data.size(), 2); stream.append(record.data); return start;
}
inline void cvNumeric(Data& data, std::uint64_t value) {
	if (value < 0x8000) data.u(value, 2);
	else if (value <= UINT32_MAX) { data.u(0x8004, 2); data.u(value, 4); }
	else { data.u(0x800a, 2); data.u(value, 8); }
}
class CVTypes {
	const RichABIResolver& abi_;
	std::map<std::string, std::uint32_t> types_;
	std::uint32_t next_ = 0x1000;
public:
	Data stream;
	explicit CVTypes(const RichABIResolver& abi) : abi_(abi) { stream.u(4, 4); }
	std::uint32_t record(unsigned kind, Data payload) { auto id = next_++; cvRecord(stream, kind, std::move(payload), true); return id; }
	std::uint32_t type(const std::string& name) {
		if (auto found = types_.find(name); found != types_.end()) return found->second;
		const auto layout = abi_.layout(name);
		std::uint32_t id;
		if (name == "void" || name == "never") id = 3;
		else if (name == "bool" || layout.kind == ABIRepresentationKind::OptionBoolNiche) id = 0x30;
		else if (name == "float") id = 0x40;
		else if (name == "double") id = 0x41;
		else if (name == "char" || layout.kind == ABIRepresentationKind::OptionCharNiche) id = 0x75;
		else if (name == "raw" || name.rfind("raw<", 0) == 0) id = 0x0603;
		else if (!layout.rich) {
			if (layout.size == 1) id = layout.signedScalar ? 0x68 : 0x69;
			else if (layout.size == 2) id = layout.signedScalar ? 0x72 : 0x73;
			else if (layout.size == 4) id = layout.signedScalar ? 0x74 : 0x75;
			else id = layout.signedScalar ? 0x13 : 0x23;
		}
		else {
			std::string element; std::size_t count = 0;
			if (typeIsArray(name, &element, &count)) {
				Data array; array.u(type(element), 4); array.u(0x23, 4); cvNumeric(array, layout.size); array.text(name); id = record(0x1503, std::move(array));
			}
			else {
				auto members = fields(layout); Data list;
				for (const auto& member : members) {
					const auto memberType = member.byReference ? 0x0603 : type(member.typeName);
					list.u(0x150d, 2); list.u(3, 2); list.u(memberType, 4); cvNumeric(list, member.offset); list.text(member.name);
					while (list.data.size() % 4) list.u(0xf0 + (4 - list.data.size() % 4), 1);
				}
				const auto fieldList = record(0x1203, std::move(list));
				Data structure; structure.u(members.size(), 2); structure.u(0, 2); structure.u(fieldList, 4); structure.u(0, 4); structure.u(0, 4); cvNumeric(structure, layout.size); structure.text(name);
				id = record(0x1505, std::move(structure));
			}
		}
		types_[name] = id; return id;
	}
	std::uint32_t procedure(const IRFunction& fn) {
		Data arguments; arguments.u(fn.parameterTypes.size(), 4);
		for (const auto& parameter : fn.parameterTypes) arguments.u(type(parameter), 4);
		const auto argumentList = record(0x1201, std::move(arguments));
		Data procedure; procedure.u(type(fn.returnType), 4); procedure.u(0, 1); procedure.u(0, 1); procedure.u(fn.parameterTypes.size(), 2); procedure.u(argumentList, 4);
		return record(0x1008, std::move(procedure));
	}
};
inline void cvSubsection(Section& section, unsigned kind, const Data& content, const std::vector<Relocation>& relocations = {}) {
	section.bytes.align(4); section.bytes.u(kind, 4); section.bytes.u(content.data.size(), 4);
	const auto base = section.bytes.data.size(); section.bytes.append(content.data);
	for (auto relocation : relocations) { relocation.offset += base; section.relocations.push_back(std::move(relocation)); }
	section.bytes.align(4);
}
inline void cvAddress(Data& data, std::vector<Relocation>& relocations, std::uint32_t offset, const std::string& symbol) {
	relocations.push_back({ data.data.size(), symbol, 0xb }); data.u(offset, 4);
	relocations.push_back({ data.data.size(), symbol, 0xa }); data.u(0, 2);
}
inline std::vector<std::uint8_t> codeViewObject(const Model& model) {
	std::vector<Section> sections{ textSection(model, true) };
	Section xdata{ ".xdata" }, pdata{ ".pdata" }, debug{ ".debug$S" }, types{ ".debug$T" }, directives{ ".drectve" };
	xdata.flags = pdata.flags = 0x40300040; debug.flags = types.flags = 0x42300040; directives.flags = 0x00100a00;
	std::map<std::uint32_t, std::pair<std::string, std::uint32_t>> frames;
	for (const auto& [offset, fn] : model.functions) frames.emplace(offset, std::make_pair(fn->name, fn->frameSize));
	if (model.artifacts.machine.functionOffsets.contains("__pcn_entry_args")) frames.emplace(model.artifacts.machine.functionOffsets.at("__pcn_entry_args"), std::make_pair("__pcn_entry_args", 256));
	for (const auto& [offset, function] : frames) {
		const auto& [name, frame] = function;
		pdata.relocations.push_back({ pdata.bytes.data.size(), ".text", 3 }); pdata.bytes.u(offset, 4);
		pdata.relocations.push_back({ pdata.bytes.data.size(), ".text", 3 }); pdata.bytes.u(offset + model.artifacts.machine.functionSizes.at(name), 4);
		pdata.relocations.push_back({ pdata.bytes.data.size(), ".xdata", 3 }); pdata.bytes.u(xdata.bytes.data.size(), 4);
		xdata.bytes.append(native_abi::windowsUnwind(frame));
	}
	debug.bytes.u(4, 4); CVTypes typeTable(model.abi);
	Data strings, checksums; strings.u(0, 1); std::map<unsigned, std::uint32_t> checksumOffsets;
	for (unsigned n = 0; n < model.files.size(); ++n) {
		checksumOffsets[n + 1] = static_cast<std::uint32_t>(checksums.data.size());
		checksums.u(strings.string(model.files[n]), 4); checksums.u(32, 1); checksums.u(3, 1);
		const auto& digest = model.sourceHashes.at(model.files[n]);
		checksums.data.insert(checksums.data.end(), digest.begin(), digest.end()); checksums.align(4);
	}
	Data records, object; object.u(0, 4); object.text("Paracine"); cvRecord(records, 0x1101, std::move(object));
	Data compiler; compiler.u(0, 4); compiler.u(0xd0, 2); for (unsigned n = 0; n < 8; ++n) compiler.u(n == 0 || n == 4 ? 1 : 0, 2); compiler.text(kCompilerBuild); cvRecord(records, 0x113c, std::move(compiler));
	cvSubsection(debug, 0xf1, records);
	for (const auto& [offset, fn] : model.functions) {
		const auto& original = model.source(fn->name); const auto size = model.artifacts.machine.functionSizes.at(fn->name);
		Data recordsForFunction, procedure; std::vector<Relocation> symbolRelocs;
		procedure.u(0, 4); procedure.u(0, 4); procedure.u(0, 4); procedure.u(size, 4); procedure.u(11, 4); procedure.u(size, 4); procedure.u(typeTable.procedure(original), 4);
		cvAddress(procedure, symbolRelocs, 0, fn->name); procedure.u(0, 1); procedure.text(fn->name);
		cvRecord(recordsForFunction, 0x1110, std::move(procedure));
		for (auto& relocation : symbolRelocs) relocation.offset += 4;
		Data frame; frame.u(fn->frameSize, 4); frame.u(0, 4); frame.u(0, 4); frame.u(8, 4); frame.u(0, 4); frame.u(0, 2); frame.u(0x28000, 4); cvRecord(recordsForFunction, 0x1012, std::move(frame));
		for (const auto& variable : original.debugVariables) {
			const auto live = model.ranges(*fn, variable);
			Data local; local.u(typeTable.type(variable.type), 4); local.u((variable.parameter ? 1 : 0) | (live.empty() ? 0x100 : 0), 2); local.text(variable.name); cvRecord(recordsForFunction, 0x113e, std::move(local));
			for (const auto& [begin, end] : live) for (auto cursor = begin; cursor < end;) {
				const auto count = std::min<std::uint32_t>(end - cursor, UINT16_MAX);
				Data range; range.u(static_cast<std::uint32_t>(-static_cast<std::int32_t>(fn->slots.at(static_cast<std::uint32_t>(variable.value)).cellOffset)), 4);
				std::vector<Relocation> relocations; cvAddress(range, relocations, cursor - offset, fn->name); range.u(count, 2);
				const auto base = cvRecord(recordsForFunction, 0x1142, std::move(range));
				for (auto relocation : relocations) { relocation.offset += base + 4; symbolRelocs.push_back(std::move(relocation)); }
				cursor += count;
			}
		}
		const auto endSymbol = cvRecord(recordsForFunction, 6, {}); recordsForFunction.patch(8, endSymbol, 4);
		cvSubsection(debug, 0xf1, recordsForFunction, symbolRelocs);
		std::map<unsigned, std::vector<const Row*>> byFile;
		for (const auto& row : model.rows) if (row.address >= offset && row.address < offset + size) byFile[model.file(row.location)].push_back(&row);
		for (const auto& [file, rows] : byFile) {
			Data lines; std::vector<Relocation> relocations; cvAddress(lines, relocations, 0, fn->name); lines.u(1, 2); lines.u(size, 4);
			lines.u(checksumOffsets.at(file), 4); lines.u(rows.size(), 4); lines.u(12 + rows.size() * 12, 4);
			for (const auto* row : rows) { lines.u(row->address - offset, 4); lines.u(row->location.line | 0x80000000u, 4); }
			for (const auto* row : rows) { lines.u(row->location.column, 2); lines.u(row->location.column, 2); }
			cvSubsection(debug, 0xf2, lines, relocations);
		}
	}
	cvSubsection(debug, 0xf4, checksums); cvSubsection(debug, 0xf3, strings);
	types.bytes = std::move(typeTable.stream);
	const auto directiveText = native_abi::coffDirectives(model.artifacts.pcir); directives.bytes.data.assign(directiveText.begin(), directiveText.end());
	sections.push_back(std::move(xdata)); sections.push_back(std::move(pdata)); sections.push_back(std::move(debug)); sections.push_back(std::move(types)); sections.push_back(std::move(directives)); sections.push_back(abiSection(model));
	return coff(model, std::move(sections));
}
inline void dwarfAddress(Section& section, std::uint64_t offset) {
	section.relocations.push_back({ section.bytes.data.size(), ".text", 1, static_cast<std::int64_t>(offset) }); section.bytes.u(0, 8);
}
inline void dwarfSectionOffset(Section& section, const std::string& target, std::uint32_t offset = 0) {
	section.relocations.push_back({ section.bytes.data.size(), target, 10, offset }); section.bytes.u(0, 4);
}
inline void dwarfLength(Section& section, std::size_t start = 0) { section.bytes.patch(start, section.bytes.data.size() - start - 4, 4); }
inline std::vector<Section> dwarfSections(const Model& model) {
	Section line{ ".debug_line" }, abbrev{ ".debug_abbrev" }, info{ ".debug_info" }, locations{ ".debug_loc" }, frame{ ".debug_frame" };
	line.bytes.u(0, 4); line.bytes.u(4, 2); const auto headerLength = line.bytes.data.size(); line.bytes.u(0, 4);
	line.bytes.u(1, 1); line.bytes.u(1, 1); line.bytes.u(1, 1); line.bytes.u(251, 1); line.bytes.u(14, 1); line.bytes.u(13, 1);
	for (auto operands : { 0,1,1,1,1,0,0,0,1,0,0,1 }) line.bytes.u(operands, 1);
	line.bytes.u(0, 1);
	for (const auto& file : model.files) { line.bytes.text(file); line.bytes.leb(0); line.bytes.leb(0); line.bytes.leb(0); }
	line.bytes.u(0, 1); line.bytes.patch(headerLength, line.bytes.data.size() - headerLength - 4, 4);
	for (const auto& [offset, fn] : model.functions) {
		std::int64_t sourceLine = 1; unsigned sourceFile = 1; std::uint32_t address = offset;
		line.bytes.u(0, 1); line.bytes.leb(9); line.bytes.u(2, 1); dwarfAddress(line, offset);
		for (const auto& row : model.rows) {
			if (row.address < offset || row.address >= offset + model.artifacts.machine.functionSizes.at(fn->name)) continue;
			if (row.address != address) { line.bytes.u(2, 1); line.bytes.leb(row.address - address); address = row.address; }
			const auto file = model.file(row.location);
			if (sourceFile != file) { line.bytes.u(4, 1); line.bytes.leb(file); sourceFile = file; }
			line.bytes.u(3, 1); line.bytes.sleb(static_cast<std::int64_t>(row.location.line) - sourceLine); sourceLine = static_cast<std::int64_t>(row.location.line);
			line.bytes.u(5, 1); line.bytes.leb(row.location.column); line.bytes.u(1, 1);
		}
		line.bytes.u(2, 1); line.bytes.leb(offset + model.artifacts.machine.functionSizes.at(fn->name) - address);
		line.bytes.u(0, 1); line.bytes.leb(1); line.bytes.u(1, 1);
	}
	dwarfLength(line);
	auto abbreviation = [&](unsigned id, unsigned tag, bool children, std::initializer_list<std::pair<unsigned, unsigned>> attributes) {
		abbrev.bytes.leb(id); abbrev.bytes.leb(tag); abbrev.bytes.u(children, 1);
		for (const auto& [attribute, form] : attributes) { abbrev.bytes.leb(attribute); abbrev.bytes.leb(form); }
		abbrev.bytes.leb(0); abbrev.bytes.leb(0);
	};
	abbreviation(1, 0x11, true, { {0x25,8},{0x13,5},{3,8},{0x10,0x17},{0x11,1},{0x12,7} });
	abbreviation(2, 0x24, false, { {3,8},{0xb,0xf},{0x3e,0xb} });
	abbreviation(3, 0xf, false, { {3,8},{0xb,0xb} });
	abbreviation(4, 0x13, true, { {3,8},{0xb,0xf} });
	abbreviation(5, 0xd, false, { {3,8},{0x49,0x13},{0x38,0xf} });
	abbreviation(6, 0x2e, true, { {3,8},{0x6e,8},{0x3a,0xf},{0x3b,0xf},{0x11,1},{0x12,7},{0x40,0x18},{0x49,0x13},{0x3f,0x19} });
	abbreviation(7, 0x34, false, { {3,8},{0x49,0x13},{0x3a,0xf},{0x3b,0xf},{2,0x17} });
	abbreviation(8, 5, false, { {3,8},{0x49,0x13},{0x3a,0xf},{0x3b,0xf},{2,0x17} });
	abbreviation(9, 0x3b, false, { {3,8} });
	abbreviation(10, 0x34, false, { {3,8},{0x49,0x13},{0x3a,0xf},{0x3b,0xf} });
	abbreviation(11, 5, false, { {3,8},{0x49,0x13},{0x3a,0xf},{0x3b,0xf} });
	abbreviation(12, 1, true, { {3,8},{0x49,0x13},{0xb,0xf} });
	abbreviation(13, 0x21, false, { {0x37,0xf} });
	abbrev.bytes.leb(0);
	info.bytes.u(0, 4); info.bytes.u(4, 2); dwarfSectionOffset(info, ".debug_abbrev"); info.bytes.u(8, 1);
	info.bytes.leb(1); info.bytes.text(kCompilerBuild); info.bytes.u(0x0002, 2); info.bytes.text(model.files.front()); dwarfSectionOffset(info, ".debug_line"); dwarfAddress(info, 0); info.bytes.u(model.artifacts.machine.text.size(), 8);
	std::map<std::string, std::uint32_t> typeOffsets;
	std::vector<std::pair<std::size_t, std::string>> references;
	auto typeReference = [&](const std::string& name) { references.emplace_back(info.bytes.data.size(), name); info.bytes.u(0, 4); };
	std::set<std::string> pending = collectABITypeNames(model.artifacts.pcir); pending.insert("void");
	for (const auto& fn : model.artifacts.pcir.functions) for (const auto& variable : fn.debugVariables) pending.insert(variable.type);
	while (!pending.empty()) {
		const auto name = *pending.begin(); pending.erase(pending.begin()); if (typeOffsets.contains(name)) continue;
		if (typeOffsets.size() > 65536) throw std::runtime_error("E0580 excessive debug type graph");
		const auto layout = model.abi.layout(name); typeOffsets[name] = static_cast<std::uint32_t>(info.bytes.data.size());
		std::string element; std::size_t count = 0;
		if (name == "void" || name == "never") { info.bytes.leb(9); info.bytes.text(name); }
		else if (name == "raw" || name.rfind("raw<", 0) == 0) { info.bytes.leb(3); info.bytes.text(name); info.bytes.u(8, 1); }
		else if (typeIsArray(name, &element, &count)) {
			info.bytes.leb(12); info.bytes.text(name); typeReference(element); pending.insert(element); info.bytes.leb(layout.size);
			info.bytes.leb(13); info.bytes.leb(count); info.bytes.leb(0);
		}
		else if (!layout.rich) {
			info.bytes.leb(2); info.bytes.text(name); info.bytes.leb(layout.size);
			info.bytes.u(name == "float" || name == "double" ? 4 : name == "bool" ? 2 : layout.signedScalar ? 5 : 7, 1);
		}
		else {
			info.bytes.leb(4); info.bytes.text(name); info.bytes.leb(layout.size);
			for (const auto& member : fields(layout)) {
				const auto memberType = member.byReference ? "raw" : member.typeName;
				info.bytes.leb(5); info.bytes.text(member.name); typeReference(memberType); pending.insert(memberType); info.bytes.leb(member.offset);
			}
			info.bytes.leb(0);
		}
	}
	for (const auto& [offset, fn] : model.functions) {
		const auto& original = model.source(fn->name);
		info.bytes.leb(6); info.bytes.text(fn->name); info.bytes.text(fn->name); info.bytes.leb(model.file(original.declaration.start)); info.bytes.leb(original.declaration.start.line);
		dwarfAddress(info, offset); info.bytes.u(model.artifacts.machine.functionSizes.at(fn->name), 8);
		info.bytes.leb(1); info.bytes.u(0x56, 1); typeReference(original.returnType);
		for (const auto& variable : original.debugVariables) {
			const auto live = model.ranges(*fn, variable);
			info.bytes.leb(live.empty() ? (variable.parameter ? 11 : 10) : (variable.parameter ? 8 : 7)); info.bytes.text(variable.name); typeReference(variable.type);
			info.bytes.leb(model.file(variable.declaration.start)); info.bytes.leb(variable.declaration.start.line);
			if (!live.empty()) {
				dwarfSectionOffset(info, ".debug_loc", static_cast<std::uint32_t>(locations.bytes.data.size()));
				for (const auto& [begin, end] : live) {
					// A base-selection entry keeps absolute relocated addresses independent of CU low_pc.
					locations.bytes.u(UINT64_MAX, 8); locations.bytes.u(0, 8);
					dwarfAddress(locations, begin); dwarfAddress(locations, end);
					Data expression; expression.u(0x91, 1); const auto& slot = fn->slots.at(static_cast<std::uint32_t>(variable.value)); expression.sleb(-static_cast<std::int64_t>(slot.cellOffset));
					locations.bytes.u(expression.data.size(), 2); locations.bytes.append(expression.data);
				}
				locations.bytes.u(0, 8); locations.bytes.u(0, 8);
			}
		}
		info.bytes.leb(0);
	}
	info.bytes.leb(0); dwarfLength(info);
	for (const auto& [offset, name] : references) info.bytes.patch(offset, typeOffsets.at(name), 4);
	frame.bytes.u(0, 4); frame.bytes.u(UINT32_MAX, 4); frame.bytes.u(3, 1); frame.bytes.text(""); frame.bytes.leb(1); frame.bytes.sleb(-8); frame.bytes.leb(16);
	frame.bytes.u(0xc, 1); frame.bytes.leb(7); frame.bytes.leb(8); frame.bytes.u(0x90, 1); frame.bytes.leb(1); frame.bytes.align(8); dwarfLength(frame);
	for (const auto& [offset, fn] : model.functions) {
		const auto start = frame.bytes.data.size(); frame.bytes.u(0, 4); dwarfSectionOffset(frame, ".debug_frame"); dwarfAddress(frame, offset); frame.bytes.u(model.artifacts.machine.functionSizes.at(fn->name), 8);
		frame.bytes.u(0x41, 1); frame.bytes.u(0xe, 1); frame.bytes.leb(16); frame.bytes.u(0x86, 1); frame.bytes.leb(2);
		frame.bytes.u(0x43, 1); frame.bytes.u(0xd, 1); frame.bytes.leb(6);
		std::uint32_t pc = offset + 4;
		auto advance = [&](std::uint32_t next) { frame.bytes.u(4, 1); frame.bytes.u(next - pc, 4); pc = next; };
		for (const auto& listing : model.artifacts.machine.listing) {
			const auto end = static_cast<std::uint32_t>(listing.offset + listing.bytes.size());
			if (listing.offset < offset || end > offset + model.artifacts.machine.functionSizes.at(fn->name) || listing.bytes.size() < 9) continue;
			const auto& bytes = listing.bytes; const auto n = bytes.size();
			if (bytes[n - 9] != 0x48 || bytes[n - 8] != 0x81 || bytes[n - 7] != 0xc4 || bytes[n - 2] != 0x5d || bytes[n - 1] != 0xc3) continue;
			advance(end - 1); frame.bytes.u(0xa, 1); frame.bytes.u(0xc, 1); frame.bytes.leb(7); frame.bytes.leb(8); frame.bytes.u(0xc6, 1);
			advance(end); frame.bytes.u(0xb, 1);
		}
		frame.bytes.align(8); dwarfLength(frame, start);
	}
	return { std::move(abbrev), std::move(info), std::move(line), std::move(locations), std::move(frame) };
}
inline std::vector<std::uint8_t> elf(const Model& model, bool executable) {
	if (!model.artifacts.machine.systemV) throw std::runtime_error("E0580 DWARF ELF output requires System V code");
	auto sections = dwarfSections(model); sections.insert(sections.begin(), textSection(model, false)); sections.push_back(abiSection(model));
	sections.push_back({ ".note.GNU-stack" });
	auto inventory = symbols(model, sections);
	std::map<std::string, std::uint32_t> indexes;
	std::vector<Symbol> ordered;
	for (const auto& symbol : inventory) if (!symbol.function && symbol.section) ordered.push_back(symbol);
	const auto firstGlobal = static_cast<std::uint32_t>(ordered.size() + 1);
	for (const auto& symbol : inventory) if (symbol.function || !symbol.section) ordered.push_back(symbol);
	for (std::uint32_t n = 0; n < ordered.size(); ++n) indexes[ordered[n].name] = n + 1;
	Section strings{ ".strtab" }, table{ ".symtab" }; strings.bytes.u(0, 1); table.bytes.data.resize(24);
	constexpr std::uint64_t codeAddress = 0x401000;
	for (const auto& symbol : ordered) {
		table.bytes.u(strings.bytes.string(symbol.name), 4); table.bytes.u(symbol.function ? 0x12 : symbol.section ? 3 : 0x10, 1); table.bytes.u(0, 1); table.bytes.u(symbol.section, 2);
		table.bytes.u(symbol.offset + (executable && symbol.section == 1 ? codeAddress : 0), 8); table.bytes.u(symbol.size, 8);
	}
	const auto originalCount = sections.size();
	const auto symbolSection = static_cast<unsigned>(originalCount + 1), stringSection = symbolSection + 1;
	sections.push_back(std::move(table)); sections.push_back(std::move(strings));
	std::map<unsigned, unsigned> relocationTargets;
	for (unsigned n = 0; n < originalCount; ++n) {
		if (sections[n].relocations.empty()) continue;
		Section relocations{ ".rela" + sections[n].name };
		for (const auto& relocation : sections[n].relocations) {
			if (executable) {
				if (relocation.kind != 1 && relocation.kind != 10) throw std::runtime_error("E0580 debug executable contains unresolved external calls");
				const auto symbol = indexes.at(relocation.symbol); const auto& target = ordered.at(symbol - 1);
				const auto value = static_cast<std::uint64_t>(relocation.addend) + target.offset + (target.section == 1 ? codeAddress : 0);
				sections[n].bytes.patch(relocation.offset, value, relocation.kind == 1 ? 8 : 4);
			}
			else {
				relocations.bytes.u(relocation.offset, 8); relocations.bytes.u((std::uint64_t(indexes.at(relocation.symbol)) << 32) | relocation.kind, 8); relocations.bytes.u(static_cast<std::uint64_t>(relocation.addend), 8);
			}
		}
		if (!executable) { relocationTargets[static_cast<unsigned>(sections.size() + 1)] = n + 1; sections.push_back(std::move(relocations)); }
	}
	Section names{ ".shstrtab" }; names.bytes.u(0, 1); std::vector<std::uint32_t> nameOffsets{ 0 };
	for (const auto& section : sections) nameOffsets.push_back(names.bytes.string(section.name));
	nameOffsets.push_back(names.bytes.string(names.name)); sections.push_back(std::move(names));
	Data out;
	if (executable) out.data = unix_output::elfExecutable(model.artifacts.machine); else out.data.resize(64);
	std::vector<std::uint64_t> offsets{ 0 }, sizes{ 0 };
	for (std::size_t n = 0; n < sections.size(); ++n) {
		if (executable && n == 0) { offsets.push_back(4096); sizes.push_back(sections[n].bytes.data.size()); continue; }
		out.align(8); offsets.push_back(out.data.size()); sizes.push_back(sections[n].bytes.data.size()); out.append(sections[n].bytes.data);
	}
	out.align(8); const auto sectionTable = out.data.size(); out.data.resize(out.data.size() + 64);
	for (unsigned n = 1; n <= sections.size(); ++n) {
		const auto& section = sections[n - 1]; const bool reloc = relocationTargets.contains(n), sym = n == symbolSection, str = n == stringSection || n == sections.size();
		out.u(nameOffsets[n], 4); out.u(reloc ? 4 : sym ? 2 : str ? 3 : 1, 4); out.u(n == 1 ? 6 : 0, 8); out.u(executable && n == 1 ? codeAddress : 0, 8);
		out.u(offsets[n], 8); out.u(sizes[n], 8); out.u(reloc ? symbolSection : sym ? stringSection : 0, 4); out.u(reloc ? relocationTargets.at(n) : sym ? firstGlobal : 0, 4);
		out.u(n == 1 ? 16 : 8, 8); out.u(reloc || sym ? 24 : 0, 8);
	}
	if (!executable) { out.patch(0, 0x00010102464c457full, 8); out.patch(16, 1, 2); out.patch(18, 62, 2); out.patch(20, 1, 4); out.patch(52, 64, 2); }
	out.patch(40, sectionTable, 8); out.patch(58, 64, 2); out.patch(60, sections.size() + 1, 2); out.patch(62, sections.size(), 2); return out.data;
}
inline std::vector<std::uint8_t> object(const CompilationArtifacts& artifacts, const CompilerOptions& options) {
	const auto& target = target_contract::get(options.target);
	target_contract::requireCodegen(target);
	if (!target_contract::debugSupported(target)) throw std::runtime_error("E0580 native debug information is not implemented for this target");
	Model model(artifacts, options);
	return target.windows() ? codeViewObject(model) : elf(model, false);
}
inline void windowsExecutable(const CompilationArtifacts& artifacts, const CompilerOptions& options, const std::filesystem::path& output) {
	const auto absolute = std::filesystem::absolute(output);
	auto pdb = absolute; pdb.replace_extension(".pdb");
	if (absolute == pdb) throw std::runtime_error("E0580 executable and PDB paths must differ");
	std::filesystem::path directory; std::random_device random;
	for (unsigned attempt = 0; attempt < 32; ++attempt) {
		const auto candidate = std::filesystem::temp_directory_path() / ("paracine-debug-" + std::to_string(random()) + "-" + std::to_string(random()));
		std::error_code error; if (std::filesystem::create_directory(candidate, error)) { directory = candidate; break; }
		if (error) throw std::runtime_error("E0580 cannot create debug link scratch directory: " + error.message());
	}
	if (directory.empty()) throw std::runtime_error("E0580 cannot allocate debug link scratch directory");
	struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path / "program.obj", error); std::filesystem::remove(path, error); } } cleanup{ directory };
	writeBinaryFile(directory / "program.obj", object(artifacts, options));
	std::vector<std::string> invocation{ options.debugLinker, "/nologo", "/machine:x64", "/nodefaultlib", "/subsystem:console", "/debug:full", "/incremental:no", "/opt:noref,noicf", "/Brepro", "/entry:" + artifacts.machine.entryName,
		"/out:" + source_modules::utf8Path(absolute), "/pdb:" + source_modules::utf8Path(pdb), source_modules::utf8Path(directory / "program.obj") };
	if (!artifacts.machine.imports.empty()) { invocation.push_back("kernel32.lib"); invocation.push_back("shell32.lib"); }
	const auto status = executeArgv(invocation);
	if (status) throw std::runtime_error("E0580 debug PE/PDB link failed with exit code " + std::to_string(status) + "; run in a configured SDK environment for Win32 imports");
	if (!std::filesystem::is_regular_file(absolute) || !std::filesystem::is_regular_file(pdb)) throw std::runtime_error("E0580 debug linker did not produce both PE and PDB artifacts");
}
}
