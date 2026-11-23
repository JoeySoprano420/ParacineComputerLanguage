#pragma once

// Included after rich-value legalization, inside namespace pcn.
namespace native_abi {
inline constexpr const char* revision = "paracine-dev-abi-1";
inline constexpr const char* windowsContract = "paracine-dev-abi-1;ms-x64;le;p64;module-v1;PCG1;rich-indirect-v1;text16;string24;no-exceptions";
inline constexpr const char* systemVContract = "paracine-dev-abi-1;sysv-x64-scalar;le;p64;module-v1;PCG1;no-exceptions";
inline std::string quote(const std::string& value) {
	std::ostringstream out; out << '"';
	for (unsigned char c : value) {
		if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
		else if (c < 32) out << "\\u00" << "0123456789abcdef"[c >> 4] << "0123456789abcdef"[c & 15];
		else out << static_cast<char>(c);
	}
	out << '"'; return out.str();
}
inline std::string layoutIdentity(const ABITypeLayout& layout) {
	std::ostringstream out; out.imbue(std::locale::classic());
	auto field = [&](const std::string& text) { out << text.size() << ':' << text; };
	field(abiKindName(layout.kind)); out << ':' << layout.size << ':' << layout.alignment << ':' << layout.rich
		<< ':' << layout.signedScalar << ':' << layout.tagOffset << ':' << layout.payloadOffset << ':';
	field(layout.valueType); field(layout.okType); field(layout.errType); field(layout.closureCallee);
	for (const auto& member : layout.fields) { field(member.name); field(member.typeName); out << ':' << member.offset << ':' << member.byReference << ';'; }
	return out.str();
}
inline std::string typeIdentity(const IRProgram& program, const ABITypeLayout& layout) {
	auto identity = layoutIdentity(layout);
	for (const auto& fact : program.skm.layouts) if (fact.typeName == layout.typeName)
		for (const auto& [name, value] : std::map<std::string, std::uint64_t>(fact.tagConstants.begin(), fact.tagConstants.end()))
			identity += ";tag:" + std::to_string(name.size()) + ":" + name + "=" + std::to_string(value);
	return identity;
}
inline std::string manifest(const IRProgram& program, bool systemV) {
	RichABIResolver abi(program); std::ostringstream out; out.imbue(std::locale::classic());
	out << "{\"schema\":\"paracine.abi.v1\",\"revision\":" << quote(revision)
		<< ",\"contract\":" << quote(systemV ? systemVContract : windowsContract)
		<< ",\"crossCompilerStable\":false,\"pointerBytes\":8,\"endianness\":\"little\",\"types\":[";
	bool first = true;
	for (const auto& name : collectABITypeNames(program)) {
		if (!first) out << ','; first = false;
		const auto layout = abi.layout(name);
		out << "{\"name\":" << quote(name) << ",\"kind\":" << quote(abiKindName(layout.kind))
			<< ",\"size\":" << layout.size << ",\"alignment\":" << layout.alignment
			<< ",\"indirect\":" << (layout.rich ? "true" : "false")
			<< ",\"layoutSHA256\":" << quote(PackageSHA256::hex(PackageSHA256::digest(typeIdentity(program, layout))))
			<< ",\"tagOffset\":" << layout.tagOffset << ",\"payloadOffset\":" << layout.payloadOffset
			<< ",\"valueType\":" << quote(layout.valueType) << ",\"okType\":" << quote(layout.okType)
			<< ",\"errorType\":" << quote(layout.errType) << ",\"closureTarget\":" << quote(layout.closureCallee) << ",\"fields\":[";
		bool memberFirst = true;
		for (const auto& member : layout.fields) {
			if (!memberFirst) out << ','; memberFirst = false;
			out << "{\"name\":" << quote(member.name) << ",\"type\":" << quote(member.typeName)
				<< ",\"offset\":" << member.offset << ",\"byReference\":" << (member.byReference ? "true" : "false") << '}';
		}
		out << "],\"tags\":[";
		bool tagFirst = true;
		std::map<std::string, std::uint64_t> builtinTags;
		if (layout.kind == ABIRepresentationKind::Result) builtinTags = { {"ok",0},{"error",1} };
		else if (layout.kind == ABIRepresentationKind::OptionTagged) builtinTags = { {"none",0},{"some",1} };
		else if (layout.kind == ABIRepresentationKind::OptionBoolNiche) builtinTags = { {"none",2},{"false",0},{"true",1} };
		else if (layout.kind == ABIRepresentationKind::OptionCharNiche) builtinTags = { {"none",0x110000} };
		else if (layout.kind == ABIRepresentationKind::OptionPointerNiche) builtinTags = { {"none",0} };
		for (const auto& [tag, value] : builtinTags) {
			if (!tagFirst) out << ','; tagFirst = false;
			out << "{\"name\":" << quote(tag) << ",\"value\":" << value << '}';
		}
		for (const auto& fact : program.skm.layouts) if (fact.typeName == name)
			for (const auto& [tag, value] : std::map<std::string, std::uint64_t>(fact.tagConstants.begin(), fact.tagConstants.end())) {
				if (!tagFirst) out << ','; tagFirst = false;
				out << "{\"name\":" << quote(tag) << ",\"value\":" << value << '}';
			}
		out << "]}";
	}
	out << "],\"functions\":["; first = true;
	std::map<std::string, const IRFunction*> functions;
	for (const auto& function : program.functions) functions.emplace(function.name, &function);
	for (const auto& [name, function] : functions) {
		if (!first) out << ','; first = false;
		auto scalar = [](const IRValueType& type) { return type.kind == IRValueKind::Void || type.kind == IRValueKind::Bool || type.kind == IRValueKind::Int64 || type.kind == IRValueKind::Null || type.kind == IRValueKind::Float32 || type.kind == IRValueKind::Float64; };
		bool supported = !systemV || scalar(function->valueType);
		if (systemV) for (std::size_t n = 0; n < function->parameterValueTypes.size(); ++n)
			supported = supported && scalar(function->parameterValueTypes[n]) && function->parameterModes.at(n) == "shared";
		out << "{\"symbol\":" << quote(name) << ",\"returnType\":" << quote(function->returnType)
			<< ",\"supported\":" << (supported ? "true" : "false")
			<< ",\"sret\":" << (!systemV && abi.isRich(function->valueType) ? "true" : "false") << ",\"parameters\":[";
		for (std::size_t n = 0; n < function->parameterTypes.size(); ++n) {
			if (n) out << ',';
			out << "{\"name\":" << quote(function->parameterNames.at(n)) << ",\"type\":" << quote(function->parameterTypes[n])
				<< ",\"mode\":" << quote(function->parameterModes.at(n)) << '}';
		}
		out << "]}";
	}
	out << "]}\n"; return out.str();
}
inline std::string coffDirectives(const IRProgram& program) {
	RichABIResolver abi(program);
	std::string result = std::string(" /FAILIFMISMATCH:\"ParacineABI=") + windowsContract + "\"";
	for (const auto& [name, nominal] : std::map<std::string, NominalTypeInfo>(program.nominalTypes.begin(), program.nominalTypes.end())) {
		if (nominal.kind != "record" && nominal.kind != "variant" && nominal.kind != "enum") continue;
		std::string encoded;
		for (unsigned char byte : name) { encoded += "0123456789abcdef"[byte >> 4]; encoded += "0123456789abcdef"[byte & 15]; }
		const std::string identity = typeIdentity(program, abi.layout(name));
		result += " /FAILIFMISMATCH:\"PCNType_" + encoded + "=" + PackageSHA256::hex(PackageSHA256::digest(identity)) + "\"";
	}
	return result + " ";
}
inline std::vector<std::uint8_t> windowsUnwind(std::uint32_t frame) {
	if (!frame || frame % 16 || frame > INT32_MAX) throw std::runtime_error("E0580 invalid x64 unwind frame size");
	std::vector<std::uint8_t> out;
	auto u8 = [&](std::uint8_t v) { out.push_back(v); };
	auto u16 = [&](std::uint16_t v) { u8(static_cast<std::uint8_t>(v)); u8(static_cast<std::uint8_t>(v >> 8)); };
	const unsigned allocationSlots = frame <= 128 ? 1 : frame / 8 <= UINT16_MAX ? 2 : 3;
	u8(1); u8(11); u8(static_cast<std::uint8_t>(allocationSlots + 1)); u8(0);
	u8(11);
	if (frame <= 128) u8(static_cast<std::uint8_t>(((frame - 8) / 8) << 4 | 2));
	else if (allocationSlots == 2) { u8(1); u16(static_cast<std::uint16_t>(frame / 8)); }
	else { u8(0x11); u16(static_cast<std::uint16_t>(frame)); u16(static_cast<std::uint16_t>(frame >> 16)); }
	u8(1); u8(0x50);
	while (out.size() % 4) u8(0);
	return out;
}
}
