#pragma once

namespace package_policy {
namespace fs = std::filesystem;
constexpr std::size_t sourceLimit = 8 * 1024 * 1024;

inline std::string readBounded(const fs::path& file, std::size_t limit) {
	return readPackageInput(file, limit);
}

inline bool within(const fs::path& path, const fs::path& root) {
	const auto relative = path.lexically_relative(root);
	return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

inline void writeAtomic(const fs::path& root, const fs::path& file, const std::string& bytes, bool replace = true) {
	const auto boundary = fs::canonical(root);
	const auto parent = fs::weakly_canonical(file.parent_path());
	if (!within(parent, boundary) && parent != boundary) throw std::runtime_error("E0564 package output escapes its boundary");
	fs::create_directories(parent);
	if (fs::canonical(parent) != parent) throw std::runtime_error("E0564 package output directory changed identity");
	std::random_device random;
	const auto temporary = parent / (".pcn-stage-" + std::to_string(random()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup { fs::path file; ~Cleanup() { std::error_code error; fs::remove(file, error); } } cleanup{ temporary };
#ifdef _WIN32
	const HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("E0564 cannot exclusively create package staging file");
	struct Close { HANDLE handle; ~Close() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } close{ handle };
	DWORD written = 0;
	if (bytes.size() > std::numeric_limits<DWORD>::max() || !WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() || !FlushFileBuffers(handle)) throw std::runtime_error("E0564 package staging write failed");
	CloseHandle(handle); close.handle = INVALID_HANDLE_VALUE;
	if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) throw std::runtime_error("E0564 package atomic publication failed");
#else
	if (!replace && fs::exists(file)) throw std::runtime_error("E0564 immutable package release already exists");
	writeTextFile(temporary, bytes); fs::rename(temporary, file);
#endif
}

struct Trust { std::string name, source, bearerEnvironment, hmacEnvironment; };
inline std::vector<Trust> readTrust(const fs::path& root) {
	std::vector<Trust> result;
	const auto path = root / ".paracine" / "trust.conf";
	if (!fs::exists(path)) return result;
	if (!within(fs::canonical(path), fs::canonical(root))) throw std::runtime_error("E0561 namespace trust file escapes project boundary");
	std::istringstream input(readBounded(path, 65536)); std::string line;
	std::set<std::string> names;
	while (std::getline(input, line)) {
		if (line.empty() || line.starts_with('#')) continue;
		std::istringstream fields(line); Trust trust; std::string trailing;
		if (!(fields >> trust.name >> trust.source >> trust.bearerEnvironment >> trust.hmacEnvironment) || (fields >> trailing)) throw std::runtime_error("E0561 malformed namespace trust binding");
		validatePackageIdentity(trust.name, "0.0.0");
		if (!names.insert(trust.name).second) throw std::runtime_error("E0561 duplicate namespace owner binding: " + trust.name);
		result.push_back(std::move(trust));
	}
	return result;
}
inline std::optional<Trust> trustFor(const fs::path& root, const std::string& name, const std::string& source) {
	for (const auto& trust : readTrust(root)) if (trust.name == name) {
		if (trust.source != source) throw std::runtime_error("E0561 package source violates namespace ownership: " + name);
		return trust;
	}
	return std::nullopt;
}

struct Release {
	std::string name, version, digest, status = "active", provenance = "local-filesystem", authentication = "none";
	std::string canonical() const { return "PCN-RELEASE-1\n" + name + "\n" + version + "\n" + digest + "\n" + status + "\n" + provenance + "\n"; }
};
inline bool isDigest(const std::string& value) {
	return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
inline Release parseRelease(const std::string& text) {
	if (text.empty() || text.back() != '\n') throw std::runtime_error("E0565 release provenance must end with a newline");
	std::istringstream input(text); std::array<std::string, 7> lines{};
	for (auto& line : lines) if (!std::getline(input, line) || line.find('\r') != std::string::npos) throw std::runtime_error("E0565 malformed release provenance record");
	std::string trailing;
	if (std::getline(input, trailing) || lines[0] != "PCN-RELEASE-1") throw std::runtime_error("E0565 unsupported release provenance schema");
	Release release{ lines[1], lines[2], lines[3], lines[4], lines[5], lines[6] };
	validatePackageIdentity(release.name, release.version);
	if (!isDigest(release.digest) || (release.status != "active" && release.status != "deprecated" && release.status != "yanked") || release.provenance.empty() || release.provenance.size() > 256 || release.provenance.find_first_of(" \t\r\n") != std::string::npos)
		throw std::runtime_error("E0565 invalid release provenance fields");
	return release;
}
inline void verifyRelease(const Release& release, const std::string& name, const std::string& version, const std::string& bytes, const std::optional<Trust>& trust, bool locked) {
	if (release.name != name || release.version != version || !packageDigestEqual(release.digest, packageDigestHex(bytes))) throw std::runtime_error("E0560 package content or identity does not match release provenance");
	if (trust && trust->hmacEnvironment != "-") {
		const auto key = packageEnvironmentSecret(trust->hmacEnvironment, true);
		if (key.size() < 32) throw std::runtime_error("E0561 registry HMAC key must contain at least 32 bytes");
		if (!isDigest(release.authentication) || !packageDigestEqual(release.authentication, packageHmac(key, release.canonical()))) throw std::runtime_error("E0565 release authentication failed");
	}
	else if (release.authentication != "none") throw std::runtime_error("E0565 release authentication has no pinned verifier");
	if (release.status == "yanked" && !locked) throw std::runtime_error("E0566 yanked release cannot be newly locked");
}

inline std::optional<std::string> readSource(const std::string& source, const std::string& name, const std::string& version, const std::string& leaf, const std::optional<Trust>& trust, std::size_t limit) {
	if (source.starts_with("fs:")) {
		const auto root = fs::canonical(filesystemPathFromUtf8(source.substr(3)));
		const auto file = root / name / version / leaf;
		if (!fs::exists(file)) return std::nullopt;
		if (!within(fs::canonical(file), root)) throw std::runtime_error("E0564 registry source escapes its boundary");
		return readBounded(file, limit);
	}
	if (!source.starts_with("https://")) throw std::runtime_error("E0561 only filesystem and HTTPS registries are supported; shell/git transports are disabled");
	if (!trust || trust->hmacEnvironment == "-") throw std::runtime_error("E0561 HTTPS registry requires pinned namespace/HMAC provenance trust");
	auto base = source; while (!base.empty() && base.back() == '/') base.pop_back();
	return packageHttpsRead(base + "/" + name + "/" + version + "/" + leaf, packageEnvironmentSecret(trust->bearerEnvironment, false), limit);
}

inline PackageResolution resolve(const fs::path& root, const std::string& name, const std::string& version, const std::string& source, bool offline = false, const std::string& expected = {}) {
	validatePackageIdentity(name, version);
	const auto trust = trustFor(root, name, source);
	const auto cache = packageCacheDir(root) / packageDigestHex(name + "\n" + version + "\n" + source) / "package.pcn";
	const auto metadata = cache.parent_path() / "release.meta";
	auto validate = [&](const std::string& bytes, const std::string& record) {
		const auto release = parseRelease(record);
		verifyRelease(release, name, version, bytes, trust, !expected.empty());
		if (!expected.empty() && !packageDigestEqual(expected, release.digest)) throw std::runtime_error("E0560 package differs from immutable lock digest");
		if (release.status == "deprecated") std::cerr << "warning[W0566]: deprecated package " << name << " " << version << "\n";
		return release;
	};
	if (fs::exists(cache) && (offline || !expected.empty())) {
		const auto boundary = fs::canonical(root);
		if (!within(fs::canonical(cache), boundary) || !within(fs::canonical(metadata), boundary)) throw std::runtime_error("E0564 package cache escapes project boundary");
		const auto bytes = readBounded(cache, sourceLimit), record = readBounded(metadata, 4096);
		const auto release = validate(bytes, record);
		if (offline || !expected.empty()) return { cache, source, release.digest };
	}
	if (offline) throw std::runtime_error("E0567 offline package cache miss: " + name + " " + version);
	const auto bytes = readSource(source, name, version, "package.pcn", trust, sourceLimit);
	if (!bytes) throw std::runtime_error("E0562 package release not found: " + name + " " + version);
	const auto record = readSource(source, name, version, "release.meta", trust, 4096);
	Release release{ name, version, packageDigestHex(*bytes) };
	if (record) release = parseRelease(*record);
	else if (!source.starts_with("fs:") || (trust && trust->hmacEnvironment != "-")) throw std::runtime_error("E0565 authenticated release metadata is required");
	verifyRelease(release, name, version, *bytes, trust, !expected.empty());
	if (!expected.empty() && !packageDigestEqual(expected, release.digest)) throw std::runtime_error("E0560 fetched package differs from immutable lock digest");
	fs::create_directories(root / ".paracine");
	writeAtomic(root, metadata, release.canonical() + release.authentication + "\n");
	writeAtomic(root, cache, *bytes);
	if (release.status == "deprecated") std::cerr << "warning[W0566]: deprecated package " << name << " " << version << "\n";
	return { cache, source, release.digest };
}
} // namespace package_policy
