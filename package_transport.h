#pragma once

inline std::string packageEnvironmentSecret(const std::string& variable, bool required) {
	if (variable == "-") {
		if (required) throw std::runtime_error("E0561 registry credential variable is required");
		return {};
	}
	if (variable.empty() || variable.size() > 128 || !std::all_of(variable.begin(), variable.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
		throw std::runtime_error("E0561 invalid registry credential environment variable");
	std::string secret;
#ifdef _MSC_VER
	char* value = nullptr; std::size_t size = 0;
	if (_dupenv_s(&value, &size, variable.c_str()) != 0) throw std::runtime_error("E0561 registry credential lookup failed");
	std::unique_ptr<char, decltype(&std::free)> cleanup(value, &std::free);
	if (value) secret = value;
#else
	if (const auto* value = std::getenv(variable.c_str())) secret = value;
#endif
	if (secret.empty()) {
		if (required) throw std::runtime_error("E0561 registry credential environment variable is unavailable: " + variable);
		return {};
	}
	if (secret.size() > 4096 || secret.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("E0561 malformed registry credential");
	return secret;
}

inline std::optional<std::string> packageHttpsRead(const std::string& url, const std::string& bearer, std::size_t limit) {
	if (!url.starts_with("https://") || url.find_first_of("\r\n\t #?@\\") != std::string::npos || url.size() > 4096)
		throw std::runtime_error("E0561 registry requires a credential-free HTTPS URL without query or fragment");
#ifdef _WIN32
	struct Handle {
		HINTERNET value = nullptr;
		explicit Handle(HINTERNET handle) : value(handle) { if (!value) throw std::runtime_error("E0562 HTTPS transport initialization failed"); }
		~Handle() { WinHttpCloseHandle(value); }
		Handle(const Handle&) = delete;
		Handle& operator=(const Handle&) = delete;
	};
	const auto native = filesystemPathFromUtf8(url).native();
	URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts); parts.dwHostNameLength = static_cast<DWORD>(-1); parts.dwUrlPathLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(native.c_str(), static_cast<DWORD>(native.size()), 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwHostNameLength == 0)
		throw std::runtime_error("E0561 invalid HTTPS registry authority");
	const std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
	for (int attempt = 0; attempt < 3; ++attempt) {
		Handle session(WinHttpOpen(L"Paracine-Package/2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
		if (!WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000)) throw std::runtime_error("E0562 HTTPS timeout configuration failed");
		DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
		protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
		if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) throw std::runtime_error("E0562 HTTPS TLS policy configuration failed");
		Handle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
		Handle request(WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
		DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
		if (!WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon))) throw std::runtime_error("E0562 HTTPS automatic credential policy configuration failed");
		DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects))) throw std::runtime_error("E0562 HTTPS redirect policy configuration failed");
		std::wstring authorization;
		if (!bearer.empty()) {
			if (!std::all_of(bearer.begin(), bearer.end(), [](unsigned char c) { return c >= 33 && c <= 126; })) throw std::runtime_error("E0561 invalid bearer token bytes");
			authorization = L"Authorization: Bearer "; authorization.append(bearer.begin(), bearer.end()); authorization += L"\r\n";
		}
		if (!WinHttpSendRequest(request.value, authorization.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : authorization.c_str(), authorization.empty() ? 0 : static_cast<DWORD>(authorization.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) {
			const auto error = GetLastError();
			if (error == ERROR_WINHTTP_TIMEOUT && attempt < 2) { std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt))); continue; }
			throw std::runtime_error("E0562 HTTPS request failed (Windows error " + std::to_string(error) + ")");
		}
		DWORD status = 0, size = sizeof(status);
		if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) throw std::runtime_error("E0562 HTTPS status unavailable");
		if (status == 404) return std::nullopt;
		if ((status == 429 || status == 502 || status == 503 || status == 504) && attempt < 2) { std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt))); continue; }
		if (status != 200) throw std::runtime_error("E0562 registry rejected request with HTTP " + std::to_string(status));
		std::string result; std::array<char, 16384> buffer{};
		for (;;) {
			DWORD read = 0;
			if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) throw std::runtime_error("E0562 HTTPS response read failed");
			if (!read) return result;
			if (read > limit - result.size()) throw std::runtime_error("E0563 registry response exceeds byte budget");
			result.append(buffer.data(), read);
		}
	}
	throw std::runtime_error("E0562 registry retry budget exhausted");
#else
	(void)bearer; (void)limit;
	throw std::runtime_error("E0562 authenticated HTTPS package transport is currently implemented only on Windows; use a pinned filesystem mirror");
#endif
}
