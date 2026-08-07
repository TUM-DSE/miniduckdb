/*
 * The extensions linked into the image.
 *
 * Upstream generates this file from CMake (extension/generated_extension_loader
 * .cpp.in) and falls back to extension/loader/dummy_static_extension_loader.cpp
 * -- a no-op -- when nothing is linked. We do neither: this is the real thing,
 * hand-written, listing what miniOSv builds in.
 *
 * It matters that these are static. dlopen always fails on miniOSv
 * (libc/dlfcn.cc), so a runtime `LOAD parquet` can never work; anything we want
 * has to be here. DuckDB::DuckDB calls LoadAllExtensions during construction
 * when config.options.load_extensions is set, which is the default, so simply
 * opening a database registers all of them.
 */

#include "duckdb/main/extension_helper.hpp"

#include "core_functions_extension.hpp"
#include "parquet_extension.hpp"
#include "tpch_extension.hpp"

namespace duckdb {

void ExtensionHelper::LoadAllExtensions(DuckDB &db)
{
	// core_functions is not optional in practice: without it TPC-H fails on
	// sum/avg/extract, and so does most non-trivial SQL.
	db.LoadStaticExtension<CoreFunctionsExtension>();
	db.LoadStaticExtension<ParquetExtension>();
	db.LoadStaticExtension<TpchExtension>();
}

vector<string> ExtensionHelper::LoadedExtensionTestPaths()
{
	// Used only by upstream's test harness to find extension binaries on disk.
	// Everything here is compiled in, so there are no paths to report.
	return {};
}

} // namespace duckdb
