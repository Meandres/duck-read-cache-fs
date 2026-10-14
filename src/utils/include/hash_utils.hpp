#pragma once

#include "duckdb/common/hash_functions.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {

// Convert SHA-256 to hex string format.
string Sha256ToHexString(const hash_bytes &sha256);

// SHA-256 of [data, data + len).
void Sha256(const char *data, idx_t len, hash_bytes &out);

// Get SHA-256 string for the given string.
string GetSha256(const string &input);

} // namespace duckdb
