#include "catch/catch.hpp"

#include <random>

#include "column_chunk_planner.hpp"
#include "parquet_layout_registry.hpp"

using namespace duckdb; // NOLINT

namespace {

// Extents [100,150), [150,180), [300,400); gaps [0,100), [180,300), [400,EOF).
const vector<FileExtent> kExtents {{100, 50}, {150, 30}, {300, 100}};
constexpr idx_t kFileSize = 500;
constexpr idx_t kGrid = 64;

struct RegisteredLayout {
	explicit RegisteredLayout(const string &path_p, vector<FileExtent> extents = kExtents) : path(path_p) {
		ParquetLayoutRegistry::Get().Clear();
		ParquetLayoutRegistry::Get().Register(path, std::move(extents));
	}
	~RegisteredLayout() {
		ParquetLayoutRegistry::Get().Clear();
	}
	string path;
};

vector<CacheReadChunk> Plan(const string &path, char *buffer, idx_t start, idx_t bytes, idx_t file_size = kFileSize) {
	return PlanColumnChunks(ColumnChunkPlanRequest {.buffer = buffer,
	                                                .requested_start_offset = start,
	                                                .requested_bytes_to_read = bytes,
	                                                .file_size = file_size,
	                                                .gap_block_size = kGrid},
	                        path);
}

} // namespace

TEST_CASE("ParquetLayoutRegistry drops empty and overlapping extents", "[column chunk planner]") {
	auto &registry = ParquetLayoutRegistry::Get();
	registry.Clear();
	const idx_t stored = registry.Register("f", {{300, 100}, {100, 50}, {120, 40} /*overlaps*/, {200, 0} /*empty*/});
	REQUIRE(stored == 2);

	idx_t start = 0, end = 0;
	REQUIRE(registry.Locate("f", 100, start, end) == LayoutRegion::kExtent); // first byte of an extent
	REQUIRE((start == 100 && end == 150));
	REQUIRE(registry.Locate("f", 150, start, end) == LayoutRegion::kGap); // one past its end
	REQUIRE((start == 150 && end == 300));
	REQUIRE(registry.Locate("f", 0, start, end) == LayoutRegion::kGap); // before the first extent
	REQUIRE((start == 0 && end == 100));
	REQUIRE(registry.Locate("f", 450, start, end) == LayoutRegion::kGap); // after the last: open-ended
	REQUIRE(start == 400);
	REQUIRE(end > kFileSize);
	REQUIRE(registry.Locate("other", 0, start, end) == LayoutRegion::kNoLayout);
	registry.Clear();
}

TEST_CASE("A read inside a column chunk caches the whole chunk", "[column chunk planner]") {
	RegisteredLayout layout {"f"};
	char buf[16];
	auto chunks = Plan("f", buf, 110, 10);
	REQUIRE(chunks.size() == 1);
	REQUIRE(chunks[0].aligned_start_offset == 100);
	REQUIRE(chunks[0].chunk_size == 50);
	REQUIRE(chunks[0].requested_start_offset == 110);
	REQUIRE(chunks[0].bytes_to_copy == 10);
	REQUIRE(chunks[0].requested_start_addr == buf);
}

TEST_CASE("A read across two chunks yields one entry per chunk", "[column chunk planner]") {
	RegisteredLayout layout {"f"};
	char buf[64];
	auto chunks = Plan("f", buf, 140, 30);
	REQUIRE(chunks.size() == 2);
	REQUIRE((chunks[0].aligned_start_offset == 100 && chunks[0].chunk_size == 50));
	REQUIRE((chunks[0].bytes_to_copy == 10 && chunks[0].requested_start_addr == buf));
	REQUIRE((chunks[1].aligned_start_offset == 150 && chunks[1].chunk_size == 30));
	REQUIRE((chunks[1].bytes_to_copy == 20 && chunks[1].requested_start_addr == buf + 10));
}

TEST_CASE("Gap entries do not depend on where the request starts", "[column chunk planner]") {
	RegisteredLayout layout {"f"};
	char buf[64];
	// Both land in grid cell [192,256), wholly inside gap [180,300).
	auto a = Plan("f", buf, 200, 10);
	auto b = Plan("f", buf, 205, 20);
	REQUIRE((a.size() == 1 && b.size() == 1));
	REQUIRE((a[0].aligned_start_offset == 192 && a[0].chunk_size == 64));
	REQUIRE((b[0].aligned_start_offset == a[0].aligned_start_offset && b[0].chunk_size == a[0].chunk_size));

	// The cell straddling the gap's start is clipped to the gap, never reaching into the chunk before it.
	auto c = Plan("f", buf, 185, 3);
	REQUIRE((c[0].aligned_start_offset == 180 && c[0].chunk_size == 12));
}

TEST_CASE("A read spanning chunks and gaps is covered contiguously", "[column chunk planner]") {
	RegisteredLayout layout {"f"};
	char buf[256];
	auto chunks = Plan("f", buf, 170, 150); // [170,320)
	struct Expect {
		idx_t entry_start, entry_size, copy_from, copy_bytes;
	};
	const vector<Expect> expected {
	    {150, 30, 170, 10}, // tail of chunk [150,180)
	    {180, 12, 180, 12}, // gap, cell [128,192) clipped to [180,192)
	    {192, 64, 192, 64}, // gap, cell [192,256)
	    {256, 44, 256, 44}, // gap, cell [256,320) clipped to [256,300)
	    {300, 100, 300, 20} // head of chunk [300,400)
	};
	REQUIRE(chunks.size() == expected.size());
	for (idx_t i = 0; i < expected.size(); ++i) {
		REQUIRE(chunks[i].aligned_start_offset == expected[i].entry_start);
		REQUIRE(chunks[i].chunk_size == expected[i].entry_size);
		REQUIRE(chunks[i].requested_start_offset == expected[i].copy_from);
		REQUIRE(chunks[i].bytes_to_copy == expected[i].copy_bytes);
		REQUIRE(chunks[i].requested_start_addr == buf + (expected[i].copy_from - 170));
	}
}

TEST_CASE("Entries are clipped to the end of the file", "[column chunk planner]") {
	RegisteredLayout layout {"f"};
	char buf[64];
	auto chunks = Plan("f", buf, 450, 50);
	REQUIRE(chunks.size() == 1);
	REQUIRE((chunks[0].aligned_start_offset == 448 && chunks[0].chunk_size == 52));
	REQUIRE(chunks[0].bytes_to_copy == 50);
}

TEST_CASE("A request past the end of the file is not planned at all", "[column chunk planner]") {
	RegisteredLayout layout {"f"};
	char buf[64];
	// [490,510) on a 500-byte file: a partial plan would leave 10 bytes of the caller's buffer unfilled.
	REQUIRE(Plan("f", buf, 490, 20).empty());
}

TEST_CASE("A file without a registered layout is not planned", "[column chunk planner]") {
	ParquetLayoutRegistry::Get().Clear();
	char buf[16];
	REQUIRE(Plan("unregistered", buf, 0, 10).empty());
}

TEST_CASE("Random requests are covered exactly, without straddling a chunk boundary", "[column chunk planner]") {
	// A layout with irregular chunk sizes and gaps, like a real footer would give.
	vector<FileExtent> extents;
	idx_t off = 37;
	std::mt19937_64 rng(42);
	while (off < 20000) {
		const idx_t size = 1 + rng() % 900;
		extents.push_back({off, size});
		off += size + (rng() % 3 == 0 ? rng() % 200 : 0);
	}
	const idx_t file_size = off + 333;
	RegisteredLayout layout {"fuzz", extents};

	auto inside_one_extent = [&](idx_t s, idx_t e) {
		for (auto &x : extents) {
			if (s == x.offset && e == x.End()) {
				return true;
			}
		}
		return false;
	};
	auto overlaps_any_extent = [&](idx_t s, idx_t e) {
		for (auto &x : extents) {
			if (s < x.End() && x.offset < e) {
				return true;
			}
		}
		return false;
	};

	vector<char> buf(file_size);
	for (int iter = 0; iter < 2000; ++iter) {
		const idx_t start = rng() % file_size;
		const idx_t bytes = 1 + rng() % std::min<idx_t>(4000, file_size - start);
		auto chunks = Plan("fuzz", buf.data(), start, bytes, file_size);
		REQUIRE(!chunks.empty());

		idx_t pos = start;
		for (auto &c : chunks) {
			const idx_t entry_end = c.aligned_start_offset + c.chunk_size;
			// Contiguous, in order, and every copy lies inside its entry.
			REQUIRE(c.requested_start_offset == pos);
			REQUIRE(c.requested_start_addr == buf.data() + (pos - start));
			REQUIRE(c.aligned_start_offset <= c.requested_start_offset);
			REQUIRE(c.requested_start_offset + c.bytes_to_copy <= entry_end);
			REQUIRE(entry_end <= file_size);
			// An entry is exactly one chunk, or lies wholly in a gap: never part of a chunk.
			REQUIRE((inside_one_extent(c.aligned_start_offset, entry_end) ||
			         !overlaps_any_extent(c.aligned_start_offset, entry_end)));
			pos += c.bytes_to_copy;
		}
		REQUIRE(pos == start + bytes);
	}
}
