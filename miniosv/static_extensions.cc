/*
 * The extensions linked into the image.
 *
 * v2.0 loads a linked-in extension through a registry: each one describes
 * itself to duckdb_register_static_extension, DBConfig copies the registry into
 * linked_extensions, and ExtensionHelper::LoadAllExtensions -- which
 * DuckDB::DuckDB calls during construction, config.options.load_extensions
 * being on by default -- walks that list. So opening a database still loads all
 * of them; this file only has to do the registering.
 *
 * Upstream generates an equivalent file from CMake
 * (extension/loader/static_extension_loader.c.in) and leans on a static
 * initialiser in extension/loader/static_extension_autoregister.cpp to call it
 * before main. The kernel defers the application's constructors, so that file
 * is left out of the build (miniosv/gen-sources.py) and main.cc calls this
 * explicitly instead, before it opens anything. The name is upstream's, so
 * anything else that expects the generated loader finds it.
 *
 * It matters that these are static. dlopen always fails on miniOSv
 * (libc/dlfcn.cc), so a runtime `LOAD parquet` can never work -- and with the
 * loadable-extensions sources left out of the build, DuckDB's own no-loader
 * provider says so with a PermissionException. Anything we want has to be here.
 */

#include "duckdb/main/database.hpp"
#include "duckdb_static_extension.h"

#include "core_functions_extension.hpp"
#include "parquet_extension.hpp"
#include "tpch_extension.hpp"
#include "autocomplete_extension.hpp"
#include "httpfs_extension.hpp"

extern "C" int32_t duckdb_register_static_extensions(void)
{
	using namespace duckdb;

	int32_t result = 0;
	// core_functions is not optional in practice: without it TPC-H fails on
	// sum/avg/extract, and so does most non-trivial SQL.
	result |= duckdb_register_static_extension(&StaticExtensionDescriber<CoreFunctionsExtension>::Describe);
	result |= duckdb_register_static_extension(&StaticExtensionDescriber<ParquetExtension>::Describe);
	result |= duckdb_register_static_extension(&StaticExtensionDescriber<TpchExtension>::Describe);
	result |= duckdb_register_static_extension(&StaticExtensionDescriber<AutocompleteExtension>::Describe);
	// https:// and s3://, over modules/mininet. Registering it costs nothing
	// when the network is down: the filesystems only reach the stack when a
	// path names one of their schemes.
	result |= duckdb_register_static_extension(&StaticExtensionDescriber<HttpfsExtension>::Describe);
	return result;
}
