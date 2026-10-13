#include "catch/catch.hpp"

#include <random>
#include <thread>

#include "cache_httpfs_instance_state.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "mock_filesystem.hpp"
#include "parquet_layout_registry.hpp"
#include "scoped_directory.hpp"
#include "test_utils.hpp"

using namespace duckdb; // NOLINT

namespace {

using Reads = vector<std::pair<idx_t, idx_t>>;

// In-memory cache over a mock file of 'a's, which records every read it serves.
struct MockReader {
	DuckDB db;
	shared_ptr<CacheHttpfsInstanceState> instance_state;
	unique_ptr<CacheFileSystem> cache_fs;
	MockFileSystem *mock = nullptr;

	MockReader(idx_t file_size, idx_t block_size, idx_t max_read_bytes, bool coalesce = true) {
		auto mock_fs = make_uniq<MockFileSystem>([]() {}, []() {});
		mock_fs->SetFileSize(static_cast<int64_t>(file_size));
		mock = mock_fs.get();
		instance_state = make_shared_ptr<CacheHttpfsInstanceState>();
		auto &config = instance_state->config;
		config.cache_type = "in_mem";
		config.cache_block_size = block_size;
		config.enable_cache_validation = false;
		config.coalesce_misses = coalesce;
		config.max_coalesced_read_bytes = max_read_bytes;
		config.profile_type = *TEMP_PROFILE_TYPE;
		instance_state->db_instance = db.instance.get();
		SetInstanceState(*db.instance.get(), instance_state);
		InitializeCacheReaderForTest(instance_state, config);
		cache_fs = make_uniq<CacheFileSystem>(std::move(mock_fs), instance_state);
	}

	void Read(idx_t offset, idx_t bytes) {
		auto handle = cache_fs->OpenFile("file", FileOpenFlags::FILE_FLAGS_READ);
		string buffer(bytes, '\0');
		cache_fs->Read(*handle, &buffer[0], static_cast<int64_t>(bytes), offset);
		REQUIRE(buffer == string(bytes, 'a'));
	}

	Reads TakeReads() {
		Reads reads;
		for (const auto &op : mock->GetSortedReadOperations()) {
			reads.emplace_back(op.start_offset, static_cast<idx_t>(op.bytes_to_read));
		}
		mock->ClearReadOperations();
		return reads;
	}

	uint64_t RemoteReadCount() {
		auto &collector = instance_state->profile_collector_manager.GetProfileCollectorOrDefault(DConstants::INVALID_INDEX);
		return collector.GetCacheAccessInfo()[static_cast<idx_t>(CacheEntity::kData)]
		    .remote_read_count.GetValue<uint64_t>();
	}
};

} // namespace

TEST_CASE("Without coalescing, every missing block is its own read", "[coalesced reads]") {
	MockReader reader(/*file_size=*/26, /*block_size=*/5, /*max_read_bytes=*/10, /*coalesce=*/false);
	reader.Read(0, 26);
	REQUIRE(reader.TakeReads() == Reads {{0, 5}, {5, 5}, {10, 5}, {15, 5}, {20, 5}, {25, 1}});
}

TEST_CASE("Adjacent missing blocks are fetched with reads of at most the cap", "[coalesced reads]") {
	MockReader reader(/*file_size=*/26, /*block_size=*/5, /*max_read_bytes=*/10);
	reader.Read(0, 26);
	REQUIRE(reader.TakeReads() == Reads {{0, 10}, {10, 10}, {20, 6}});
	REQUIRE(reader.RemoteReadCount() == 3);

	// Cached as the same six blocks as without coalescing: a second read is all hits.
	reader.Read(0, 26);
	REQUIRE(reader.TakeReads().empty());
	reader.Read(12, 3);
	REQUIRE(reader.TakeReads().empty());
}

TEST_CASE("A cached block splits a run of misses", "[coalesced reads]") {
	MockReader reader(/*file_size=*/26, /*block_size=*/5, /*max_read_bytes=*/100);
	reader.Read(5, 5);
	REQUIRE(reader.TakeReads() == Reads {{5, 5}});
	reader.Read(0, 26);
	REQUIRE(reader.TakeReads() == Reads {{0, 5}, {10, 16}});
}

TEST_CASE("Column chunks are coalesced like blocks", "[coalesced reads]") {
	MockReader reader(/*file_size=*/100, /*block_size=*/8, /*max_read_bytes=*/25);
	reader.instance_state->config.chunking = *COLUMN_CHUNK_CHUNKING;
	ParquetLayoutRegistry::Get().Clear();
	ParquetLayoutRegistry::Get().Register("file", {{10, 30}, {40, 20}, {70, 30}});

	// A read inside chunk [10,40) fetches the whole chunk, in reads of at most the cap.
	reader.Read(15, 10);
	REQUIRE(reader.TakeReads() == Reads {{10, 25}, {35, 5}});

	// Entries [0,8) [8,10) miss, [10,40) hits, then chunk [40,60), gap entries [60,64) [64,70) and chunk [70,100)
	// miss: two runs.
	reader.Read(0, 100);
	REQUIRE(reader.TakeReads() == Reads {{0, 10}, {40, 25}, {65, 25}, {90, 10}});
	reader.Read(0, 100);
	REQUIRE(reader.TakeReads().empty());
	ParquetLayoutRegistry::Get().Clear();
}

TEST_CASE("A failed fetch releases its in-flight claims", "[coalesced reads]") {
	MockReader reader(/*file_size=*/26, /*block_size=*/5, /*max_read_bytes=*/10);
	reader.mock->SetThrowExceptionOnRead(true);
	REQUIRE_THROWS(reader.Read(0, 26));
	reader.mock->SetThrowExceptionOnRead(false);

	// Had the claims leaked, this would wait forever on fetches nobody is doing.
	reader.Read(0, 26);
	REQUIRE(reader.TakeReads() == Reads {{0, 10}, {10, 10}, {20, 6}});
}

TEST_CASE("Concurrent coalesced reads return the file's bytes", "[coalesced reads]") {
	ScopedDirectory dir(StringUtil::Format("/tmp/duckdb_test_coalesced_%s", UUID::ToString(UUID::GenerateRandomUUID())));
	const string path = dir.GetPath() + "/file";
	constexpr idx_t kFileSize = 200000;
	string content(kFileSize, '\0');
	std::mt19937_64 rng(7);
	for (auto &c : content) {
		c = static_cast<char>(rng());
	}
	{
		auto local = LocalFileSystem::CreateLocal();
		auto handle = local->OpenFile(path, FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE_NEW);
		local->Write(*handle, &content[0], kFileSize, /*location=*/0);
		handle->Sync();
	}

	auto chunking = GENERATE(as<string> {}, "block", "column_chunk");
	TestCacheConfig test_config;
	test_config.cache_type = "in_mem";
	test_config.cache_block_size = 4096;
	// Small, so that entries are evicted while other threads read them.
	test_config.max_in_mem_cache_block_count = 16;
	TestCacheFileSystemHelper helper(std::move(test_config));
	auto &config = helper.GetConfig();
	config.coalesce_misses = true;
	config.max_coalesced_read_bytes = 10000;
	config.chunking = chunking;
	ParquetLayoutRegistry::Get().Clear();
	if (chunking == "column_chunk") {
		vector<FileExtent> extents;
		for (idx_t off = 100; off + 3000 < kFileSize; off += 3000 + off % 7) {
			extents.push_back({off, 1000 + off % 2000});
		}
		ParquetLayoutRegistry::Get().Register(path, extents);
	}

	auto *cache_fs = helper.GetCacheFileSystem();
	std::atomic<idx_t> mismatches {0};
	vector<std::thread> threads;
	for (int t = 0; t < 8; ++t) {
		threads.emplace_back([&, t]() {
			std::mt19937_64 thread_rng(t);
			auto handle = cache_fs->OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
			for (int iter = 0; iter < 300; ++iter) {
				const idx_t start = thread_rng() % kFileSize;
				const idx_t bytes = 1 + thread_rng() % std::min<idx_t>(30000, kFileSize - start);
				string buffer(bytes, '\0');
				cache_fs->Read(*handle, &buffer[0], static_cast<int64_t>(bytes), start);
				if (buffer != content.substr(start, bytes)) {
					++mismatches;
				}
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	REQUIRE(mismatches == 0);
	ParquetLayoutRegistry::Get().Clear();
}
