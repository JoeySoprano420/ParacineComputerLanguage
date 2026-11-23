// Included in runCompilerSelfTests, after its local assertion helpers.
test("targets: registered architectures never silently select x64", [&] {
	std::set<std::string> identities;
	for (const auto& target : target_contract::targets) {
		if (!identities.insert(target.name).second || &target_contract::get(target.name) != &target)
			throw std::runtime_error("duplicate or unstable target identity");
		if (target.architecture == target_contract::Architecture::ARM64) {
			bool rejected = false;
			try { target_contract::requireCodegen(target); }
			catch (const std::runtime_error& error) { rejected = std::string_view(error.what()).starts_with("E0590"); }
			if (!rejected) throw std::runtime_error("unimplemented ARM64 target accepted for code generation");
			for (const auto& feature : target_contract::features(target))
				if (feature.support != target_contract::Support::Unsupported) throw std::runtime_error("ARM64 capability overclaim");
		}
		for (const auto& service : runtime_contract::services(target))
			if ((std::string_view(service.name) == "threads" || std::string_view(service.name) == "stackOverflow") && service.support != target_contract::Support::Unsupported)
				throw std::runtime_error("compiler workers/unwind metadata mistaken for runtime services");
	}
	if (identities.size() != 6 || target_contract::get("linux-arm64").machine != 183 ||
		target_contract::get("windows-arm64").machine != 0xaa64 || target_contract::get("macos-arm64").machine != 0x0100000c)
		throw std::runtime_error("target platform identities drifted");
	bool rejected = false;
	try { (void)target_contract::get("linux-unknown"); }
	catch (const std::runtime_error& error) { rejected = std::string_view(error.what()).starts_with("E0540"); }
	if (!rejected) throw std::runtime_error("unknown target accepted");
});
test("ABI classifier: positional Windows lanes, indirect sret and C varargs", [&] {
	using V = target_contract::ValueClass;
	const auto& target = target_contract::get("windows-x86_64");
	const auto call = target_contract::classify(target, { V::Indirect,V::Float64,V::Integer,V::Float32,V::Integer,V::Float32 }, true, 3);
	if (call.stackBytes != 48 || call.arguments.size() != 6 || call.vectorRegisters != 0)
		throw std::runtime_error("incorrect Windows home/overflow area");
	for (std::size_t n = 0; n < call.arguments.size(); ++n) {
		const auto& lane = call.arguments[n];
		if (lane.stack != (n >= 4) || lane.index != (n >= 4 ? n - 4 : n)) throw std::runtime_error("non-positional Windows lane");
	}
	if (!call.arguments[1].duplicateInteger || !call.arguments[3].duplicateInteger ||
		!call.arguments[3].promoteFloat || !call.arguments[5].promoteFloat || call.arguments[5].duplicateInteger || call.arguments[2].floating)
		throw std::runtime_error("Windows variadic floating duplication/promotion changed");
	if (target_contract::classify(target, {}).stackBytes != 32 || target_contract::classify(target, { V::Float32 }).arguments[0].promoteFloat)
		throw std::runtime_error("fixed scalar call classification changed");
});
test("ABI classifier: independent System V banks and ordered overflow lanes", [&] {
	using V = target_contract::ValueClass;
	const auto& target = target_contract::get("linux-x86_64");
	std::vector<V> arguments(6, V::Integer);
	arguments.push_back(V::Integer);
	arguments.insert(arguments.end(), 8, V::Float64);
	arguments.push_back(V::Float32);
	arguments.push_back(V::Integer);
	const auto call = target_contract::classify(target, arguments, true, 7);
	if (call.stackBytes != 32 || call.vectorRegisters != 8 || !call.arguments[6].stack || call.arguments[6].index != 0 ||
		!call.arguments[15].stack || call.arguments[15].index != 1 || !call.arguments[15].promoteFloat ||
		!call.arguments[16].stack || call.arguments[16].index != 2)
		throw std::runtime_error("System V overflow lanes are not source ordered");
	for (std::size_t n = 7; n < 15; ++n)
		if (!call.arguments[n].floating || call.arguments[n].stack || call.arguments[n].index != n - 7)
			throw std::runtime_error("integer exhaustion consumed floating registers");
	bool rejected = false;
	try { (void)target_contract::classify(target, { V::Indirect }); }
	catch (const std::runtime_error& error) { rejected = std::string_view(error.what()).starts_with("E0540"); }
	if (!rejected) throw std::runtime_error("rich System V boundary classified as scalar");
	rejected = false;
	try { (void)target_contract::classify(target, { V::Integer }, true, 2); }
	catch (const std::runtime_error& error) { rejected = std::string_view(error.what()).starts_with("E0591"); }
	if (!rejected) throw std::runtime_error("invalid variadic boundary accepted");
	rejected = false;
	try { (void)target_contract::classify(target, { static_cast<V>(99) }); }
	catch (const std::runtime_error& error) { rejected = std::string_view(error.what()).starts_with("E0591"); }
	if (!rejected) throw std::runtime_error("invalid argument class accepted");
});
test("ABI conformance: Windows variadic fixed float reaches both register banks", [&] {
	for (int level : { 0,3 }) {
		CompilerOptions options; options.optimizationLevel = level;
		auto artifacts = compileAll("foreign c routine probe(value: double, ...) gives int\nroutine main() gives int\n    give probe(2.5, 1)\n", options);
		if (!artifacts.analysis.succeeded() || artifacts.machine.externalCalls.size() != 1 || artifacts.machine.externalCalls.front().symbol != "probe")
			throw std::runtime_error("invalid foreign ABI probe fixture");
		auto& machine = artifacts.machine;
		const auto call = machine.externalCalls.front();
		const auto target = machine.text.size();
		// Leaf probe compares incoming RCX with the exact XMM0 bit pattern.
		machine.text.insert(machine.text.end(), { 0x66,0x48,0x0f,0x7e,0xc0,0x48,0x39,0xc1,0x75,0x06,0xb8,42,0,0,0,0xc3,0x31,0xc0,0xc3 });
		const auto displacement = static_cast<std::int32_t>(target - (call.dispPos + 4));
		std::memcpy(machine.text.data() + call.dispPos, &displacement, 4);
		machine.externalCalls.clear();
		for (auto& line : machine.listing) std::copy_n(machine.text.begin() + static_cast<std::ptrdiff_t>(line.offset), line.bytes.size(), line.bytes.begin());
		if (!MachineVerifier{}.verify(machine).empty()) throw std::runtime_error("linked register probe is malformed");
#if defined(_WIN32)
		const auto file = std::filesystem::temp_directory_path() / ("paracine-abi-probe-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".exe");
		struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{ file };
		writeBinaryFile(file, PECOFFGenerator{}.emitExecutable(artifacts.pcir, artifacts.pmir, machine));
		if (runNativeExecutable(file) != 42) throw std::runtime_error("variadic named float not duplicated into RCX");
#elif defined(__linux__) && defined(__x86_64__)
		if (executeEmittedMachineTextIsolated(machine) != 42) throw std::runtime_error("variadic named float not duplicated into RCX");
#endif
	}
});
test("PCIR mutation fuzz: malformed registers, branches and terminators reject", [&] {
	CompilerOptions options; options.optimizationLevel = 0;
	const auto seed = compileAll("routine main() gives int\n    var result: int = 0\n    from index = 1 to 5\n        result += index\n    give result\n", options, false);
	if (!seed.analysis.succeeded() || !PCIRVerifier{}.verify(seed.pcir).empty()) throw std::runtime_error("invalid mutation seed");
	std::uint32_t random = 0x5eed1234;
	for (unsigned iteration = 0; iteration < 192; ++iteration) {
		random = random * 1664525u + 1013904223u;
		auto program = seed.pcir;
		auto& function = program.functions.front();
		auto& block = function.blocks[random % function.blocks.size()];
		if (block.instructions.empty()) throw std::runtime_error("empty mutation seed block");
		const auto invalid = function.totalVregs + 100 + iteration;
		switch (iteration % 6) {
		case 0: block.instructions.front().args.push_back(static_cast<VReg>(invalid)); break;
		case 1: block.instructions.front().dest = static_cast<VReg>(invalid); break;
		case 2: block.instructions.back().targetBlocks.push_back(static_cast<BlockID>(0x70000000u + iteration)); break;
		case 3: block.instructions.push_back(block.instructions.front()); break;
		case 4: block.instructions.front().op = static_cast<IROpcode>(0x7000u + iteration); break;
		case 5: function.blocks.push_back(block); break;
		}
		const auto errors = PCIRVerifier{}.verify(program);
		if (errors.empty()) throw std::runtime_error("invalid IR mutation survived at iteration " + std::to_string(iteration));
		const auto expected = iteration % 6 == 4 ? "unknown PCIR opcode" : iteration % 6 == 5 ? "duplicate PCIR block identity" : "";
		if (*expected && std::none_of(errors.begin(), errors.end(), [&](const auto& error) { return error.find(expected) != std::string::npos; }))
			throw std::runtime_error("IR mutation rejected for an unrelated reason");
	}
	if (PCIRInterpreter(seed.pcir).run("") != 15) throw std::runtime_error("mutation campaign modified immutable seed");
});
test("lexer/parser mutation fuzz: lossless recovery and transactional incremental edits", [&] {
	const std::string seed = "# trivia\nroutine main() gives int\n    let value: int = 41\n    give value + 1\n";
	std::uint32_t random = 0xc0ffee;
	for (unsigned iteration = 0; iteration < 256; ++iteration) {
		random = random * 1664525u + 1013904223u;
		const auto position = random % seed.size();
		std::string changed = seed;
		const std::string replacement = std::string(1, "()[]+\n #0\t"[iteration % 10]);
		changed.replace(position, 1, replacement);
		IncrementalLexer incremental(seed);
		std::vector<Token> expected;
		bool invalid = false;
		try { expected = Lexer(changed).tokenize(); }
		catch (const std::runtime_error& error) {
			if (!std::string_view(error.what()).starts_with("E01")) throw;
			invalid = true;
		}
		if (invalid) {
			bool rejected = false;
			try { (void)incremental.replace(position, 1, replacement); }
			catch (const std::runtime_error&) { rejected = true; }
			if (!rejected || incremental.source() != seed) throw std::runtime_error("invalid incremental edit was committed");
			continue;
		}
		(void)incremental.replace(position, 1, replacement);
		const auto& actual = incremental.tokens();
		if (actual.size() != expected.size()) throw std::runtime_error("incremental token count differs from full lexer");
		for (std::size_t n = 0; n < actual.size(); ++n)
			if (actual[n].kind != expected[n].kind || actual[n].text != expected[n].text || actual[n].startOffset != expected[n].startOffset ||
				actual[n].endOffset != expected[n].endOffset || actual[n].location.line != expected[n].location.line || actual[n].location.column != expected[n].location.column)
				throw std::runtime_error("incremental token/span differs from full lexer");
		auto syntax = SyntaxTree::fromSource(changed, expected);
		auto parsed = Parser(expected).parseRecovering(std::move(syntax));
		if (parsed.syntaxTree.reconstructSource() != changed) throw std::runtime_error("parser recovery lost mutated source/trivia");
	}
});
test("archives: portable paths reject aliases traversal and file/directory collisions", [&] {
	const std::vector<std::string> invalid{ "", "/absolute", "../escape", "a/../b", "a//b", "./a", "a/", "a/./b", "C:escape", "a\\b", "a\nb", std::string("a\0b", 3), "CON", "con.txt", "a/NUL.txt", "COM1.log", "LPT9", "a.", "a ", "a/<b>" };
	auto rejectsArchive = [&](const std::vector<ArchiveFile>& files) {
		try { (void)makeDeterministicZip(files); }
		catch (const std::runtime_error& error) { if (std::string_view(error.what()).starts_with("E0593")) return; throw; }
		throw std::runtime_error("unsafe archive member set was accepted");
	};
	for (const auto& name : invalid) rejectsArchive({ {name,{1}} });
	for (const auto& names : { std::pair{"a", "A"}, std::pair{"a", "a/b"}, std::pair{"a/b", "a"}, std::pair{"bin/A", "BIN/a"} })
		rejectsArchive({ {names.first,{1}}, {names.second,{2}} });
	const std::vector<ArchiveFile> valid{ {"manifest.json",byteVector("{}")}, {"bin/program.exe",{0,1,2}} };
	const auto first = makeDeterministicZip(valid), second = makeDeterministicZip(valid);
	if (first != second || first.size() < 22 || first[0] != 0x50 || first[1] != 0x4b || archiveCRC32(byteVector("123456789")) != 0xcbf43926u)
		throw std::runtime_error("deterministic ZIP structure/CRC changed");
});
