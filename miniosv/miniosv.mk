# DuckDB as the miniOSv application. Included by app/Makefile for `make app=duckdb`.
#
# The engine is compiled from its real source tree, file by file, rather than
# through upstream's amalgamation: it parallelises, rebuilds incrementally, and
# errors name the file they came from. miniosv/sources.mk holds the generated
# list; regenerate it with miniosv/gen-sources.py after a version bump.
#
# Nothing under app/miniduckdb outside miniosv/ is modified -- the DuckDB tree
# is upstream v1.5.5 unchanged. Everything miniOSv needs is a shim here, put on
# the include path ahead of the real headers or force-included.

include app/miniduckdb/miniosv/sources.mk

duckdb-dir     := app/miniduckdb
duckdb-miniosv := $(duckdb-dir)/miniosv

# --- flags ------------------------------------------------------------------

# The include path DuckDB expects, plus our shim directory ahead of it.
duckdb-includes := -I$(duckdb-miniosv)/stubs \
                   $(addprefix -I,$(duckdb-include-dirs))

# jemalloc's own headers, and a shadowing config header (see stubs/jemalloc).
jemalloc-includes := -I$(duckdb-miniosv)/stubs/jemalloc \
                     -I$(duckdb-dir)/third_party/jemalloc/include

# The kernel builds -Wall -Werror; DuckDB and its vendored third-party trees do
# not, and are not ours to fix. Warnings are off for these objects only.
#
# -include wchar_shim.hpp: libc++ here is narrow-only, and both fmt and
#  <sstream> name std::wstring/wstringstream at parse time. See the shim.
#
# -DDUCKDB_DISABLE_BUILTIN_HTTPLIB: upstream's own gate; the vendored
#  cpp-httplib wants sys/socket.h, netdb.h and poll.h, none of which exist.
#
# -DDUCKDB_ENABLE_JEMALLOC: route DuckDB's Allocator at jemalloc. Symbols are
#  duckdb_je_ prefixed, so miniOSv's malloc is untouched.
# DUCKDB_VERSION / DUCKDB_SOURCE_ID: upstream's CMake derives these from git
# and src/function/table/version/pragma_version.cpp does not compile without
# them. sources.mk carries the captured values.
duckdb-defines := -DDUCKDB_DISABLE_BUILTIN_HTTPLIB \
                  -DDUCKDB_ENABLE_JEMALLOC \
                  -DDUCKDB_VERSION=\"$(duckdb-version)\" \
                  -DDUCKDB_SOURCE_ID=\"$(duckdb-source-id)\"

# Common to both languages. The wchar shim is C++ only -- force-including it
# into a C translation unit (jemalloc, zstd, brotli, libpg_query are all C)
# drags <string> and <sstream> in and fails with "'cstring' file not found".
duckdb-commonflags := $(duckdb-includes) $(jemalloc-includes) $(duckdb-defines) \
                      -w -Wno-error

# fstream_shim.hpp supplies std::ifstream/ofstream: libc++ here is built with
# LIBCXX_ENABLE_FILESYSTEM=OFF, so <iosfwd> declares them but <fstream> never
# defines them. The benchmark runner reads every .benchmark file with one.
duckdb-cxxflags := $(duckdb-commonflags) \
                   -include $(duckdb-miniosv)/stubs/wchar_shim.hpp \
                   -include include/osv/fstream_shim.hpp
duckdb-cflags   := $(duckdb-commonflags)

# aarch64 compiles -nostdinc, which hides clang's own freestanding headers and
# with them <arm_neon.h>. Add the resource dir back at lowest priority so it
# cannot shadow include/api.
ifeq ($(arch),aarch64)
duckdb-cxxflags += -isystem $(shell $(CXX) -print-resource-dir)/include
duckdb-cflags   += -isystem $(shell $(CC) -print-resource-dir)/include
endif

# dbgen declares `EXTERN long verbose`, which collides with the kernel's own
# `bool verbose` (core/debug.cc:24) at link time.
duckdb-tpch-flags := -Dverbose=dbgen_verbose \
                     -I$(duckdb-dir)/extension/tpch/include \
                     -I$(duckdb-dir)/extension/tpch/dbgen/include

# --- objects ----------------------------------------------------------------

duckdb-objects := $(duckdb-sources)
duckdb-objects := $(duckdb-objects:.cpp=.o)
duckdb-objects := $(duckdb-objects:.cc=.o)
duckdb-objects := $(duckdb-objects:.c=.o)

app-objects  = $(duckdb-objects)
app-objects += $(duckdb-miniosv)/main.o
app-objects += $(duckdb-miniosv)/static_extensions.o
app-objects += $(duckdb-miniosv)/benchmark_support.o
app-objects += $(duckdb-miniosv)/stubs/posix_stubs.o
app-objects += $(duckdb-miniosv)/fs/local_file_system.o

# Apply the flags to every DuckDB object. Target-specific variables cover the
# whole app/miniduckdb subtree, so this reaches the generated file list without
# naming each file.
$(out)/$(duckdb-dir)/%.o: CXXFLAGS += $(duckdb-cxxflags)
$(out)/$(duckdb-dir)/%.o: CFLAGS   += $(duckdb-cflags)
$(out)/$(duckdb-dir)/extension/tpch/%.o: CXXFLAGS += $(duckdb-tpch-flags)

# allocator_jemalloc.cpp calls StringUtil::Format without including
# duckdb/common/string_util.hpp. Upstream never notices because it compiles
# this inside a unity chunk where a sibling file supplies the header; building
# file by file exposes the missing include. Force it in rather than patch.
$(out)/$(duckdb-dir)/src/common/allocator/allocator_jemalloc.o: CXXFLAGS += \
    -include duckdb/common/string_util.hpp

# The benchmark runner ships its own main(). Rename it so it can be linked
# alongside the kernel's osv_app_main() and called from the dispatcher; after
# the macro it is an ordinary C++ function, not a program entry point.
# DUCKDB_ROOT_DIRECTORY is a CMake define upstream: the directory the runner
# resolves benchmark paths against. The .benchmark files live on the data disk,
# so it is the mount point here.
$(out)/$(duckdb-dir)/benchmark/benchmark_runner.o: CXXFLAGS += \
    -Dmain=duckdb_benchmark_main -DDUCKDB_ROOT_DIRECTORY=\"/db\"

# The CLI ships its own main() too. Its signature takes const char **, so
# main.cc wraps it rather than calling it directly.
$(out)/$(duckdb-dir)/tools/shell/shell.o: CXXFLAGS += -Dmain=duckdb_shell_main

# The miniosv/ sources use DuckDB's headers but are ours, so they build with
# the same include path and shim as the engine.
$(out)/$(duckdb-miniosv)/static_extensions.o: CXXFLAGS += $(duckdb-cxxflags) $(httpfs-includes)
$(out)/$(duckdb-miniosv)/benchmark_support.o: CXXFLAGS += $(duckdb-cxxflags)
$(out)/$(duckdb-miniosv)/stubs/posix_stubs.o: CXXFLAGS += $(duckdb-cxxflags)
$(out)/$(duckdb-miniosv)/fs/local_file_system.o: CXXFLAGS += $(duckdb-cxxflags)

# main.cc talks to both DuckDB and miniext.
$(out)/$(duckdb-miniosv)/main.o: CXXFLAGS += $(duckdb-includes) \
    -I$(duckdb-dir)/extension/core_functions/include \
    -I$(duckdb-dir)/extension/parquet/include \
    -I$(duckdb-dir)/extension/tpch/include \
    -include $(duckdb-miniosv)/stubs/wchar_shim.hpp \
    -include include/osv/fstream_shim.hpp -w -Wno-error

# The .cpp pattern rule lives in the top-level Makefile: llama.cpp needs it too.

# --- httpfs -----------------------------------------------------------------
#
# app/miniduckdb-httpfs is upstream duckdb-httpfs, unmodified, pinned at the
# last commit before it started requiring HTTPClient::Options -- a virtual the
# core in this tree does not declare. Nothing in it is patched: the HTTP client
# is chosen by which file gets compiled, which is upstream's own mechanism
# (httplib normally, a stub under Emscripten), so leaving those out of the list
# below and supplying miniosv/http/mininet_client.cpp instead is all it takes.
#
# Left out on purpose:
#   httpfs_httplib_client / httpfs_curl_client / httpfs_client_wasm
#       replaced by ours; the first two want sockets and libcurl anyway.
#   crypto.cpp
#       OpenSSL, and only reachable through OVERRIDE_ENCRYPTION_UTILS. Leaving
#       that undefined keeps EncryptionUtil as core's mbedtls implementation,
#       which is already compiled in -- so SigV4 needs nothing further.

httpfs-dir := app/miniduckdb-httpfs

httpfs-sources := \
    $(httpfs-dir)/src/httpfs.cpp \
    $(httpfs-dir)/src/http_state.cpp \
    $(httpfs-dir)/src/httpfs_connection_caching.cpp \
    $(httpfs-dir)/src/httpfs_extension.cpp \
    $(httpfs-dir)/src/create_secret_functions.cpp \
    $(httpfs-dir)/src/hash_functions.cpp \
    $(httpfs-dir)/src/hffs.cpp \
    $(httpfs-dir)/src/s3fs.cpp \
    $(httpfs-dir)/src/s3_multi_part_upload.cpp

httpfs-objects := $(httpfs-sources:.cpp=.o)

httpfs-includes := -I$(httpfs-dir)/src/include

$(out)/$(httpfs-dir)/%.o: CXXFLAGS += $(duckdb-cxxflags) $(httpfs-includes)

# Our client, and the C++ side of the stack it talks to.
include modules/mininet/mininet.mk

$(out)/$(duckdb-miniosv)/http/%.o: CXXFLAGS += \
    $(duckdb-cxxflags) $(httpfs-includes)

# main.cc brings the stack up, so it needs the endpoint. There is no resolver
# in the guest, so the address is a build-time constant exactly as it is for
# the benchmark:
#
#     make app=duckdb MININET_HOST=bucket.s3.eu-north-1.amazonaws.com \
#                     MININET_ADDR=3.5.216.240
#
# Leave MININET_HOST empty and the image simply has no network.
MININET_HOST ?=
MININET_ADDR ?= 0.0.0.0
MININET_WORKERS ?= 2
MININET_CONNS ?= 8
# 0 dials plain HTTP on port 80 instead of TLS on 443 -- isolates the network
# stack's cost from the TLS handshake, the way BENCH_SCHEME=http does for
# apps/bench/smoltcp-s3. The bucket policy needs no aws:SecureTransport deny
# for this to work; the one bucket_policy.py writes doesn't add one.
MININET_TLS ?= 1

# Those reach the compiler through a target-specific variable, and make does
# not rebuild an object when one changes -- it only compares timestamps. A
# stale address does not fail, it dials the wrong host, so record them in a
# file and depend on that.
duckdb-net-stamp = $(out)/$(duckdb-miniosv)/mininet-config.stamp
.PHONY: duckdb-net-phony
$(duckdb-net-stamp): duckdb-net-phony
	$(call very-quiet, $(makedir))
	@v='$(MININET_HOST) $(MININET_ADDR) $(MININET_WORKERS) $(MININET_CONNS) $(MININET_TLS)'; \
	 [ "$$(cat $@ 2>/dev/null)" = "$$v" ] || echo "$$v" > $@

$(out)/$(duckdb-miniosv)/main.o: $(duckdb-net-stamp)
$(out)/$(duckdb-miniosv)/main.o: CXXFLAGS += \
    -DMININET_HOST=\"$(MININET_HOST)\" \
    -DMININET_ADDR=\"$(MININET_ADDR)\" \
    -DMININET_WORKERS=$(MININET_WORKERS) \
    -DMININET_CONNS=$(MININET_CONNS) \
    -DMININET_TLS=$(MININET_TLS)

app-objects += $(httpfs-objects)
app-objects += $(duckdb-miniosv)/http/mininet_client.o
app-objects += $(duckdb-miniosv)/http/curl_unsupported.o
app-objects += $(mininet-lib-objects)
