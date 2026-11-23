#pragma once

struct EscapeSummary {
	std::set<std::size_t> returned, retained;
	bool unknownCall = false;
	bool operator==(const EscapeSummary&) const = default;
};

inline std::map<std::string, EscapeSummary> analyzeParameterEscape(const IRProgram& program) {
	std::map<std::string, EscapeSummary> summaries;
	for (const auto& function : program.functions) summaries.emplace(function.name, EscapeSummary{});
	bool changed = true;
	while (changed) {
		changed = false;
		for (const auto& function : program.functions) {
			using Origins = std::set<std::size_t>;
			std::map<std::uint32_t, Origins> origins;
			for (std::size_t n = 0; n < function.parameterVregs.size(); ++n)
				origins[static_cast<std::uint32_t>(function.parameterVregs[n])].insert(n);
			auto summary = summaries.at(function.name);
			auto merge = [](Origins& into, const Origins& from) {
				const auto before = into.size(); into.insert(from.begin(), from.end()); return before != into.size();
			};
			bool propagated = true;
			while (propagated) {
				propagated = false;
				auto visit = [&](const IRInstruction& instruction) {
					Origins values;
					for (auto arg : instruction.args) merge(values, origins[static_cast<std::uint32_t>(arg)]);
					for (const auto& [predecessor, arg] : instruction.phiInputs) {
						(void)predecessor; merge(values, origins[static_cast<std::uint32_t>(arg)]);
					}
					if (instruction.op == IROpcode::Call) {
						const auto callee = summaries.find(instruction.callee);
						if (callee == summaries.end()) {
							merge(summary.retained, values); summary.unknownCall = true;
						}
						else {
							values.clear();
							for (auto n : callee->second.returned) if (n < instruction.args.size())
								merge(values, origins[static_cast<std::uint32_t>(instruction.args[n])]);
							for (auto n : callee->second.retained) if (n < instruction.args.size())
								merge(summary.retained, origins[static_cast<std::uint32_t>(instruction.args[n])]);
							summary.unknownCall = summary.unknownCall || callee->second.unknownCall;
						}
					}
					else if (instruction.op == IROpcode::CallClosure) {
						merge(summary.retained, values); summary.unknownCall = true;
					}
					if (instruction.op == IROpcode::Return) merge(summary.returned, values);
					if (instruction.op == IROpcode::StoreField || instruction.op == IROpcode::ArrayStore)
						merge(summary.retained, values);
					if (instruction.dest != VReg::None)
						propagated = merge(origins[static_cast<std::uint32_t>(instruction.dest)], values) || propagated;
				};
				for (const auto& block : function.blocks) {
					for (const auto& phi : block.phiNodes) visit(phi);
					for (const auto& instruction : block.instructions) visit(instruction);
				}
			}
			if (summary != summaries.at(function.name)) { summaries[function.name] = std::move(summary); changed = true; }
		}
	}
	return summaries;
}

inline std::string parameterEscapeReport(const IRProgram& program) {
	std::ostringstream out;
	out << "Conservative whole-PCIR parameter escape summaries (may escape; not lifetime proofs)\n";
	for (const auto& [name, summary] : analyzeParameterEscape(program)) {
		out << "  " << name << " returned=";
		for (auto n : summary.returned) out << n << ',';
		out << " retained=";
		for (auto n : summary.retained) out << n << ',';
		out << " unknown_call=" << (summary.unknownCall ? "yes" : "no") << '\n';
	}
	return out.str();
}
