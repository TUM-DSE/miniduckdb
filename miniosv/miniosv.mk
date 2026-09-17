# DuckDB as the miniOSv application. Included by the app Makefile that names
# this tree -- see apps/bench/duckdb-tpch/Makefile in miniosv_apps.
#
# The engine is compiled from its real source tree, file by file, rather than
# through upstream's amalgamation: it parallelises, rebuilds incrementally, and
# errors name the file they came from. miniosv/sources.mk holds the generated
# list; regenerate it with miniosv/gen-sources.py after a version bump.
#
# Nothing in this repository outside miniosv/ is modified -- the DuckDB tree
# is upstream v1.5.5 unchanged. Everything miniOSv needs is a shim here, put on
# the include path ahead of the real headers or force-included.

# Every path below is derived from this checkout's root, so it can live
# anywhere: inside the miniOSv tree under app/, or beside it in a separate apps
# checkout. The includer may say where that is; otherwise it is this file's own
# grandparent directory -- $(MAKEFILE_LIST)'s last word is this file while it is
# being read, which is why the default is computed here and not deferred.
ifndef duckdb-dir
duckdb-dir := $(patsubst %/miniosv,%,$(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST)))))
endif
duckdb-miniosv := $(duckdb-dir)/miniosv

# Objects do not mirror the source path under $(out): an out-of-tree checkout
# has no path relative to the miniOSv root that stays inside it, and "../" in
# an object path escapes the per-mode/arch build directory.
#
# They go under $(app) instead, which is the one place the kernel allows. It
# generates $(out)/app_init_late.ld from that variable --
#
#     KEEP(*$(app)/*(... .init_array .ctors))
#
# -- and arch/*/loader.ld INCLUDEs it to move the application's constructors
# into .init_array_late, which runs from the main thread once the allocator,
# scheduler and console exist. Everything left in .init_array runs far earlier,
# in premain(). DuckDB has thousands of static constructors and they allocate,
# so an object path outside that glob links cleanly and then dies in premain,
# before the kernel prints its first line.
#
# The leading "/" goes because $(out)/ already supplies one; the glob is a
# plain substring match either way, and this keeps "//" out of every path.
duckdb-objdir := $(patsubst /%,%,$(app))/duckdb
httpfs-objdir := $(patsubst /%,%,$(app))/httpfs

include $(duckdb-miniosv)/sources.mk

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

# $(out)/$(duckdb-objdir)/<path>.o for a source at $(duckdb-dir)/<path>, so the
# object tree mirrors the checkout no matter where the checkout is. The kernel
# Makefile's own $(out)/%.o rules cannot do this -- they derive the object path
# from the source path -- so the three compiles are spelled out here.
$(out)/$(duckdb-objdir)/%.o: $(duckdb-dir)/%.cpp | generated-headers $(out)/.libcxx-built
	$(makedir)
	$(call quiet, $(CXX) $(CXXFLAGS) -c -o $@ $<, CXX duckdb/$*.cpp)

$(out)/$(duckdb-objdir)/%.o: $(duckdb-dir)/%.cc | generated-headers $(out)/.libcxx-built
	$(makedir)
	$(call quiet, $(CXX) $(CXXFLAGS) -c -o $@ $<, CXX duckdb/$*.cc)

$(out)/$(duckdb-objdir)/%.o: $(duckdb-dir)/%.c | generated-headers
	$(makedir)
	$(call quiet, $(CC) $(CFLAGS) -c -o $@ $<, CC duckdb/$*.c)

duckdb-objects := $(patsubst $(duckdb-dir)/%,$(duckdb-objdir)/%,$(duckdb-sources))
duckdb-objects := $(duckdb-objects:.cpp=.o)
duckdb-objects := $(duckdb-objects:.cc=.o)
duckdb-objects := $(duckdb-objects:.c=.o)

app-objects  = $(duckdb-objects)
app-objects += $(duckdb-objdir)/miniosv/main.o
app-objects += $(duckdb-objdir)/miniosv/static_extensions.o
app-objects += $(duckdb-objdir)/miniosv/benchmark_support.o
app-objects += $(duckdb-objdir)/miniosv/stubs/posix_stubs.o
app-objects += $(duckdb-objdir)/miniosv/fs/local_file_system.o

# Apply the flags to every DuckDB object. Target-specific variables cover the
# whole object subtree, so this reaches the generated file list without naming
# each file.
$(out)/$(duckdb-objdir)/%.o: CXXFLAGS += $(duckdb-cxxflags)
$(out)/$(duckdb-objdir)/%.o: CFLAGS   += $(duckdb-cflags)
$(out)/$(duckdb-objdir)/extension/tpch/%.o: CXXFLAGS += $(duckdb-tpch-flags)

# allocator_jemalloc.cpp calls StringUtil::Format without including
# duckdb/common/string_util.hpp. Upstream never notices because it compiles
# this inside a unity chunk where a sibling file supplies the header; building
# file by file exposes the missing include. Force it in rather than patch.
$(out)/$(duckdb-objdir)/src/common/allocator/allocator_jemalloc.o: CXXFLAGS += \
    -include duckdb/common/string_util.hpp

# The benchmark runner ships its own main(). Rename it so it can be linked
# alongside the kernel's osv_app_main() and called from the dispatcher; after
# the macro it is an ordinary C++ function, not a program entry point.
# DUCKDB_ROOT_DIRECTORY is a CMake define upstream: the directory the runner
# resolves benchmark paths against. The .benchmark files live on the data disk,
# so it is the mount point here.
$(out)/$(duckdb-objdir)/benchmark/benchmark_runner.o: CXXFLAGS += \
    -Dmain=duckdb_benchmark_main -DDUCKDB_ROOT_DIRECTORY=\"/db\"

# The CLI ships its own main() too. Its signature takes const char **, so
# main.cc wraps it rather than calling it directly.
$(out)/$(duckdb-objdir)/tools/shell/shell.o: CXXFLAGS += -Dmain=duckdb_shell_main

# The miniosv/ sources use DuckDB's headers but are ours, so they build with
# the same include path and shim as the engine.
$(out)/$(duckdb-objdir)/miniosv/static_extensions.o: CXXFLAGS += $(duckdb-cxxflags) $(httpfs-includes)
$(out)/$(duckdb-objdir)/miniosv/benchmark_support.o: CXXFLAGS += $(duckdb-cxxflags)
$(out)/$(duckdb-objdir)/miniosv/stubs/posix_stubs.o: CXXFLAGS += $(duckdb-cxxflags)
$(out)/$(duckdb-objdir)/miniosv/fs/local_file_system.o: CXXFLAGS += $(duckdb-cxxflags)

# main.cc talks to both DuckDB and miniext.
$(out)/$(duckdb-objdir)/miniosv/main.o: CXXFLAGS += $(duckdb-includes) \
    -I$(duckdb-dir)/extension/core_functions/include \
    -I$(duckdb-dir)/extension/parquet/include \
    -I$(duckdb-dir)/extension/tpch/include \
    -include $(duckdb-miniosv)/stubs/wchar_shim.hpp \
    -include include/osv/fstream_shim.hpp -w -Wno-error

# --- httpfs -----------------------------------------------------------------
#
# The httpfs checkout is upstream duckdb-httpfs, unmodified, pinned at the
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

# A checkout of its own, not a subdirectory of this one, so the includer says
# where it is. The default is the sibling layout the apps repository uses.
httpfs-dir ?= $(abspath $(duckdb-dir)/../miniduckdb-httpfs)

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

httpfs-objects := $(patsubst $(httpfs-dir)/%,$(httpfs-objdir)/%,$(httpfs-sources))
httpfs-objects := $(httpfs-objects:.cpp=.o)

httpfs-includes := -I$(httpfs-dir)/src/include

# Same reason as the DuckDB rules above: the object path is ours to choose, so
# it cannot come from the kernel Makefile's source-derived pattern.
$(out)/$(httpfs-objdir)/%.o: $(httpfs-dir)/%.cpp | generated-headers $(out)/.libcxx-built
	$(makedir)
	$(call quiet, $(CXX) $(CXXFLAGS) -c -o $@ $<, CXX httpfs/$*.cpp)

$(out)/$(httpfs-objdir)/%.o: CXXFLAGS += $(duckdb-cxxflags) $(httpfs-includes)

# Our client, and the C++ side of the stack it talks to.
include modules/mininet/mininet.mk

$(out)/$(duckdb-objdir)/miniosv/http/%.o: CXXFLAGS += \
    $(duckdb-cxxflags) $(httpfs-includes)

# main.cc brings the stack up, so it needs the endpoint. There is no resolver
# in the guest, so the address is a build-time constant exactly as it is for
# the benchmark:
#
#     just build apps/bench/duckdb-tpch \
#         MININET_HOST=bucket.s3.eu-north-1.amazonaws.com \
#         MININET_ADDR=3.5.216.240
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
duckdb-net-stamp = $(out)/$(duckdb-objdir)/miniosv/mininet-config.stamp
.PHONY: duckdb-net-phony
$(duckdb-net-stamp): duckdb-net-phony
	$(call very-quiet, $(makedir))
	@v='$(MININET_HOST) $(MININET_ADDR) $(MININET_WORKERS) $(MININET_CONNS) $(MININET_TLS)'; \
	 [ "$$(cat $@ 2>/dev/null)" = "$$v" ] || echo "$$v" > $@

$(out)/$(duckdb-objdir)/miniosv/main.o: $(duckdb-net-stamp)
$(out)/$(duckdb-objdir)/miniosv/main.o: CXXFLAGS += \
    -DMININET_HOST=\"$(MININET_HOST)\" \
    -DMININET_ADDR=\"$(MININET_ADDR)\" \
    -DMININET_WORKERS=$(MININET_WORKERS) \
    -DMININET_CONNS=$(MININET_CONNS) \
    -DMININET_TLS=$(MININET_TLS)

app-objects += $(httpfs-objects)
app-objects += $(duckdb-objdir)/miniosv/http/mininet_client.o
app-objects += $(duckdb-objdir)/miniosv/http/curl_unsupported.o
app-objects += $(mininet-lib-objects)
