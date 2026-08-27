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
#include "autocomplete_extension.hpp"
#include "httpfs_extension.hpp"
#include "httpfs_client.hpp"

#include "duckdb/main/config.hpp"

namespace duckdb {

void ExtensionHelper::LoadAllExtensions(DuckDB &db)
{
	// core_functions is not optional in practice: without it TPC-H fails on
	// sum/avg/extract, and so does most non-trivial SQL.
	LoadExtension(db, "core_functions");
	LoadExtension(db, "parquet");
	LoadExtension(db, "tpch");
	// The CLI's tab completion.
	LoadExtension(db, "autocomplete");
	// https:// and s3://, over modules/mininet. Registering it costs nothing
	// when the network is down: the filesystems only reach the stack when a
	// path actually names one of their schemes.
	LoadExtension(db, "httpfs");
}

//! Named lookup, for callers that ask for an extension by string (the benchmark
//! runner's `require` directive). It has to do the registering, not just report
//! availability: a benchmark opens its database with load_extensions off, so
//! LoadAllExtensions never ran. LoadStaticExtension is idempotent.
ExtensionLoadResult ExtensionHelper::LoadExtension(DuckDB &db, const std::string &extension)
{
	if (extension == "core_functions") {
		db.LoadStaticExtension<CoreFunctionsExtension>();
	} else if (extension == "parquet") {
		db.LoadStaticExtension<ParquetExtension>();
	} else if (extension == "tpch") {
		db.LoadStaticExtension<TpchExtension>();
	} else if (extension == "autocomplete") {
		db.LoadStaticExtension<AutocompleteExtension>();
	} else if (extension == "httpfs") {
		db.LoadStaticExtension<HttpfsExtension>();
		// httpfs installs HTTPFSCurlUtil as the default HTTP backend at the end
		// of its Load, and curl does not exist here -- its InitializeClient is
		// the one in miniosv/http/curl_unsupported.cpp, which throws. Point the
		// configuration back at the base HTTPFSUtil, whose InitializeClient is
		// the mininet client. SetHTTPUtil is public API and this runs after
		// Load, so nothing upstream has to change; without it every http:// and
		// s3:// query fails with "the curl backend is not available".
		DBConfig::GetConfig(*db.instance).SetHTTPUtil(make_shared_ptr<HTTPFSUtil>());
	} else {
		return ExtensionLoadResult::NOT_LOADED;
	}
	return ExtensionLoadResult::LOADED_EXTENSION;
}

vector<string> ExtensionHelper::LoadedExtensionTestPaths()
{
	// Used only by upstream's test harness to find extension binaries on disk.
	// Everything here is compiled in, so there are no paths to report.
	return {};
}

} // namespace duckdb
