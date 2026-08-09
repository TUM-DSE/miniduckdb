/*
 * The one piece of the test-helper library the benchmark runner needs.
 *
 * interpreted_benchmark.cpp calls DeleteDatabase() before running a benchmark
 * that uses an on-disk database. Upstream's definition lives in
 * test/helpers/test_helpers.cpp, which also drags in TestConfiguration, catch's
 * main() and duckdb::getpid() -- the whole test framework, for one function.
 */

#include "duckdb/common/file_system.hpp"

namespace duckdb {

void DeleteDatabase(string path)
{
	auto fs = FileSystem::CreateLocal();
	// Both may legitimately be absent; TryRemoveFile does not complain.
	fs->TryRemoveFile(path);
	fs->TryRemoveFile(path + ".wal");
}

} // namespace duckdb
