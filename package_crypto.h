#pragma once

class PackageSHA256 {
public:
	void update(const void* data, std::size_t length) {
		const auto* p = static_cast<const std::uint8_t*>(data);
		length_ += static_cast<std::uint64_t>(length);
		while (length) {
			const auto count = std::min<std::size_t>(64 - used_, length);
			std::memcpy(buffer_.data() + used_, p, count);
			used_ += count; p += count; length -= count;
			if (used_ == 64) { block(buffer_.data()); used_ = 0; }
		}
	}
	void update(std::string_view value) { update(value.data(), value.size()); }
	std::array<std::uint8_t, 32> finish() {
		const auto bits = length_ * 8;
		buffer_[used_++] = 0x80;
		if (used_ > 56) { while (used_ < 64) buffer_[used_++] = 0; block(buffer_.data()); used_ = 0; }
		while (used_ < 56) buffer_[used_++] = 0;
		for (int n = 7; n >= 0; --n) buffer_[used_++] = static_cast<std::uint8_t>(bits >> (8 * n));
		block(buffer_.data());
		std::array<std::uint8_t, 32> result{};
		for (std::size_t n = 0; n < 8; ++n) for (int byte = 0; byte < 4; ++byte) result[n * 4 + byte] = static_cast<std::uint8_t>(state_[n] >> (24 - byte * 8));
		return result;
	}
	static std::array<std::uint8_t, 32> digest(std::string_view value) { PackageSHA256 hash; hash.update(value); return hash.finish(); }
	static std::string hex(const std::array<std::uint8_t, 32>& bytes) {
		static constexpr char digits[] = "0123456789abcdef";
		std::string result; result.reserve(64);
		for (auto byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
		return result;
	}
private:
	static std::uint32_t rotate(std::uint32_t value, int count) { return (value >> count) | (value << (32 - count)); }
	void block(const std::uint8_t* bytes) {
		static constexpr std::uint32_t constants[64] = {
			0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
			0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
			0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
			0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
			0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
			0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
			0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
			0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
		std::uint32_t words[64]{};
		for (int n = 0; n < 16; ++n) words[n] = (static_cast<std::uint32_t>(bytes[n * 4]) << 24) | (static_cast<std::uint32_t>(bytes[n * 4 + 1]) << 16) | (static_cast<std::uint32_t>(bytes[n * 4 + 2]) << 8) | bytes[n * 4 + 3];
		for (int n = 16; n < 64; ++n) {
			const auto a = words[n - 15], b = words[n - 2];
			words[n] = (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) + words[n - 16] + words[n - 7] + (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
		}
		auto a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5], g = state_[6], h = state_[7];
		for (int n = 0; n < 64; ++n) {
			const auto first = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + constants[n] + words[n];
			const auto second = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			h = g; g = f; f = e; e = d + first; d = c; c = b; b = a; a = first + second;
		}
		state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d; state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
	}
	std::uint32_t state_[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
	std::array<std::uint8_t, 64> buffer_{};
	std::size_t used_ = 0;
	std::uint64_t length_ = 0;
};

inline std::string packageHmac(std::string_view key, std::string_view message) {
	std::array<std::uint8_t, 64> normalized{}, inner{}, outer{};
	if (key.size() > normalized.size()) {
		const auto digest = PackageSHA256::digest(key); std::copy(digest.begin(), digest.end(), normalized.begin());
	}
	else std::copy(key.begin(), key.end(), normalized.begin());
	for (std::size_t n = 0; n < 64; ++n) { inner[n] = normalized[n] ^ 0x36; outer[n] = normalized[n] ^ 0x5c; }
	PackageSHA256 inside; inside.update(inner.data(), inner.size()); inside.update(message);
	const auto digest = inside.finish();
	PackageSHA256 outside; outside.update(outer.data(), outer.size()); outside.update(digest.data(), digest.size());
	return PackageSHA256::hex(outside.finish());
}

inline bool packageDigestEqual(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) return false;
	unsigned difference = 0;
	for (std::size_t n = 0; n < a.size(); ++n) difference |= static_cast<unsigned char>(a[n]) ^ static_cast<unsigned char>(b[n]);
	return difference == 0;
}
