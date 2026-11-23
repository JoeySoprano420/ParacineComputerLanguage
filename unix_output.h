#pragma once

// Included inside namespace pcn after the target encoder.
namespace unix_output {
inline const target_contract::Target& target(const std::string& name) {
	const auto& selected = target_contract::get(name);
	target_contract::requireCodegen(selected);
	if (!selected.systemV()) throw std::runtime_error("E0540 Unix output requires the implemented System V x86-64 profile");
	return selected;
}

class Bytes {
public:
	std::vector<std::uint8_t> data;
	void integer(std::uint64_t value, unsigned width) {
		for (unsigned n = 0; n < width; ++n) data.push_back(static_cast<std::uint8_t>(value >> (n * 8)));
	}
	void patch(std::size_t offset, std::uint64_t value, unsigned width) {
		if (offset > data.size() || width > data.size() - offset) throw std::runtime_error("E0542 object patch out of bounds");
		for (unsigned n = 0; n < width; ++n) data[offset + n] = static_cast<std::uint8_t>(value >> (n * 8));
	}
	void align(std::size_t alignment) { while (data.size() % alignment) data.push_back(0); }
	void name(const std::string& value, std::size_t width) {
		if (value.size() > width) throw std::runtime_error("E0542 object section name too long");
		data.insert(data.end(), value.begin(), value.end()); data.insert(data.end(), width - value.size(), 0);
	}
	std::uint32_t string(const std::string& value) {
		if (data.size() > UINT32_MAX - value.size() - 1) throw std::runtime_error("E0542 object string table overflow");
		const auto offset = static_cast<std::uint32_t>(data.size());
		data.insert(data.end(), value.begin(), value.end()); data.push_back(0); return offset;
	}
};
struct Symbol { std::string name; std::uint32_t offset = 0, size = 0; bool defined = false; };
inline std::vector<Symbol> symbols(const MachineProgram& machine) {
    const auto errors = MachineVerifier{}.verify(machine);
    if (!machine.systemV) throw std::runtime_error("E0540 Unix output requires System V machine code");
    if (!errors.empty()) throw std::runtime_error("E0542 Unix output requires verified machine code: " + errors.front());
    if (!machine.imports.empty()) throw std::runtime_error("E0540 Win32 imports cannot appear in Unix output");
	if (machine.text.size() > 256u * 1024u * 1024u) throw std::runtime_error("E0542 Unix text exceeds 256 MiB output limit");
	std::map<std::string, Symbol> items;
	for (const auto& [name, offset] : machine.functionOffsets)
		items[name] = { name,offset,machine.functionSizes.at(name),true };
	for (const auto& relocation : machine.externalCalls) {
		if (relocation.dispPos == 0 || relocation.dispPos > machine.text.size() || machine.text.size() - relocation.dispPos < 4 ||
			machine.text[relocation.dispPos - 1] != 0xE8 || relocation.symbol.empty())
			throw std::runtime_error("E0542 invalid Unix external call relocation");
		if (!items.contains(relocation.symbol)) items[relocation.symbol] = { relocation.symbol,0,0,false };
	}
	std::vector<Symbol> result;
	for (const auto& [name, item] : items) result.push_back(item);
	return result;
}

inline std::vector<std::uint8_t> elfObject(const MachineProgram& machine) {
	const auto inventory = symbols(machine);
	Bytes names, strings, symtab, relocs; names.integer(0, 1); strings.integer(0, 1);
	const std::string sectionNames[] = { "", ".text", ".rela.text", ".symtab", ".strtab", ".shstrtab", ".note.GNU-stack" };
	std::vector<std::uint32_t> nameOffsets{ 0 };
	for (std::size_t n = 1; n < 7; ++n) nameOffsets.push_back(names.string(sectionNames[n]));
	symtab.data.resize(24);
	std::map<std::string, std::uint32_t> indexes;
	for (const auto& symbol : inventory) {
		indexes[symbol.name] = static_cast<std::uint32_t>(indexes.size() + 1);
		symtab.integer(strings.string(symbol.name), 4); symtab.integer(0x12, 1); symtab.integer(0, 1);
		symtab.integer(symbol.defined ? 1 : 0, 2); symtab.integer(symbol.offset, 8); symtab.integer(symbol.size, 8);
	}
	auto external = machine.externalCalls;
	std::sort(external.begin(), external.end(), [](const auto& a, const auto& b) { return a.dispPos < b.dispPos; });
	for (const auto& relocation : external) {
		relocs.integer(relocation.dispPos, 8);
		relocs.integer((std::uint64_t(indexes.at(relocation.symbol)) << 32) | 4, 8); // R_X86_64_PLT32
		relocs.integer(static_cast<std::uint64_t>(-4), 8);
	}
	Bytes out; out.data.resize(64);
	const std::vector<std::uint8_t>* contents[] = { nullptr,&machine.text,&relocs.data,&symtab.data,&strings.data,&names.data,nullptr };
	const std::uint64_t alignments[] = { 0,16,8,8,1,1,1 };
	std::uint64_t offsets[7]{}, sizes[7]{};
	for (std::size_t n = 1; n < 7; ++n) {
		out.align(static_cast<std::size_t>(alignments[n])); offsets[n] = out.data.size();
		if (contents[n]) { sizes[n] = contents[n]->size(); out.data.insert(out.data.end(), contents[n]->begin(), contents[n]->end()); }
	}
	out.align(8); const auto sectionTable = out.data.size(); out.data.resize(out.data.size() + 64);
	const std::uint32_t types[] = { 0,1,4,2,3,3,1 };
	for (std::size_t n = 1; n < 7; ++n) {
		out.integer(nameOffsets[n], 4); out.integer(types[n], 4); out.integer(n == 1 ? 6 : 0, 8); out.integer(0, 8);
		out.integer(offsets[n], 8); out.integer(sizes[n], 8); out.integer(n == 2 ? 3 : n == 3 ? 4 : 0, 4);
		out.integer(n == 2 ? 1 : n == 3 ? 1 : 0, 4); out.integer(alignments[n], 8); out.integer(n == 2 || n == 3 ? 24 : 0, 8);
	}
	out.patch(0, 0x00010102464c457full, 8); out.patch(16, 1, 2); out.patch(18, 62, 2); out.patch(20, 1, 4);
	out.patch(40, sectionTable, 8); out.patch(52, 64, 2); out.patch(58, 64, 2); out.patch(60, 7, 2); out.patch(62, 5, 2);
	return out.data;
}

inline std::vector<std::uint8_t> machoObject(const MachineProgram& machine) {
	const auto inventory = symbols(machine);
	if (inventory.size() >= (1u << 24)) throw std::runtime_error("E0542 Mach-O relocation symbol index overflow");
	Bytes strings, symtab, relocs; strings.integer(0, 1);
	std::map<std::string, std::uint32_t> indexes;
	for (const auto& symbol : inventory) {
		indexes[symbol.name] = static_cast<std::uint32_t>(indexes.size());
		symtab.integer(strings.string("_" + symbol.name), 4); symtab.integer(symbol.defined ? 0x0f : 0x01, 1);
		symtab.integer(symbol.defined ? 1 : 0, 1); symtab.integer(0, 2); symtab.integer(symbol.offset, 8);
	}
	auto external = machine.externalCalls;
	// Mach-O relocation tables conventionally run in descending address order.
	std::sort(external.begin(), external.end(), [](const auto& a, const auto& b) { return a.dispPos > b.dispPos; });
	for (const auto& relocation : external) {
		relocs.integer(relocation.dispPos, 4);
		relocs.integer(indexes.at(relocation.symbol) | (1u << 24) | (2u << 25) | (1u << 27) | (2u << 28), 4);
	}
	constexpr std::uint32_t commandsSize = 152 + 24 + 24;
	Bytes out;
	out.integer(0xfeedfacf, 4); out.integer(0x01000007, 4); out.integer(3, 4); out.integer(1, 4);
	out.integer(3, 4); out.integer(commandsSize, 4); out.integer(0, 4); out.integer(0, 4);
	const auto textOffset = (32 + commandsSize + 15) & ~15u;
	const auto relocOffset = (textOffset + machine.text.size() + 3) & ~std::size_t{3};
	const auto symbolOffset = (relocOffset + relocs.data.size() + 7) & ~std::size_t{7};
	const auto stringOffset = symbolOffset + symtab.data.size();
	out.integer(0x19, 4); out.integer(152, 4); out.name("", 16); out.integer(0, 8); out.integer(machine.text.size(), 8);
	out.integer(textOffset, 8); out.integer(machine.text.size(), 8); out.integer(7, 4); out.integer(5, 4); out.integer(1, 4); out.integer(0, 4);
	out.name("__text", 16); out.name("__TEXT", 16); out.integer(0, 8); out.integer(machine.text.size(), 8);
	out.integer(textOffset, 4); out.integer(4, 4); out.integer(relocOffset, 4); out.integer(external.size(), 4);
	out.integer(0x80000400, 4); out.integer(0, 4); out.integer(0, 4); out.integer(0, 4);
	out.integer(2, 4); out.integer(24, 4); out.integer(symbolOffset, 4); out.integer(inventory.size(), 4);
	out.integer(stringOffset, 4); out.integer(strings.data.size(), 4);
	out.integer(0x32, 4); out.integer(24, 4); out.integer(1, 4); out.integer(0x000a0f00, 4); out.integer(0, 4); out.integer(0, 4);
	out.align(16); out.data.insert(out.data.end(), machine.text.begin(), machine.text.end()); out.align(4);
	out.data.insert(out.data.end(), relocs.data.begin(), relocs.data.end()); out.align(8);
	out.data.insert(out.data.end(), symtab.data.begin(), symtab.data.end()); out.data.insert(out.data.end(), strings.data.begin(), strings.data.end());
	return out.data;
}

inline std::vector<std::uint8_t> elfExecutable(const MachineProgram& machine) {
	(void)symbols(machine);
	if (machine.entryName.empty()) throw std::runtime_error("E0541 static ELF requires an executable entry point");
	if (!machine.externalCalls.empty()) throw std::runtime_error("E0541 static ELF requires all routines to be defined; emit an object and use a System V linker for external libraries");
	Bytes text; text.data = machine.text; text.align(16); const auto entryOffset = text.data.size();
	text.data.insert(text.data.end(), { 0x48,0x83,0xe4,0xf0,0xe8 });
	const auto displacement = static_cast<std::int64_t>(machine.entryOffset) - static_cast<std::int64_t>(text.data.size() + 4);
	text.integer(static_cast<std::uint32_t>(displacement), 4);
	text.data.insert(text.data.end(), { 0x89,0xc7,0xb8,0x3c,0,0,0,0x0f,0x05,0x0f,0x0b });
	constexpr std::uint64_t base = 0x400000, codeOffset = 4096;
	Bytes out; out.data.resize(64);
	out.patch(0, 0x00010102464c457full, 8); out.patch(16, 2, 2); out.patch(18, 62, 2); out.patch(20, 1, 4);
	out.patch(24, base + codeOffset + entryOffset, 8); out.patch(32, 64, 8); out.patch(52, 64, 2); out.patch(54, 56, 2); out.patch(56, 3, 2);
	auto segment = [&](std::uint32_t type, std::uint32_t flags, std::uint64_t offset, std::uint64_t address, std::uint64_t size, std::uint64_t alignment) {
		out.integer(type, 4); out.integer(flags, 4); out.integer(offset, 8); out.integer(address, 8); out.integer(address, 8);
		out.integer(size, 8); out.integer(size, 8); out.integer(alignment, 8);
	};
	segment(1, 4, 0, base, 64 + 3 * 56, 4096);
	segment(1, 5, codeOffset, base + codeOffset, text.data.size(), 4096);
	segment(0x6474e551, 6, 0, 0, 0, 16);
	out.data.resize(codeOffset); out.data.insert(out.data.end(), text.data.begin(), text.data.end()); return out.data;
}

class Reader {
	const std::vector<std::uint8_t>& data_;
public:
	explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}
	void require(std::uint64_t offset, std::uint64_t length) const {
		if (offset > data_.size() || length > data_.size() - offset) throw std::runtime_error("E0542 Unix artifact range exceeds file");
	}
	std::uint64_t integer(std::uint64_t offset, unsigned width) const {
		require(offset, width); std::uint64_t value = 0;
		for (unsigned n = 0; n < width; ++n) value |= std::uint64_t(data_[static_cast<std::size_t>(offset + n)]) << (8 * n);
		return value;
	}
	void string(std::uint64_t table, std::uint64_t size, std::uint64_t index) const {
		require(table, size);
		if (index >= size) throw std::runtime_error("E0542 Unix string index is out of bounds");
		for (auto n = index; n < size; ++n) if (!integer(table + n, 1)) return;
		throw std::runtime_error("E0542 Unix string is not terminated");
	}
};
inline bool isUnix(const std::vector<std::uint8_t>& data) {
	return data.size() >= 4 && ((data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') ||
		(data[0] == 0xcf && data[1] == 0xfa && data[2] == 0xed && data[3] == 0xfe));
}
inline std::string verify(const std::vector<std::uint8_t>& data) {
	Reader r(data);
	auto fail = [](const char* message) { throw std::runtime_error(std::string("E0542 ") + message); };
	if (r.integer(0, 4) == 0x464c457f) {
		r.require(0, 64);
		if (r.integer(4, 1) != 2 || r.integer(5, 1) != 1 || r.integer(6, 1) != 1 ||
			r.integer(18, 2) != 62 || r.integer(20, 4) != 1 || r.integer(52, 2) != 64) fail("unsupported ELF identification");
		const auto type = r.integer(16, 2);
		if (type == 2) {
			const auto table = r.integer(32, 8), count = r.integer(56, 2), width = r.integer(54, 2), entry = r.integer(24, 8);
			if (width != 56 || !count) fail("invalid ELF program-header table");
			r.require(table, count * width); bool mapped = false, stack = false;
			for (std::uint64_t n = 0; n < count; ++n) {
				const auto p = table + n * width, kind = r.integer(p, 4), flags = r.integer(p + 4, 4);
				const auto offset = r.integer(p + 8, 8), address = r.integer(p + 16, 8), size = r.integer(p + 32, 8);
				const auto memory = r.integer(p + 40, 8), align = r.integer(p + 48, 8);
				r.require(offset, size);
				if (size > memory || memory > UINT64_MAX - address) fail("invalid ELF segment memory size");
				if (kind == 1) {
					if ((flags & 3) == 3 || (align > 1 && ((align & (align - 1)) || offset % align != address % align))) fail("invalid ELF protection/alignment");
					mapped = mapped || ((flags & 1) && entry >= address && entry - address < size);
				}
				if (kind == 0x6474e551) { if (flags & 1) fail("executable ELF stack"); stack = true; }
			}
			if (!mapped || !stack) fail("ELF entry mapping or nonexecutable stack missing");
			return "ELF64 static executable, x86-64 System V";
		}
		if (type != 1) fail("unsupported ELF file type");
		const auto table = r.integer(40, 8), width = r.integer(58, 2), count = r.integer(60, 2), names = r.integer(62, 2);
		if (width != 64 || !count || names >= count) fail("invalid ELF section table");
		r.require(table, count * width);
		const auto nameHeader = table + names * width, nameOffset = r.integer(nameHeader + 24, 8), nameSize = r.integer(nameHeader + 32, 8);
		for (std::uint64_t n = 0; n < count; ++n) {
			const auto p = table + n * width, kind = r.integer(p + 4, 4), offset = r.integer(p + 24, 8), size = r.integer(p + 32, 8);
			const auto link = r.integer(p + 40, 4), info = r.integer(p + 44, 4), alignment = r.integer(p + 48, 8);
			if (kind != 8) r.require(offset, size);
			if (link >= count || (alignment > 1 && (alignment & (alignment - 1)))) fail("invalid ELF section dependency/alignment");
			r.string(nameOffset, nameSize, r.integer(p, 4));
			if (kind == 2) {
				if (r.integer(p + 56, 8) != 24 || size % 24 || r.integer(table + link * width + 4, 4) != 3) fail("invalid ELF symbol/string table dependency");
				const auto strings = table + link * width;
				for (std::uint64_t s = 0; s < size; s += 24) {
					r.string(r.integer(strings + 24, 8), r.integer(strings + 32, 8), r.integer(offset + s, 4));
					const auto section = r.integer(offset + s + 6, 2);
					if (section && section < 0xff00 && section >= count) fail("ELF symbol references invalid section");
				}
			}
			if (kind == 4) {
				if (r.integer(p + 56, 8) != 24 || size % 24 || info >= count || r.integer(table + link * width + 4, 4) != 2) fail("invalid ELF relocation dependencies");
				const auto symbolCount = r.integer(table + link * width + 32, 8) / 24;
				const auto targetSize = r.integer(table + info * width + 32, 8);
				for (std::uint64_t relocation = 0; relocation < size; relocation += 24) {
					const auto address = r.integer(offset + relocation, 8), encoding = r.integer(offset + relocation + 8, 8);
					const auto relocationKind = encoding & UINT32_MAX;
					const auto relocationWidth = relocationKind == 1 ? 8u : 4u;
					if (address > targetSize || targetSize - address < relocationWidth || (encoding >> 32) >= symbolCount ||
						(relocationKind != 4 && relocationKind != 1 && relocationKind != 10)) fail("invalid ELF call/debug relocation");
				}
			}
		}
		return "ELF64 relocatable object, x86-64 System V";
	}
	r.require(0, 32);
	if (r.integer(0, 4) != 0xfeedfacf || r.integer(4, 4) != 0x01000007 || r.integer(12, 4) != 1) fail("unsupported Mach-O identification");
	const auto count = r.integer(16, 4), commands = r.integer(20, 4); r.require(32, commands);
	std::uint64_t cursor = 32, symbolCount = 0; bool symbolTable = false;
	struct Relocations { std::uint64_t offset, count, size; };
	std::vector<Relocations> relocations;
	for (std::uint64_t n = 0; n < count; ++n) {
		if (cursor > 32 + commands || 32 + commands - cursor < 8) fail("Mach-O command header exceeds table");
		const auto command = r.integer(cursor, 4), size = r.integer(cursor + 4, 4);
		if (size < 8 || size % 8 || size > 32 + commands - cursor) fail("invalid Mach-O load-command size");
		if (command == 0x19) {
			if (size < 72) fail("truncated Mach-O segment");
			const auto sections = r.integer(cursor + 64, 4);
			if (sections > (size - 72) / 80) fail("truncated Mach-O section table");
			r.require(r.integer(cursor + 40, 8), r.integer(cursor + 48, 8));
			for (std::uint64_t section = 0; section < sections; ++section) {
				const auto p = cursor + 72 + section * 80, length = r.integer(p + 40, 8), offset = r.integer(p + 48, 4);
				r.require(offset, length);
				const auto relocs = r.integer(p + 56, 4), total = r.integer(p + 60, 4); r.require(relocs, total * 8);
				relocations.push_back({ relocs,total,length });
			}
		}
		if (command == 2) {
			if (size != 24 || symbolTable) fail("invalid Mach-O symbol command");
			const auto offset = r.integer(cursor + 8, 4); symbolCount = r.integer(cursor + 12, 4);
			const auto strings = r.integer(cursor + 16, 4), length = r.integer(cursor + 20, 4); r.require(offset, symbolCount * 16);
			for (std::uint64_t symbol = 0; symbol < symbolCount; ++symbol) r.string(strings, length, r.integer(offset + symbol * 16, 4));
			symbolTable = true;
		}
		cursor += size;
	}
	if (cursor != 32 + commands || !symbolTable) fail("Mach-O command graph is incomplete");
	for (const auto& group : relocations) for (std::uint64_t n = 0; n < group.count; ++n) {
		const auto address = r.integer(group.offset + n * 8, 4), encoding = r.integer(group.offset + n * 8 + 4, 4);
		if (address > group.size || group.size - address < 4 || (encoding & 0xffffff) >= symbolCount || (encoding >> 24) != 0x2d) fail("invalid Mach-O external branch relocation");
	}
	return "Mach-O64 relocatable object, x86-64 System V";
}
} // namespace unix_output
