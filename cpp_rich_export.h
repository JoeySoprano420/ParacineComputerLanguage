#pragma once

static const char* portableRichRuntime() {
	return R"PCN(#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/wait.h>
#endif

namespace pcn_runtime {
[[noreturn]] inline void trap() { std::fputs("Paracine trap\n", stderr); std::fflush(stderr); std::abort(); }
struct Object;
struct Value {
	std::int64_t scalar = 0;
	std::shared_ptr<Object> object;
	Value() = default;
	Value(std::int64_t number) : scalar(number) {}
	operator std::int64_t() const { if (object) trap(); return scalar; }
};
struct Object {
	std::string type;
	std::shared_ptr<std::string> storage;
	std::weak_ptr<std::string> backing;
	std::string_view view;
	bool borrowed = false;
	int tag = 0;
	std::vector<std::string> names;
	std::vector<Value> fields;
	std::function<Value(const std::vector<Value>&)> call;
};
inline Value object(std::string type) { Value value; value.object = std::make_shared<Object>(); value.object->type = std::move(type); return value; }
inline std::string_view bytes(const Value& value) {
	if (!value.object) trap();
	if (value.object->borrowed) { if (value.object->backing.expired()) trap(); return value.object->view; }
	if (!value.object->storage) trap();
	return *value.object->storage;
}
inline Value text(std::string data, bool owned = false) {
	auto value = object(owned ? "string" : "text");
	value.object->storage = std::make_shared<std::string>(std::move(data)); return value;
}
inline Value borrow(const Value& source, std::size_t begin, std::size_t length) {
	const auto data = bytes(source);
	if (begin > data.size() || length > data.size() - begin) trap();
	auto value = object("text"); value.object->borrowed = true;
	value.object->backing = source.object->borrowed ? source.object->backing : source.object->storage;
	value.object->view = data.substr(begin, length);
	// Keep immutable literal backing alive without copying sliced bytes.
	if (source.object->type == "text" && source.object->storage) value.object->storage = source.object->storage;
	return value;
}
inline Value copy(const Value& source) {
	if (!source.object || source.object->call || source.object->type == "text" || source.object->type == "string") return source;
	auto value = object(source.object->type); *value.object = *source.object;
	for (auto& field : value.object->fields) field = copy(field);
	return value;
}
inline Value aggregate(std::string type, std::vector<std::string> names, std::vector<Value> fields) {
	auto value = object(std::move(type)); value.object->names = std::move(names); value.object->fields = std::move(fields); return value;
}
inline Value& field(Value& value, std::string_view name) {
	if (!value.object) trap();
	const auto found = std::find(value.object->names.begin(), value.object->names.end(), name);
	if (found == value.object->names.end()) trap();
	return value.object->fields.at(static_cast<std::size_t>(found - value.object->names.begin()));
}
inline Value& index(Value& value, std::int64_t position) {
	if (!value.object || position < 0 || static_cast<std::uint64_t>(position) >= value.object->fields.size()) trap();
	return value.object->fields[static_cast<std::size_t>(position)];
}
inline Value tagged(std::string type, int tag, std::vector<Value> payload) {
	auto value = object(std::move(type)); value.object->tag = tag; value.object->fields = std::move(payload); return value;
}
inline Value payload(const Value& value) { if (!value.object || value.object->fields.empty()) trap(); return value.object->fields.front(); }
inline std::int64_t tag(const Value& value) { if (!value.object) trap(); return value.object->tag; }
inline Value closure(std::string type, std::vector<Value> captures, std::function<Value(const std::vector<Value>&)> call, std::vector<std::string> names = {}) {
	auto value = object(std::move(type)); value.object->fields = std::move(captures); value.object->names = std::move(names); value.object->call = std::move(call); return value;
}
inline Value invoke(const Value& closure, std::vector<Value> arguments) {
	if (!closure.object || !closure.object->call) trap();
	auto captures = closure.object->fields; captures.insert(captures.end(), arguments.begin(), arguments.end());
	return closure.object->call(captures);
}
inline Value string_move(Value& source) {
	if (!source.object || source.object->type != "string" || !source.object->storage) trap();
	auto result = object("string"); result.object->storage = std::move(source.object->storage);
	source.object->storage = std::make_shared<std::string>(); return result;
}
inline void move_owner_path(Value& source, Value& destination, const std::vector<std::string_view>& path, std::size_t depth) {
	if (depth == path.size()) { destination = string_move(source); return; }
	Value& sourceField = field(source, path[depth]);
	Value& destinationField = field(destination, path[depth]);
	if (!sourceField.object) trap();
	if (destinationField.object == sourceField.object) {
		auto copy = object(sourceField.object->type); *copy.object = *sourceField.object;
		destinationField = std::move(copy);
	}
	move_owner_path(sourceField, destinationField, path, depth + 1);
}
inline Value closure_move(Value& source, std::initializer_list<std::string_view> ownerPaths) {
	if (!source.object || !source.object->type.starts_with("closure$")) trap();
	auto result = object(source.object->type); *result.object = *source.object;
	for (const auto path : ownerPaths) {
		std::vector<std::string_view> fields;
		std::size_t begin = 0;
		while (begin <= path.size()) {
			const auto end = path.find('.', begin);
			fields.push_back(path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin));
			if (end == std::string_view::npos) break;
			begin = end + 1;
		}
		move_owner_path(source, result, fields, 0);
	}
	return result;
}
inline void dispose(Value& source) { if (!source.object || source.object->type != "string") trap(); source.object->storage = std::make_shared<std::string>(); }
inline void clear(Value& source) { if (!source.object || source.object->type != "string" || !source.object->storage) trap(); source.object->storage->clear(); }
inline void append(Value& source, const Value& added) {
	if (!source.object || source.object->type != "string") trap();
	auto data = std::string(bytes(source)); const auto extra = bytes(added);
	if (extra.size() > data.max_size() - data.size()) trap(); data.append(extra);
	source.object->storage = std::make_shared<std::string>(std::move(data));
}
inline std::int64_t add(std::int64_t a, std::int64_t b) { return std::bit_cast<std::int64_t>(std::uint64_t(a) + std::uint64_t(b)); }
inline std::int64_t sub(std::int64_t a, std::int64_t b) { return std::bit_cast<std::int64_t>(std::uint64_t(a) - std::uint64_t(b)); }
inline std::int64_t mul(std::int64_t a, std::int64_t b) { return std::bit_cast<std::int64_t>(std::uint64_t(a) * std::uint64_t(b)); }
inline std::int64_t div(std::int64_t a, std::int64_t b) { if (!b) trap(); if (a == INT64_MIN && b == -1) return a; return a / b; }
inline std::int64_t mod(std::int64_t a, std::int64_t b) { if (!b) trap(); if (a == INT64_MIN && b == -1) return 0; return a % b; }
inline std::int64_t shl(std::int64_t a, std::int64_t b) { return std::bit_cast<std::int64_t>(std::uint64_t(a) << (std::uint64_t(b) & 63)); }
inline std::int64_t shr(std::int64_t a, std::int64_t b) { return a >> (std::uint64_t(b) & 63); }
inline std::int64_t fib(std::int64_t n) {
	const bool negative = n < 0; const auto magnitude = negative ? 0 - std::uint64_t(n) : std::uint64_t(n);
	if (magnitude > 92) trap(); std::int64_t a = 0, b = 1;
	for (std::uint64_t i = 1; i < magnitude; ++i) { const auto next = a + b; a = b; b = next; }
	const auto value = magnitude ? b : 0; return negative && magnitude % 2 == 0 ? -value : value;
}
inline std::int64_t byte_at(const Value& value, std::int64_t position) {
	const auto data = bytes(value); if (position < 0 || std::uint64_t(position) >= data.size()) trap(); return static_cast<unsigned char>(data[std::size_t(position)]);
}
inline std::int64_t find_byte(const Value& value, std::int64_t byte) {
	if (byte < 0 || byte > 255) return -1; const auto position = bytes(value).find(static_cast<char>(byte)); return position == std::string_view::npos ? -1 : std::int64_t(position);
}
inline std::int64_t find_text(const Value& value, const Value& needle, bool reverse = false) {
	const auto position = reverse ? bytes(value).rfind(bytes(needle)) : bytes(value).find(bytes(needle)); return position == std::string_view::npos ? -1 : std::int64_t(position);
}
inline std::int64_t compare(const Value& a, const Value& b) {
	const auto x = bytes(a), y = bytes(b); const auto count = std::min(x.size(), y.size());
	for (std::size_t i = 0; i < count; ++i) if (static_cast<unsigned char>(x[i]) != static_cast<unsigned char>(y[i])) return static_cast<unsigned char>(x[i]) < static_cast<unsigned char>(y[i]) ? -1 : 1;
	return x.size() == y.size() ? 0 : x.size() < y.size() ? -1 : 1;
}
inline Value slice(const Value& source, std::int64_t begin, std::int64_t length) {
	const auto data = bytes(source);
	if (begin < 0 || length < 0 || std::uint64_t(begin) > data.size() || std::uint64_t(length) > data.size() - std::size_t(begin)) trap();
	const auto end = std::size_t(begin) + std::size_t(length);
	const auto boundary = [&](std::size_t at) { return at == data.size() || (static_cast<unsigned char>(data[at]) & 0xc0) != 0x80; };
	if (!boundary(std::size_t(begin)) || !boundary(end)) trap(); return borrow(source, std::size_t(begin), std::size_t(length));
}
inline std::int64_t hash(const Value& value) { std::uint64_t h = 14695981039346656037ull; for (const unsigned char byte : bytes(value)) { h ^= byte; h *= 1099511628211ull; } return std::bit_cast<std::int64_t>(h); }
inline std::int64_t codepoint(const Value& value, std::int64_t index) {
	const auto data = bytes(value); if (index < 0 || std::uint64_t(index) >= data.size()) trap(); const auto at = std::size_t(index);
	const auto lead = static_cast<unsigned char>(data[at]); unsigned count = 0; std::uint32_t scalar = 0, minimum = 0;
	if (lead < 128) return lead;
	if (lead >= 0xc2 && lead <= 0xdf) { count = 1; scalar = lead & 31; minimum = 128; }
	else if (lead >= 0xe0 && lead <= 0xef) { count = 2; scalar = lead & 15; minimum = 2048; }
	else if (lead >= 0xf0 && lead <= 0xf4) { count = 3; scalar = lead & 7; minimum = 65536; }
	else trap();
	if (data.size() - at <= count) trap();
	for (unsigned i = 1; i <= count; ++i) { const auto byte = static_cast<unsigned char>(data[at + i]); if ((byte & 0xc0) != 0x80) trap(); scalar = (scalar << 6) | (byte & 63); }
	if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) trap(); return scalar;
}
inline std::int64_t codepoints(const Value& value) { std::int64_t count = 0; for (unsigned char byte : bytes(value)) if ((byte & 0xc0) != 0x80) ++count; return count; }
inline bool ascii(const Value& value) { for (unsigned char byte : bytes(value)) if (byte >= 128) return false; return true; }
inline void validate_utf8(const Value& value) {
	const auto data = bytes(value);
	for (std::size_t index = 0; index < data.size();) {
		const auto scalar = codepoint(value, std::int64_t(index));
		index += scalar < 128 ? 1 : scalar < 2048 ? 2 : scalar < 65536 ? 3 : 4;
	}
}
inline bool write(const Value& value, bool error) {
	const auto data = bytes(value); auto* file = error ? stderr : stdout;
	const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
	return std::fflush(file) == 0 && written;
}
inline void print(const Value& value, bool newline) { if (value.object) { if (!write(value, false)) trap(); } else if (std::printf("%lld", static_cast<long long>(value.scalar)) < 0) trap(); if (newline && std::putchar('\n') == EOF) trap(); }
inline Value read_stream() {
	std::string data; char buffer[4096]; constexpr std::size_t limit = 1048576;
	for (;;) { const auto count = std::fread(buffer, 1, sizeof(buffer), stdin); if (count > limit - data.size()) trap(); data.append(buffer, count); if (count != sizeof(buffer)) { if (std::ferror(stdin)) trap(); break; } }
	return text(std::move(data), true);
}
inline std::filesystem::path path(const Value& value) {
	const auto data = bytes(value); if (data.empty() || data.find('\0') != std::string_view::npos) trap(); validate_utf8(value);
	std::u8string utf8; for (unsigned char byte : data) utf8.push_back(static_cast<char8_t>(byte)); return std::filesystem::path(utf8);
}
inline Value read_file(const Value& value) {
	std::ifstream file(path(value), std::ios::binary); if (!file) trap();
	std::string data; char buffer[16384]; constexpr std::size_t limit = 64u * 1024u * 1024u;
	while (file) { file.read(buffer, sizeof(buffer)); const auto count = std::size_t(file.gcount()); if (count > limit - data.size()) trap(); data.append(buffer, count); }
	if (file.bad()) trap(); auto result = text(std::move(data), true); validate_utf8(result); return result;
}
inline bool exists(const Value& value) { std::error_code error; const bool found = std::filesystem::exists(path(value), error); if (error && error != std::errc::no_such_file_or_directory) trap(); return found; }
inline bool write_file(const Value& target, const Value& value) {
	const auto target_path = path(target); const auto data = bytes(value); if (data.size() > 64u * 1024u * 1024u) trap();
	std::ofstream file(target_path, std::ios::binary | std::ios::trunc); if (!file) return false;
	file.write(data.data(), static_cast<std::streamsize>(data.size())); file.flush(); return file.good();
}
inline std::int64_t process(const Value& value) {
	const auto command = bytes(value); if (command.empty() || command.size() > 32760 || command.find('\0') != std::string_view::npos) trap(); validate_utf8(value);
#ifdef _WIN32
	const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.data(), static_cast<int>(command.size()), nullptr, 0);
	if (count <= 0) trap(); std::wstring wide(std::size_t(count), L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.data(), static_cast<int>(command.size()), wide.data(), count)) trap();
	wide.push_back(L'\0'); STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
	if (!CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child)) return -1;
	const auto waited = WaitForSingleObject(child.hProcess, INFINITE); DWORD exit = 0xffffffff;
	const bool complete = waited == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &exit);
	CloseHandle(child.hThread); CloseHandle(child.hProcess); return complete ? std::int64_t(exit) : -1;
#else
	const auto status = std::system(std::string(command).c_str());
#if defined(__linux__)
	return status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#else
	return status;
#endif
#endif
}
inline std::int64_t checked_cast(std::int64_t value, bool) { if (value < 0) trap(); return value; }
template<class T> inline std::int64_t floating(std::int64_t a, std::int64_t b, int op) {
	T x, y; if constexpr (sizeof(T) == 4) { x = std::bit_cast<float>(std::uint32_t(a)); y = std::bit_cast<float>(std::uint32_t(b)); } else { x = std::bit_cast<double>(a); y = std::bit_cast<double>(b); }
	if (op == 4) return x == y; if (op == 5) return x != y; if (op == 6) return x < y; if (op == 7) return x <= y; if (op == 8) return x > y; if (op == 9) return x >= y;
	T result = op == 0 ? x + y : op == 1 ? x - y : op == 2 ? x * y : x / y;
	if constexpr (sizeof(T) == 4) return std::int64_t(std::bit_cast<std::uint32_t>(result)); else return std::bit_cast<std::int64_t>(result);
}
} // namespace pcn_runtime
using pcn_runtime::Value;

)PCN";
}

class RichCppIREmitter {
public:
	static std::string functionName(std::string_view source) { return name(source); }
	std::string emit(const IRProgram& program) const {
		const auto errors = PCIRVerifier{}.verify(program);
		if (!errors.empty()) throw std::runtime_error("C++ export requires verified PCIR: " + errors.front());
		RichABIResolver abi(program);
		std::map<std::string, const IRFunction*> functions;
		for (const auto& function : program.functions) {
			if (!functions.emplace(function.name, &function).second) throw std::runtime_error("duplicate C++ export routine: " + function.name);
		}
		std::ostringstream out;
		out << portableRichRuntime();
		for (const auto& function : program.functions) { prototype(out, function); out << ";\n"; }
		for (const auto& function : program.functions) {
			prototype(out, function); out << " {\n    std::uint32_t predecessor = 0;\n";
			for (std::uint32_t reg = 1; reg <= function.totalVregs; ++reg) out << "    Value v" << reg << ";\n";
			for (std::size_t index = 0; index < function.parameterVregs.size(); ++index)
				out << "    " << value(function.parameterVregs[index]) << " = p" << index << ";\n";
			if (!function.blocks.empty()) out << "    goto " << label(function.blocks.front().id) << ";\n";
			for (const auto& block : function.blocks) {
				out << label(block.id) << ": {\n";
				for (std::size_t index = 0; index < block.phiNodes.size(); ++index) {
					const auto& phi = block.phiNodes[index];
					out << "    Value phi" << index << ";\n    switch (predecessor) {\n";
					for (const auto& [from, source] : phi.phiInputs) out << "    case " << std::uint32_t(from) << ": phi" << index << " = " << value(source) << "; break;\n";
					out << "    default: pcn_runtime::trap();\n    }\n";
				}
				for (std::size_t index = 0; index < block.phiNodes.size(); ++index) out << "    " << value(block.phiNodes[index].dest) << " = phi" << index << ";\n";
				for (const auto& instruction : block.instructions) emitInstruction(out, instruction, block.id, functions, abi);
				out << "}\n";
			}
			out << "    pcn_runtime::trap();\n}\n\n";
		}
		const auto entry = functions.find("main");
		if (entry == functions.end() || !entry->second->parameterVregs.empty()) throw std::runtime_error("C++ export requires a zero-argument main routine");
		out << "int main() { return static_cast<int>(static_cast<std::int64_t>(" << name("main") << "())); }\n";
		return out.str();
	}
private:
	static std::string name(std::string_view source) {
		static constexpr char hex[] = "0123456789abcdef";
		std::string result = "pcn_f_";
		for (unsigned char byte : source) { result += hex[byte >> 4]; result += hex[byte & 15]; }
		return result;
	}
	static std::string quote(std::string_view source) {
		std::string result = "\"";
		for (unsigned char byte : source) {
			result += '\\'; result += char('0' + ((byte >> 6) & 7)); result += char('0' + ((byte >> 3) & 7)); result += char('0' + (byte & 7));
		}
		return result + "\"";
	}
	static std::string value(VReg reg) { return "v" + std::to_string(std::uint32_t(reg)); }
	static std::string label(BlockID block) { return "bb_" + std::to_string(std::uint32_t(block)); }
	static void prototype(std::ostringstream& out, const IRFunction& function) {
		out << "static Value " << name(function.name) << '(';
		for (std::size_t index = 0; index < function.parameterNames.size(); ++index) { if (index) out << ','; out << "Value p" << index; }
		out << ')';
	}
	static std::string arguments(const IRInstruction& instruction, std::size_t begin = 0) {
		std::string result;
		for (auto index = begin; index < instruction.args.size(); ++index) { if (index != begin) result += ','; result += value(instruction.args[index]); }
		return result;
	}
	static void emitInstruction(std::ostringstream& out, const IRInstruction& i, BlockID block,
		const std::map<std::string, const IRFunction*>& functions, const RichABIResolver& abi) {
		auto arg = [&](std::size_t index) { return value(i.args.at(index)); };
		auto assign = [&](const std::string& expression) { out << "    " << (i.dest == VReg::None ? std::string{} : value(i.dest) + " = ") << expression << ";\n"; };
		auto helper = [&](const char* function) { assign(std::string("pcn_runtime::") + function + '(' + arguments(i) + ')'); };
		auto binary = [&](const char* op) { assign("static_cast<std::int64_t>(" + arg(0) + ") " + op + " static_cast<std::int64_t>(" + arg(1) + ")"); };
		if (i.sourceSpan) out << "    // source " << i.sourceSpan->start.line << ':' << i.sourceSpan->start.column << "\n";
		const bool single = i.valueType.kind == IRValueKind::Float32 || (!i.argumentValueTypes.empty() && i.argumentValueTypes.front().kind == IRValueKind::Float32);
		const bool floating = single || i.valueType.kind == IRValueKind::Float64 || (!i.argumentValueTypes.empty() && i.argumentValueTypes.front().kind == IRValueKind::Float64);
		if (floating) {
			int operation = -1;
			switch (i.op) { case IROpcode::Add: operation = 0; break; case IROpcode::Sub: operation = 1; break; case IROpcode::Mul: operation = 2; break; case IROpcode::Div: operation = 3; break; case IROpcode::Eq: operation = 4; break; case IROpcode::Ne: operation = 5; break; case IROpcode::Lt: operation = 6; break; case IROpcode::Le: operation = 7; break; case IROpcode::Gt: operation = 8; break; case IROpcode::Ge: operation = 9; break; default: break; }
			if (operation >= 0) { assign(std::string("pcn_runtime::floating<") + (single ? "float" : "double") + ">(" + arguments(i) + ',' + std::to_string(operation) + ')'); return; }
		}
		switch (i.op) {
		case IROpcode::ConstInt: assign("std::bit_cast<std::int64_t>(std::uint64_t{" + std::to_string(std::uint64_t(parseParacineInteger(i.literalText.empty() ? std::to_string(i.immediate) : i.literalText))) + "ull})"); break;
		case IROpcode::ConstBool: assign(i.immediate ? "1" : "0"); break;
		case IROpcode::ConstNull: assign("0"); break;
		case IROpcode::ConstFloat: {
			std::uint64_t bits = 0;
			if (single) bits = std::bit_cast<std::uint32_t>(parseParacineFloating<float>(i.literalText));
			else bits = std::bit_cast<std::uint64_t>(parseParacineFloating<double>(i.literalText));
			assign("std::bit_cast<std::int64_t>(std::uint64_t{" + std::to_string(bits) + "ull})"); break;
		}
		case IROpcode::ConstText: assign("pcn_runtime::text(std::string(" + quote(i.literalText) + ',' + std::to_string(i.literalText.size()) + "))"); break;
		case IROpcode::Copy: helper("copy"); break;
		case IROpcode::Add: helper("add"); break; case IROpcode::Sub: helper("sub"); break; case IROpcode::Mul: helper("mul"); break;
		case IROpcode::Div: helper("div"); break; case IROpcode::Mod: helper("mod"); break; case IROpcode::Shl: helper("shl"); break; case IROpcode::Shr: helper("shr"); break;
		case IROpcode::BitAnd: binary("&"); break; case IROpcode::BitOr: binary("|"); break; case IROpcode::BitXor: binary("^"); break;
		case IROpcode::Neg:
			if (floating) assign("std::bit_cast<std::int64_t>(std::uint64_t(static_cast<std::int64_t>(" + arg(0) + ")) ^ " + (single ? "0x80000000ull" : "0x8000000000000000ull") + ')');
			else assign("pcn_runtime::sub(0," + arg(0) + ')');
			break;
		case IROpcode::BitNot: assign("~static_cast<std::int64_t>(" + arg(0) + ')'); break;
		case IROpcode::Not: assign("!static_cast<std::int64_t>(" + arg(0) + ')'); break;
		case IROpcode::Eq: binary("=="); break; case IROpcode::Ne: binary("!="); break; case IROpcode::Lt: binary("<"); break; case IROpcode::Le: binary("<="); break; case IROpcode::Gt: binary(">"); break; case IROpcode::Ge: binary(">="); break; case IROpcode::And: binary("&&"); break; case IROpcode::Or: binary("||"); break;
		case IROpcode::Fibonacci: helper("fib"); break;
		case IROpcode::Branch: out << "    predecessor = " << std::uint32_t(block) << "; goto " << label(i.targetBlocks.at(0)) << ";\n"; break;
		case IROpcode::BranchIf: out << "    predecessor = " << std::uint32_t(block) << "; if (static_cast<std::int64_t>(" << arg(0) << ")) goto " << label(i.targetBlocks.at(0)) << "; else goto " << label(i.targetBlocks.at(1)) << ";\n"; break;
		case IROpcode::Call:
			if (i.callee == "print" || i.callee == "println") { out << "    pcn_runtime::print(" << arg(0) << ',' << (i.callee == "println" ? "true" : "false") << ");\n"; }
			else { if (!functions.contains(i.callee)) throw std::runtime_error("C++ export requires an explicit foreign binding for '" + i.callee + "'"); assign(name(i.callee) + '(' + arguments(i) + ')'); }
			break;
		case IROpcode::MakeClosure: {
			const auto layout = abi.layout(i.valueType);
			const auto target = functions.find(i.callee);
			if (target == functions.end() || layout.kind != ABIRepresentationKind::Closure || layout.fields.size() != i.args.size()) throw std::runtime_error("C++ export invalid closure environment for '" + i.callee + "'");
			std::string call = "pcn_runtime::closure(" + quote(i.valueType.name) + ",{" + arguments(i) + "},[](const std::vector<Value>& a) -> Value { if (a.size() != " + std::to_string(target->second->parameterNames.size()) + ") pcn_runtime::trap(); return " + name(i.callee) + '(';
			for (std::size_t index = 0; index < target->second->parameterNames.size(); ++index) { if (index) call += ','; call += "a[" + std::to_string(index) + ']'; }
			std::string fields;
			for (const auto& field : layout.fields) { if (!fields.empty()) fields += ','; fields += quote(field.name); }
			assign(call + "); },{" + fields + "})"); break;
		}
		case IROpcode::CallClosure: assign("pcn_runtime::invoke(" + arg(0) + ",{" + arguments(i, 1) + "})"); break;
		case IROpcode::TextToString: assign("pcn_runtime::text(std::string(pcn_runtime::bytes(" + arg(0) + ")),true)"); break;
		case IROpcode::StringToText: assign("pcn_runtime::borrow(" + arg(0) + ",0,pcn_runtime::bytes(" + arg(0) + ").size())"); break;
		case IROpcode::StringMove: helper("string_move"); break;
		case IROpcode::ClosureMove: {
			const auto layout = abi.layout(i.valueType);
			if (layout.kind != ABIRepresentationKind::Closure) throw std::runtime_error("C++ export closure.move requires a concrete environment");
			std::vector<std::string> ownerPaths;
			std::function<void(const std::string&, const std::string&, std::set<std::string>&)> collect =
				[&](const std::string& type, const std::string& prefix, std::set<std::string>& active) {
					if (type == "string") { ownerPaths.push_back(prefix); return; }
					if (type.empty() || !active.insert(type).second) return;
					const auto nested = abi.layout(type);
					if (nested.kind == ABIRepresentationKind::Closure || nested.kind == ABIRepresentationKind::Aggregate)
						for (const auto& field : nested.fields) if (!field.byReference)
							collect(field.typeName, prefix + "." + field.name, active);
					active.erase(type);
				};
			for (const auto& field : layout.fields) if (!field.byReference) {
				std::set<std::string> active; collect(field.typeName, field.name, active);
			}
			std::string paths;
			for (const auto& path : ownerPaths) { if (!paths.empty()) paths += ','; paths += quote(path); }
			assign("pcn_runtime::closure_move(" + arg(0) + ",{" + paths + "})"); break;
		}
		case IROpcode::StringDispose: out << "    pcn_runtime::dispose(" << arg(0) << ");\n"; break;
		case IROpcode::StringClear: out << "    pcn_runtime::clear(" << arg(0) << ");\n"; break;
		case IROpcode::StringAppend: out << "    pcn_runtime::append(" << arguments(i) << ");\n"; break;
		case IROpcode::ByteCount: assign("static_cast<std::int64_t>(pcn_runtime::bytes(" + arg(0) + ").size())"); break;
		case IROpcode::TextEquals: assign("pcn_runtime::bytes(" + arg(0) + ") == pcn_runtime::bytes(" + arg(1) + ')'); break;
		case IROpcode::TextByteAt: helper("byte_at"); break; case IROpcode::TextFindByte: helper("find_byte"); break;
		case IROpcode::TextStartsWith: assign("pcn_runtime::bytes(" + arg(0) + ").starts_with(pcn_runtime::bytes(" + arg(1) + "))"); break;
		case IROpcode::TextEndsWith: assign("pcn_runtime::bytes(" + arg(0) + ").ends_with(pcn_runtime::bytes(" + arg(1) + "))"); break;
		case IROpcode::TextSlice: helper("slice"); break; case IROpcode::TextHash64: helper("hash"); break;
		case IROpcode::TextCodepointAt: helper("codepoint"); break; case IROpcode::TextCodepointCount: helper("codepoints"); break; case IROpcode::TextIsAscii: helper("ascii"); break;
		case IROpcode::TextFindText: helper("find_text"); break; case IROpcode::TextCompare: helper("compare"); break;
		case IROpcode::TextRFindText: assign("pcn_runtime::find_text(" + arguments(i) + ",true)"); break;
		case IROpcode::TextStreamWrite: assign("pcn_runtime::write(" + arg(0) + ',' + (i.literalText == "stderr" ? "true" : "false") + ')'); break;
		case IROpcode::TextStreamRead: helper("read_stream"); break;
		case IROpcode::FsReadText: helper("read_file"); break; case IROpcode::FsExists: helper("exists"); break; case IROpcode::FsWriteText: helper("write_file"); break; case IROpcode::ProcessRun: helper("process"); break;
		case IROpcode::NumericCheckedCast: assign("pcn_runtime::checked_cast(" + arg(0) + ',' + (i.valueType.name == "size" ? "true" : "false") + ')'); break;
		case IROpcode::MakeAggregate: {
			std::string names;
			for (const auto& field : i.argumentTypes) { if (!names.empty()) names += ','; names += quote(field); }
			assign("pcn_runtime::aggregate(" + quote(i.typeName) + ",{" + names + "},{" + arguments(i) + "})"); break;
		}
		case IROpcode::ExtractField: assign("pcn_runtime::field(" + arg(0) + ',' + quote(i.literalText) + ')'); break;
		case IROpcode::StoreField: out << "    pcn_runtime::field(" << arg(0) << ',' << quote(i.literalText) << ") = pcn_runtime::copy(" << arg(1) << ");\n"; break;
		case IROpcode::ArrayIndex: helper("index"); break;
		case IROpcode::ArrayStore: out << "    pcn_runtime::index(" << arg(0) << ',' << arg(1) << ") = pcn_runtime::copy(" << arg(2) << ");\n"; break;
		case IROpcode::ResultOk: case IROpcode::ResultErr: case IROpcode::OptionSome: case IROpcode::OptionNone:
			assign("pcn_runtime::tagged(" + quote(i.valueType.name) + ',' + (i.op == IROpcode::ResultErr || i.op == IROpcode::OptionSome ? "1" : "0") + ",{" + arguments(i) + "})"); break;
		case IROpcode::ResultIsErr: helper("tag"); break;
		case IROpcode::ResultTry: out << "    if (pcn_runtime::tag(" << arg(0) << ")) return " << arg(0) << ";\n"; helper("payload"); break;
		case IROpcode::Return: out << "    return " << (i.args.empty() ? "Value{}" : arg(0)) << ";\n"; break;
		case IROpcode::Phi: throw std::runtime_error("C++ export phi must be attached to its block");
		case IROpcode::Nop: break;
		default: throw std::runtime_error("C++ export unsupported PCIR opcode " + std::to_string(static_cast<int>(i.op)));
		}
	}
};
