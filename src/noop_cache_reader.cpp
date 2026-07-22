#include "noop_cache_reader.hpp"

#include "cache_filesystem.hpp"
#include "cache_httpfs_instance_state.hpp"
#include "remote_simulation.hpp"

namespace duckdb {

void NoopCacheReader::ReadAndCache(FileHandle &handle, char *buffer, idx_t requested_start_offset,
                                   idx_t requested_bytes_to_read, idx_t file_size) {
	auto &cache_handle = handle.Cast<CacheFileSystemHandle>();
	auto *internal_filesystem = cache_handle.GetInternalFileSystem();

	auto state = instance_state.lock();
	auto &collector = GetProfileCollectorOrThrow(state, cache_handle.GetConnectionId());
	const auto latency_guard = collector.RecordOperationStart(IoOperation::kRead);
	// The no-caching baseline has to pay the simulated remote cost too, or it
	// would look artificially good next to the configurations that cache.
	SimulateRemoteRead(state->config, requested_bytes_to_read);
	internal_filesystem->Read(*cache_handle.internal_file_handle, buffer, requested_bytes_to_read,
	                          requested_start_offset);
}

} // namespace duckdb
