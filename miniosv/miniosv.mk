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

duckdb-cxxflags := $(duckdb-commonflags) \
                   -include $(duckdb-miniosv)/stubs/wchar_shim.hpp
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

# The miniosv/ sources use DuckDB's headers but are ours, so they build with
# the same include path and shim as the engine.
$(out)/$(duckdb-miniosv)/static_extensions.o: CXXFLAGS += $(duckdb-cxxflags)
$(out)/$(duckdb-miniosv)/stubs/posix_stubs.o: CXXFLAGS += $(duckdb-cxxflags)
$(out)/$(duckdb-miniosv)/fs/local_file_system.o: CXXFLAGS += $(duckdb-cxxflags)

# main.cc talks to both DuckDB and miniext.
$(out)/$(duckdb-miniosv)/main.o: CXXFLAGS += $(duckdb-includes) \
    -I$(duckdb-dir)/extension/core_functions/include \
    -I$(duckdb-dir)/extension/parquet/include \
    -I$(duckdb-dir)/extension/tpch/include \
    -include $(duckdb-miniosv)/stubs/wchar_shim.hpp -w -Wno-error

# --- rules ------------------------------------------------------------------

# The kernel has pattern rules for .cc/.c/.S/.s but not .cpp, which is what
# DuckDB uses. Same recipe as the .cc rule in the top-level Makefile, including
# the order-only dependency on the libc++ build.
$(out)/%.o: %.cpp | generated-headers $(out)/.libcxx-built
	$(makedir)
	$(call quiet, $(CXX) $(CXXFLAGS) -c -o $@ $<, CXX $*.cpp)
