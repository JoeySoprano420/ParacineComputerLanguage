#pragma once

// Included inside namespace pcn; independent of frontend and machine IR.
namespace target_contract {
inline constexpr const char* revision = "paracine-targets-v1";
enum class Architecture { X86_64, ARM64 };
enum class OS { Windows, Linux, MacOS };
enum class ObjectFormat { COFF, ELF, MachO };
enum class ABI { MicrosoftX64, SystemVX64, WindowsARM64, AAPCS64, DarwinARM64 };
enum class Support { Implemented, Partial, Unsupported };
struct Feature { const char* name; Support support; const char* detail; };
struct Target {
	const char* name;
	Architecture architecture;
	OS os;
	ObjectFormat objectFormat;
	ABI abi;
	std::uint32_t machine;
	bool codegen;
	const char* platformABI;
	const char* relocationModel;
	const char* unwindModel;
	const char* debugModel;
	const char* tlsModel;
	const char* systemLibraries;
	bool windows() const { return os == OS::Windows; }
	bool systemV() const { return abi == ABI::SystemVX64; }
	bool elf() const { return objectFormat == ObjectFormat::ELF; }
	const char* format() const { return objectFormat == ObjectFormat::COFF ? "COFF/PE32+" : elf() ? "ELF64" : "Mach-O64"; }
};
inline constexpr Target targets[] = {
	{ "windows-x86_64", Architecture::X86_64, OS::Windows, ObjectFormat::COFF, ABI::MicrosoftX64, 0x8664, true,
	  "Microsoft x64 with Paracine rich-indirect-v1", "AMD64 REL32/ADDR32NB/SECREL/SECTION", "Windows x64 .pdata/.xdata", "CodeView/PDB", "PE TLS directory", "kernel32.lib;shell32.lib" },
	{ "linux-x86_64", Architecture::X86_64, OS::Linux, ObjectFormat::ELF, ABI::SystemVX64, 62, true,
	  "System V AMD64 scalar subset", "R_X86_64_PLT32/64/32", "DWARF .debug_frame (debug only)", "DWARF 4", "ELF TLS", "none for static scalar startup; external C libraries through system linker" },
	{ "macos-arm64", Architecture::ARM64, OS::MacOS, ObjectFormat::MachO, ABI::DarwinARM64, 0x0100000c, false,
	  "Darwin ARM64 (not implemented)", "not implemented", "not implemented", "not implemented", "Mach-O TLV (not implemented)", "not linked" },
	{ "linux-arm64", Architecture::ARM64, OS::Linux, ObjectFormat::ELF, ABI::AAPCS64, 183, false,
	  "AAPCS64 (not implemented)", "not implemented", "not implemented", "not implemented", "ELF TLS (not implemented)", "not linked" },
	{ "windows-arm64", Architecture::ARM64, OS::Windows, ObjectFormat::COFF, ABI::WindowsARM64, 0xaa64, false,
	  "Windows ARM64 (not implemented)", "not implemented", "not implemented", "not implemented", "PE TLS directory (not implemented)", "not linked" },
	{ "macos-x86_64", Architecture::X86_64, OS::MacOS, ObjectFormat::MachO, ABI::SystemVX64, 0x01000007, true,
	  "System V AMD64 scalar subset", "X86_64_RELOC_BRANCH", "not implemented", "not implemented", "Mach-O TLV (not implemented)", "external libraries through macOS linker only" }
};
inline const Target& get(std::string_view name) {
	for (const auto& target : targets) if (name == target.name) return target;
	throw std::runtime_error("E0540 unknown native target: " + std::string(name));
}
inline void requireCodegen(const Target& target) {
	if (!target.codegen) throw std::runtime_error("E0590 native code generation is not implemented for " + std::string(target.name) + "; target registration is not backend support");
}
inline bool debugSupported(const Target& target) { return target.codegen && target.os != OS::MacOS; }
inline bool executableSupported(const Target& target) { return target.codegen && target.os != OS::MacOS; }
inline std::vector<Feature> features(const Target& target) {
	if (!target.codegen) return {
		{ "abiClassification", Support::Unsupported, "No argument/result classifier or machine legalizer" },
		{ "objectFormat", Support::Unsupported, "Platform identity only; no object writer" },
		{ "relocations", Support::Unsupported, "No instruction fixups or relocation writer" },
		{ "unwind", Support::Unsupported, "No frame encoding" },
		{ "debug", Support::Unsupported, "No machine/source location encoding" },
		{ "tls", Support::Unsupported, "No language TLS storage or access lowering" },
		{ "exceptions", Support::Unsupported, "No exception propagation or cleanup ABI" },
		{ "systemLibraries", Support::Unsupported, "No runtime import adapter" },
		{ "startup", Support::Unsupported, "No process entry adapter" },
		{ "dynamicLinking", Support::Unsupported, "No native loader/linker integration" }
	};
	const bool windows = target.windows(), linux = target.os == OS::Linux;
	return {
		{ "abiClassification", Support::Partial, windows ? "Scalars and Paracine indirect rich values, not full foreign C aggregate classification" : "Scalar integer/floating calls and C variadics only" },
		{ "objectFormat", Support::Implemented, target.format() },
		{ "relocations", Support::Partial, target.relocationModel },
		{ "unwind", windows ? Support::Implemented : linux ? Support::Partial : Support::Unsupported, target.unwindModel },
		{ "debug", debugSupported(target) ? Support::Partial : Support::Unsupported, debugSupported(target) ? "Source lines/types; bounded O0 scalar locals; no optimized reconstruction" : "No debug emission" },
		{ "tls", Support::Unsupported, "No language TLS storage or access lowering" },
		{ "exceptions", Support::Unsupported, "Traps terminate; no exception propagation or cleanup ABI" },
		{ "systemLibraries", windows ? Support::Partial : Support::Unsupported, target.systemLibraries },
		{ "startup", windows || linux ? Support::Partial : Support::Unsupported, windows ? "PE scalar entry or UTF-8 argv adapter; no CRT initialization" : linux ? "Static no-argument entry, exit syscall; no libc initialization" : "External linker required" },
		{ "dynamicLinking", windows ? Support::Partial : Support::Unsupported, windows ? "PE imports; external COFF DLL linking; no language dlopen API" : "Relocatable external calls only; no integrated dynamic executable or shared-library writer" }
	};
}
inline const char* supportName(Support support) {
	return support == Support::Implemented ? "implemented" : support == Support::Partial ? "partial" : "unsupported";
}
inline std::string quote(std::string_view value) {
	std::ostringstream out; out << '"';
	for (unsigned char byte : value) {
		if (byte == '"' || byte == '\\') out << '\\' << static_cast<char>(byte);
		else if (byte < 32) out << "\\u00" << "0123456789abcdef"[byte >> 4] << "0123456789abcdef"[byte & 15];
		else out << static_cast<char>(byte);
	}
	out << '"'; return out.str();
}
inline void writeFeatures(std::ostream& out, const std::vector<Feature>& inventory) {
	out << '['; bool first = true;
	for (const auto& feature : inventory) {
		if (!first) out << ','; first = false;
		out << "{\"name\":" << quote(feature.name) << ",\"support\":" << quote(supportName(feature.support)) << ",\"detail\":" << quote(feature.detail) << '}';
	}
	out << ']';
}
inline std::string report(bool json, const Target* selected = nullptr) {
	std::ostringstream out; out.imbue(std::locale::classic());
	if (json) out << "{\"schema\":\"paracine.targets.v1\",\"revision\":" << quote(revision) << ",\"targets\":[";
	bool first = true;
	for (const auto& target : targets) {
		if (selected && selected != &target) continue;
		if (json) {
			if (!first) out << ','; first = false;
			out << "{\"name\":" << quote(target.name) << ",\"architecture\":" << quote(target.architecture == Architecture::X86_64 ? "x86_64" : "arm64")
				<< ",\"os\":" << quote(target.windows() ? "windows" : target.os == OS::Linux ? "linux" : "macos")
				<< ",\"pointerBytes\":8,\"stackAlignment\":16,\"endianness\":\"little\",\"machine\":" << target.machine
				<< ",\"codegen\":" << (target.codegen ? "true" : "false") << ",\"format\":" << quote(target.format())
				<< ",\"platformABI\":" << quote(target.platformABI) << ",\"platformTLS\":" << quote(target.tlsModel)
				<< ",\"platformDebug\":" << quote(target.debugModel) << ",\"platformUnwind\":" << quote(target.unwindModel)
				<< ",\"platformRelocations\":" << quote(target.relocationModel) << ",\"systemLibraries\":" << quote(target.systemLibraries) << ",\"features\":";
			writeFeatures(out, features(target)); out << '}';
		}
		else {
			out << target.name << " (" << target.format() << ", codegen=" << (target.codegen ? "yes" : "no") << ")\n";
			for (const auto& feature : features(target)) out << "  " << feature.name << ": " << supportName(feature.support) << " - " << feature.detail << '\n';
		}
	}
	if (json) out << "]}\n";
	return out.str();
}

enum class ValueClass { Integer, Float32, Float64, Indirect };
struct ArgumentLocation {
	bool floating = false, stack = false;
	std::size_t index = 0;
	bool duplicateInteger = false, promoteFloat = false;
};
struct CallLayout {
	std::vector<ArgumentLocation> arguments;
	std::size_t stackBytes = 0, vectorRegisters = 0;
};
inline CallLayout classify(const Target& target, const std::vector<ValueClass>& arguments, bool variadic = false, std::size_t fixedArguments = 0) {
	requireCodegen(target);
	if (variadic && fixedArguments > arguments.size()) throw std::runtime_error("E0591 variadic fixed argument count exceeds signature");
	CallLayout result;
	std::size_t integer = 0, floating = 0, stack = 0;
	for (std::size_t n = 0; n < arguments.size(); ++n) {
		const auto value = arguments[n];
		if (static_cast<unsigned>(value) > static_cast<unsigned>(ValueClass::Indirect)) throw std::runtime_error("E0591 unknown ABI argument class");
		const bool fp = value == ValueClass::Float32 || value == ValueClass::Float64;
		if (target.systemV() && value == ValueClass::Indirect) throw std::runtime_error("E0540 System V rich argument classification is not implemented");
		const bool promoted = variadic && n >= fixedArguments && value == ValueClass::Float32;
		if (target.abi == ABI::MicrosoftX64) {
			const bool spilled = n >= 4;
			result.arguments.push_back({ fp, spilled, spilled ? n - 4 : n, variadic && fp && !spilled, promoted });
			if (spilled) ++stack;
		}
		else if (target.systemV()) {
			auto& bank = fp ? floating : integer;
			const auto limit = fp ? 8u : 6u;
			const bool spilled = bank >= limit;
			result.arguments.push_back({ fp, spilled, spilled ? stack++ : bank++, false, promoted });
		}
		else throw std::runtime_error("E0591 no ABI argument classifier for target");
	}
	result.stackBytes = (target.abi == ABI::MicrosoftX64 ? 32 : 0) + stack * 8;
	result.stackBytes = (result.stackBytes + 15) & ~std::size_t{15};
	if (target.systemV()) result.vectorRegisters = floating;
	return result;
}
}

namespace runtime_contract {
inline constexpr const char* revision = "paracine-runtime-v1";
inline constexpr std::size_t maxFileBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t maxCommandBytes = 32760;
inline constexpr std::size_t maxStdinBytes = 1024u * 1024u;
inline constexpr std::size_t fileChunkBytes = 65536;
inline std::vector<target_contract::Feature> services(const target_contract::Target& target) {
	using target_contract::Support;
	if (!target.codegen) return {
		{ "allocation", Support::Unsupported, "No runtime allocator" }, { "deallocation", Support::Unsupported, "No runtime deallocator" },
		{ "panic", Support::Unsupported, "No trap lowering" }, { "stackOverflow", Support::Unsupported, "No stack probing or recovery" },
		{ "text", Support::Unsupported, "No native text runtime" }, { "printing", Support::Unsupported, "No stream adapter" },
		{ "fileIO", Support::Unsupported, "No filesystem adapter" }, { "environment", Support::Unsupported, "No environment API" },
		{ "threads", Support::Unsupported, "No language thread API" }, { "synchronization", Support::Unsupported, "No language synchronization API" },
		{ "startup", Support::Unsupported, "No native entry" }, { "dynamicLibraries", Support::Unsupported, "No language dynamic-library API" }
	};
	const bool windows = target.windows();
	return {
		{ "allocation", windows ? Support::Partial : Support::Unsupported, windows ? "Owned text uses GetProcessHeap/HeapAlloc; allocation failure traps; no cross-allocator ownership transfer" : "No allocation adapter" },
		{ "deallocation", windows ? Support::Partial : Support::Unsupported, windows ? "HeapFree on owning runtime scope; recursive aggregate drop glue is incomplete" : "No deallocation adapter" },
		{ "panic", Support::Implemented, "UD2 for language traps; no unwinding, panic recovery or guaranteed cleanup" },
		{ "stackOverflow", Support::Unsupported, "No stack probing or stack-overflow recovery; unwind metadata is not overflow protection" },
		{ "text", windows ? Support::Partial : Support::Unsupported, windows ? "UTF-8 text pointer/count (16 bytes), owned string pointer/count/capacity (24 bytes); no implicit NUL" : "No native text runtime" },
		{ "printing", windows ? Support::Partial : Support::Unsupported, windows ? "GetStdHandle/WriteFile; explicit byte counts; no C stdio buffering" : "No stream adapter" },
		{ "fileIO", windows ? Support::Partial : Support::Unsupported, windows ? "Strict UTF-8 paths converted to UTF-16; read/exists/write; read errors trap, write returns bool; 64 MiB limit" : "No filesystem adapter" },
		{ "environment", Support::Unsupported, "No language environment API; compiler environment snapshots are not program runtime services" },
		{ "threads", Support::Unsupported, "Compiler worker threads are not language runtime threads" },
		{ "synchronization", Support::Unsupported, "No language mutex, atomic or memory-order contract" },
		{ "startup", windows || target.os == target_contract::OS::Linux ? Support::Partial : Support::Unsupported, windows ? "PE entry and optional UTF-8 argv adapter; returns int; no CRT global constructors" : "Linux static scalar entry only; macOS requires external startup" },
		{ "dynamicLibraries", Support::Unsupported, "No language load/unload/symbol lookup or cross-library lifetime API" }
	};
}
inline std::string report(const target_contract::Target& target, bool json) {
	std::ostringstream out; out.imbue(std::locale::classic());
	if (json) {
		out << "{\"schema\":\"paracine.runtime.v1\",\"revision\":" << target_contract::quote(revision)
			<< ",\"target\":" << target_contract::quote(target.name) << ",\"maxFileBytes\":" << maxFileBytes
			<< ",\"maxCommandBytes\":" << maxCommandBytes << ",\"maxStdinBytes\":" << maxStdinBytes << ",\"fileChunkBytes\":" << fileChunkBytes << ",\"services\":";
		target_contract::writeFeatures(out, services(target)); out << "}\n";
	}
	else {
		out << revision << " for " << target.name << '\n';
		for (const auto& service : services(target)) out << "  " << service.name << ": " << target_contract::supportName(service.support) << " - " << service.detail << '\n';
	}
	return out.str();
}
}
