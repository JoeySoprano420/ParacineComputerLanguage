#pragma once

static std::string jsonEscape(const std::string& input);

struct DiagnosticRenderOptions { std::string color = "auto"; };
inline thread_local DiagnosticRenderOptions diagnosticRenderOptions;

inline SemanticDiagnostic diagnosticFromException(const std::string& detail, const std::string& file) {
	SemanticDiagnostic diagnostic{ { 0, 0, file }, detail }; diagnostic.code = "E0000"; diagnostic.phase = "compiler";
	if (detail.size() >= 5 && detail.front() == 'E' && std::all_of(detail.begin() + 1, detail.begin() + 5, [](unsigned char byte) { return byte >= '0' && byte <= '9'; })) {
		diagnostic.code = detail.substr(0, 5);
		diagnostic.phase = diagnostic.code.starts_with("E01") ? "lexer" : diagnostic.code.starts_with("E02") ? "parser" : "compiler";
		std::size_t at = 5;
		if (detail.compare(at, 4, " at ") == 0) {
			const auto lineStart = at + 4, separator = detail.find(':', lineStart), end = separator == std::string::npos ? std::string::npos : detail.find(':', separator + 1);
			if (end != std::string::npos) {
				const auto line = std::from_chars(detail.data() + lineStart, detail.data() + separator, diagnostic.location.line);
				const auto column = std::from_chars(detail.data() + separator + 1, detail.data() + end, diagnostic.location.column);
				if (line.ec == std::errc{} && line.ptr == detail.data() + separator && column.ec == std::errc{} && column.ptr == detail.data() + end) at = end + 1;
				else diagnostic.location.line = diagnostic.location.column = 0;
			}
		}
		diagnostic.message = detail.substr(at);
		const auto first = diagnostic.message.find_first_not_of(' '); if (first != std::string::npos) diagnostic.message.erase(0, first);
	}
	if (diagnostic.code == "E0100" || diagnostic.code == "E0101" || diagnostic.code == "E0102") diagnostic.hint = "Indent using exactly four ASCII spaces per level; structural tabs are forbidden.";
	else if (diagnostic.code == "E0116") diagnostic.hint = "Use valid escapes, including Unicode \\u{HEX}.";
	else if (diagnostic.code == "E0201") diagnostic.hint = "Begin the block on a new significant line indented four spaces.";
	return diagnostic;
}

inline const char* diagnosticSeverityName(DiagnosticSeverity severity) {
	return severity == DiagnosticSeverity::Error ? "error" : severity == DiagnosticSeverity::Warning ? "warning" : "note";
}

inline void applyDiagnosticPolicy(SemanticAnalysisResult& analysis, const DiagnosticPolicy& policy) {
	std::erase_if(analysis.diagnostics, [&](const auto& diagnostic) {
		return diagnostic.severity == DiagnosticSeverity::Warning &&
			(diagnostic.warningLevel > policy.warningLevel || policy.allowedWarnings.contains(semanticDiagnosticCode(diagnostic)));
	});
	for (auto& diagnostic : analysis.diagnostics) if (diagnostic.severity == DiagnosticSeverity::Warning &&
		(policy.warningsAsErrors || policy.deniedWarnings.contains(semanticDiagnosticCode(diagnostic)))) {
		diagnostic.severity = DiagnosticSeverity::Error; diagnostic.promoted = true;
	}
}

inline std::optional<std::size_t> diagnosticOffset(std::string_view source, const SourceLocation& location) {
	if (!location.line || !location.column) return std::nullopt;
	std::size_t offset = 0;
	for (std::size_t line = 1; line < location.line; ++line) {
		const auto end = source.find('\n', offset);
		if (end == std::string_view::npos) return std::nullopt;
		offset = end + 1;
	}
	const auto end = source.find('\n', offset);
	const auto width = (end == std::string_view::npos ? source.size() : end) - offset;
	if (location.column - 1 > width) return std::nullopt;
	return offset + location.column - 1;
}

inline bool diagnosticHasColor() {
	if (diagnosticRenderOptions.color == "never") return false;
	if (diagnosticRenderOptions.color == "always") return true;
#ifdef _WIN32
	if (GetEnvironmentVariableW(L"NO_COLOR", nullptr, 0) || GetLastError() == ERROR_SUCCESS) return false;
	const auto handle = GetStdHandle(STD_ERROR_HANDLE); DWORD mode = 0;
	if (!GetConsoleMode(handle, &mode)) return false;
	return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) || SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
	return false;
#endif
}

inline void prepareDiagnostics(SemanticAnalysisResult& analysis, const Program& program) {
	analysis.sourceFiles = program.sourceFiles;
	for (auto& diagnostic : analysis.diagnostics) {
		if (!diagnostic.primarySpan.start.file.empty() || diagnostic.primarySpan.start.line != 1 || diagnostic.primarySpan.start.column != 1) {
			if (diagnostic.primarySpan.end.file.empty()) diagnostic.primarySpan.end.file = diagnostic.location.file;
		}
		else diagnostic.primarySpan = { diagnostic.location, diagnostic.location };
		diagnostic.code = semanticDiagnosticCode(diagnostic);
		if (diagnostic.phase == "semantic" && diagnostic.code.starts_with("E02")) diagnostic.phase = "parser";
		if (diagnostic.hint.empty()) diagnostic.hint = semanticDiagnosticHint(diagnostic);
		const auto expected = diagnostic.message.find("expected "), got = diagnostic.message.find(", got ");
		if (expected != std::string::npos && got != std::string::npos && got > expected && diagnostic.message.find("type mismatch") != std::string::npos) {
			diagnostic.expectedType = diagnostic.message.substr(expected + 9, got - expected - 9);
			diagnostic.actualType = diagnostic.message.substr(got + 6);
		}
		if (const auto chain = program.importChains.find(diagnostic.location.file); chain != program.importChains.end())
			for (const auto& [location, spelling] : chain->second) diagnostic.traces.push_back({ "import", spelling, { location, location } });
		if (diagnostic.primarySpan.start.column == diagnostic.primarySpan.end.column && diagnostic.primarySpan.start.line == diagnostic.primarySpan.end.line) {
			const auto file = analysis.sourceFiles.find(diagnostic.location.file);
			if (file != analysis.sourceFiles.end()) if (const auto offset = diagnosticOffset(file->second, diagnostic.location); offset && *offset < file->second.size() && file->second[*offset] != '\n') {
				std::size_t width = 1;
				while (*offset + width < file->second.size() && (static_cast<unsigned char>(file->second[*offset + width]) & 0xc0) == 0x80) ++width;
				diagnostic.primarySpan.end.column += width;
			}
		}
	}
	std::stable_sort(analysis.diagnostics.begin(), analysis.diagnostics.end(), [](const auto& left, const auto& right) {
		return std::tie(left.location.file, left.location.line, left.location.column) < std::tie(right.location.file, right.location.line, right.location.column);
	});
	std::vector<SemanticDiagnostic> merged;
	std::map<std::tuple<std::string, std::size_t, std::size_t, std::string, std::string, bool>, std::size_t> warnings;
	for (auto& diagnostic : analysis.diagnostics) {
		if (diagnostic.severity == DiagnosticSeverity::Warning || diagnostic.promoted) {
			const auto key = std::tuple{ diagnostic.location.file, diagnostic.location.line, diagnostic.location.column, diagnostic.code, diagnostic.message, diagnostic.promoted };
			const auto [found, inserted] = warnings.emplace(key, merged.size());
			if (!inserted) {
				auto& previous = merged[found->second];
				for (const auto& trace : diagnostic.traces) if (std::none_of(previous.traces.begin(), previous.traces.end(), [&](const auto& existing) {
					return std::tie(existing.kind, existing.message, existing.span.start.file, existing.span.start.line, existing.span.start.column) ==
						std::tie(trace.kind, trace.message, trace.span.start.file, trace.span.start.line, trace.span.start.column);
				})) previous.traces.push_back(trace);
				continue;
			}
		}
		merged.push_back(std::move(diagnostic));
	}
	analysis.diagnostics = std::move(merged);
}

inline std::string diagnosticsJSON(const SemanticAnalysisResult& analysis, std::string_view fallbackPath = {}) {
	std::ostringstream out;
	out << "{\"schema\":\"paracine.diagnostics.v1\",\"version\":1,\"coordinates\":\"normalized-utf8-byte-1-based\",\"ok\":" << (analysis.succeeded() ? "true" : "false") << ",\"diagnostics\":[";
	auto quoted = [&](std::string_view value) { out << '"' << jsonEscape(std::string(value)) << '"'; };
	auto span = [&](const SourceSpan& value) {
		const auto file = value.start.file.empty() ? std::string(fallbackPath) : value.start.file;
		out << "{\"file\":"; quoted(file);
		auto position = [&](const SourceLocation& location) {
			out << "{\"line\":" << location.line << ",\"column\":" << location.column << ",\"offset\":";
			const auto source = analysis.sourceFiles.find(value.start.file);
			const auto offset = source == analysis.sourceFiles.end() ? std::nullopt : diagnosticOffset(source->second, location);
			if (offset) out << *offset; else out << "null";
			out << '}';
		};
		out << ",\"start\":"; position(value.start); out << ",\"end\":"; position(value.end); out << '}';
	};
	for (std::size_t index = 0; index < analysis.diagnostics.size(); ++index) {
		const auto& diagnostic = analysis.diagnostics[index]; if (index) out << ',';
		out << "{\"severity\":"; quoted(diagnosticSeverityName(diagnostic.severity));
		out << ",\"code\":"; quoted(semanticDiagnosticCode(diagnostic));
		out << ",\"phase\":"; quoted(diagnostic.phase);
		out << ",\"message\":"; quoted(diagnostic.message);
		out << ",\"hint\":"; quoted(diagnostic.hint);
		out << ",\"line\":" << diagnostic.location.line << ",\"column\":" << diagnostic.location.column;
		out << ",\"warning_level\":" << diagnostic.warningLevel << ",\"promoted\":" << (diagnostic.promoted ? "true" : "false");
		out << ",\"primary\":"; span(diagnostic.primarySpan);
		out << ",\"secondary\":[";
		for (std::size_t item = 0; item < diagnostic.secondarySpans.size(); ++item) {
			if (item) out << ','; out << "{\"span\":"; span(diagnostic.secondarySpans[item].span);
			out << ",\"message\":"; quoted(diagnostic.secondarySpans[item].message); out << '}';
		}
		out << "],\"traces\":[";
		for (std::size_t item = 0; item < diagnostic.traces.size(); ++item) {
			if (item) out << ','; const auto& trace = diagnostic.traces[item];
			out << "{\"kind\":"; quoted(trace.kind); out << ",\"message\":"; quoted(trace.message); out << ",\"span\":"; span(trace.span); out << '}';
		}
		out << "],\"fixes\":[";
		for (std::size_t item = 0; item < diagnostic.fixes.size(); ++item) {
			if (item) out << ','; const auto& fix = diagnostic.fixes[item];
			out << "{\"span\":"; span(fix.span); out << ",\"replacement\":"; quoted(fix.replacement);
			out << ",\"applicability\":"; quoted(fix.applicability); out << '}';
		}
		out << "],\"type_difference\":";
		if (diagnostic.expectedType.empty() && diagnostic.actualType.empty()) out << "null";
		else { out << "{\"expected\":"; quoted(diagnostic.expectedType); out << ",\"actual\":"; quoted(diagnostic.actualType); out << '}'; }
		out << '}';
	}
	out << "]}\n"; return out.str();
}

inline std::string diagnosticTerminalText(std::string_view text, bool tabs = false) {
	static constexpr char hex[] = "0123456789abcdef";
	std::string result;
	for (unsigned char byte : text) {
		if ((byte < 32 && !(tabs && byte == '\t')) || byte == 127) { result += "\\x"; result += hex[byte >> 4]; result += hex[byte & 15]; }
		else result += static_cast<char>(byte);
	}
	return result;
}

inline void renderSemanticDiagnostics(std::ostream& out, const SemanticAnalysisResult& analysis, std::string_view sourcePath = {}, std::string_view source = {}, bool color = false) {
	auto location = [&](const SourceSpan& span, std::string_view label) {
		const auto file = span.start.file.empty() ? std::string(sourcePath) : span.start.file;
		out << "  " << diagnosticTerminalText(file) << ':' << span.start.line << ':' << span.start.column << ": " << diagnosticTerminalText(label) << '\n';
		const auto found = analysis.sourceFiles.find(span.start.file);
		const std::string_view text = found == analysis.sourceFiles.end() ? (span.start.file.empty() ? source : std::string_view{}) : std::string_view(found->second);
		auto start = span.start; start.column = 1;
		if (const auto offset = diagnosticOffset(text, start)) {
			const auto end = text.find('\n', *offset);
			const auto line = text.substr(*offset, end == std::string_view::npos ? text.size() - *offset : end - *offset);
			out << "    " << diagnosticTerminalText(line, true) << "\n    ";
			const auto prefix = std::min(span.start.column ? span.start.column - 1 : 0, line.size());
			for (std::size_t index = 0; index < prefix; ++index) {
				const auto byte = static_cast<unsigned char>(line[index]);
				if ((byte < 32 && byte != '\t') || byte == 127) out << "    ";
				else if ((byte & 0xc0) != 0x80) out << (byte == '\t' ? '\t' : ' ');
			}
			const auto width = span.end.line == span.start.line && span.end.column > span.start.column ? span.end.column - span.start.column : 1;
			out << '^' << std::string(std::min<std::size_t>(width - 1, 120), '~') << '\n';
		}
	};
	for (const auto& diagnostic : analysis.diagnostics) {
		const char* tint = diagnostic.severity == DiagnosticSeverity::Error ? "\x1b[31m" : diagnostic.severity == DiagnosticSeverity::Warning ? "\x1b[33m" : "\x1b[36m";
		if (color) out << tint;
		out << diagnosticSeverityName(diagnostic.severity) << '[' << semanticDiagnosticCode(diagnostic) << ']';
		if (color) out << "\x1b[0m";
		out << ": " << diagnosticTerminalText(diagnostic.message) << (diagnostic.promoted ? " (warning promoted to error)" : "") << '\n';
		location(diagnostic.primarySpan, "here");
		for (const auto& secondary : diagnostic.secondarySpans) location(secondary.span, secondary.message);
		for (const auto& trace : diagnostic.traces) location(trace.span, trace.kind + ": " + trace.message);
		if (!diagnostic.expectedType.empty() || !diagnostic.actualType.empty()) {
			const auto count = std::min(diagnostic.expectedType.size(), diagnostic.actualType.size()); std::size_t common = 0;
			while (common < count && diagnostic.expectedType[common] == diagnostic.actualType[common]) ++common;
			out << "  expected: " << diagnosticTerminalText(diagnostic.expectedType) << "\n  actual:   " << diagnosticTerminalText(diagnostic.actualType) << "\n            " << std::string(std::min<std::size_t>(common, 120), ' ') << "^ types differ\n";
		}
		if (!diagnostic.hint.empty()) out << "  help: " << diagnosticTerminalText(diagnostic.hint) << '\n';
		for (const auto& fix : diagnostic.fixes) out << "  fix (" << diagnosticTerminalText(fix.applicability) << "): " << (fix.replacement.empty() ? "remove the indicated span" : "replace with " + diagnosticTerminalText(fix.replacement)) << '\n';
	}
}
