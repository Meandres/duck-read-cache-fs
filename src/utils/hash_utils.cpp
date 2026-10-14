#include "hash_utils.hpp"

#include "mbedtls_wrapper.hpp"

namespace duckdb {

string Sha256ToHexString(const hash_bytes &sha256) {
	static constexpr char kHexChars[] = "0123456789abcdef";
	string result;
	// SHA256 has 32 byte, we encode 2 chars for each byte of SHA256.
	result.reserve(64);

	for (unsigned char byte : sha256) {
		result += kHexChars[byte >> 4];  // Get high 4 bits
		result += kHexChars[byte & 0xF]; // Get low 4 bits
	}
	return result;
}

void Sha256(const char *data, idx_t len, hash_bytes &out) {
	duckdb_mbedtls::MbedTlsWrapper::ComputeSha256Hash(data, len, reinterpret_cast<char *>(out));
}

string GetSha256(const string &input) {
	duckdb::hash_bytes sha256_val;
	static_assert(sizeof(sha256_val) == 32);
	Sha256(input.data(), input.length(), sha256_val);
	return Sha256ToHexString(sha256_val);
}

} // namespace duckdb
