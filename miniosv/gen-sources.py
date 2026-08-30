#!/usr/bin/env python3
"""Regenerate miniosv/sources.mk, the list of DuckDB sources the kernel builds.

    miniosv/gen-sources.py

Run this when the DuckDB source tree changes (a version bump, or adding an
extension below) and commit the result. The list is checked in rather than
generated during the build so that `make` needs no Python, and so a version
bump shows up as a reviewable diff instead of silently changing what is
compiled.

Why not the amalgamation: upstream's scripts/amalgamation.py concatenates the
whole engine into one translation unit, which builds serially, has to be
compiled with warnings off wholesale, and forces extensions to be compiled
against src/include while the engine is compiled against the amalgamated
header -- mixing the two double-defines, because amalgamation strips include
guards. Compiling the real tree file by file parallelises, rebuilds
incrementally, and gives errors that name the actual file.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # the DuckDB tree root

sys.path.insert(0, os.path.join(ROOT, "scripts"))
import amalgamation  # noqa: E402  (needs the path above)

# Statically linked extensions. Runtime LOAD is dead on miniOSv -- dlopen
# always fails -- so anything we want has to be compiled in.
#
#   core_functions  required: TPC-H fails on sum/avg/extract without it
#   parquet         read_parquet / COPY TO ... (PARQUET)
#   tpch            dbgen + the 22 queries
#   autocomplete    the CLI's tab completion; shell.cpp includes its header
EXTENSIONS = ["core_functions", "parquet", "tpch", "autocomplete"]

# Third-party trees pulled in only by the extensions above (parquet needs
# thrift for the file format and the compression codecs).
EXTENSION_THIRD_PARTY = [
    "third_party/parquet",
    "third_party/thrift",
    "third_party/snappy",
    "third_party/lz4",
    "third_party/brotli",
]

# jemalloc is DuckDB's allocator extension. Every symbol is prefixed
# duckdb_je_, so it does not replace miniOSv's malloc; DuckDB reaches it
# through its own Allocator.
JEMALLOC_DIR = "third_party/jemalloc/src"

# The benchmark runner (https://duckdb.org/docs/current/dev/benchmark). It has
# its own main(), which miniosv.mk renames so the dispatcher can call it.
# interpreted_benchmark.cpp reads the .benchmark files at run time through
# FileSystem, so they live on the data disk rather than in the image.
BENCHMARK_DIRS = ["benchmark"]

# interpreted_benchmark.cpp uses exactly one function from the test helpers,
# DeleteDatabase(). Building test/helpers/test_helpers.cpp to get it drags in
# the whole test framework (TestConfiguration, catch's main, duckdb::getpid),
# so miniosv/benchmark_support.cc supplies that one function instead.
BENCHMARK_EXTRA = []

# The DuckDB CLI. Its main() is renamed by miniosv.mk, like the benchmark
# runner's. tests/ is upstream's own test harness and is not built.
SHELL_DIRS = ["tools/shell"]
SHELL_SKIP = ("tools/shell/tests",)

# Files that are #included by another source rather than compiled on their own.
# DuckDB keeps the same list in scripts/package_build.py:9.
EXCLUDED = {
    "utf8proc_data.cpp",
    "dummy_static_extension_loader.cpp",
    # Only compiled when OVERRIDE_NEW_DELETE is on, which DuckDB leaves off
    # (third_party/jemalloc/CMakeLists.txt:73-74). Compiling it anyway fails
    # with "OVERRIDE_NEW_DELETE not properly defined" -- and we do not want
    # jemalloc taking over global new/delete from miniOSv's allocator.
    "jemalloc_cpp.cpp",
    # Replaced by miniosv/fs/local_file_system.cpp: on miniOSv miniext IS the
    # local filesystem, and upstream's version is built entirely on POSIX calls
    # that fail here. Replacing the class rather than adding one beside it means
    # FileSystem::CreateLocal() -- which is static, and which the benchmark
    # runner calls in four places -- returns the right thing without any call
    # site being told.
    "local_file_system.cpp",
}


def walk(rel_dir, exts=(".cpp", ".c", ".cc")):
    out = []
    for dirpath, _, files in os.walk(os.path.join(ROOT, rel_dir)):
        for name in sorted(files):
            if not name.endswith(exts) or name in EXCLUDED:
                continue
            full = os.path.join(dirpath, name)
            out.append(os.path.relpath(full, ROOT))
    return sorted(out)


def main():
    # amalgamation.list_sources() walks relative paths, so it has to run from
    # the DuckDB root.
    os.chdir(ROOT)

    core = [s for s in amalgamation.list_sources()
            if os.path.basename(s) not in EXCLUDED]

    groups = [("DuckDB engine and its third-party dependencies", core)]

    for ext in EXTENSIONS:
        groups.append((f"extension: {ext}", walk(f"extension/{ext}")))
    for tp in EXTENSION_THIRD_PARTY:
        groups.append((f"extension dependency: {os.path.basename(tp)}", walk(tp)))
    groups.append(("jemalloc (duckdb_je_ prefixed)", walk(JEMALLOC_DIR)))
    for d in BENCHMARK_DIRS:
        groups.append((f"benchmark runner: {d}", walk(d)))
    for d in SHELL_DIRS:
        srcs = [s for s in walk(d) if not s.startswith(SHELL_SKIP)]
        groups.append((f"CLI: {d}", srcs))
    groups.append(("benchmark runner: test helpers", sorted(BENCHMARK_EXTRA)))

    include_dirs = amalgamation.list_include_dirs()

    # Each extension keeps its own include/ tree, and its sources include
    # headers relative to it (e.g. "core_functions/aggregate/...").
    for ext in EXTENSIONS:
        d = f"extension/{ext}/include"
        if os.path.isdir(os.path.join(ROOT, d)):
            include_dirs.append(d)
    # thrift and the codecs are included by path from the parquet sources.
    for tp in EXTENSION_THIRD_PARTY:
        if os.path.isdir(os.path.join(ROOT, tp)):
            include_dirs.append(tp)
    # The benchmark runner's own headers, and the test helpers it borrows.
    for d in ["benchmark/include", "test/include", "third_party/catch",
              "tools/shell/include", "tools/shell/linenoise/include",
              "extension/autocomplete/include"]:
        if os.path.isdir(os.path.join(ROOT, d)):
            include_dirs.append(d)

    # Upstream's CMake derives these from git, and pragma_version.cpp will not
    # compile without them. Capture them here so `make` needs no git.
    version = amalgamation.git_dev_version()
    source_id = amalgamation.git_commit_hash()

    lines = [
        "# Generated by miniosv/gen-sources.py -- do not edit by hand.",
        "#",
        "# The DuckDB sources compiled into the kernel image, and the include",
        "# directories they need. Paths are relative to this tree's root;",
        "# miniosv.mk prefixes them with wherever the tree sits.",
        "",
        f"duckdb-version   := {version}",
        f"duckdb-source-id := {source_id}",
        "",
        "duckdb-include-dirs :=",
    ]
    for d in include_dirs:
        lines.append(f"duckdb-include-dirs += {d}")
    lines.append("")

    total = 0
    for title, srcs in groups:
        lines.append(f"# --- {title} ({len(srcs)} files)")
        lines.append("duckdb-sources :=" if total == 0 else "")
        for s in srcs:
            lines.append(f"duckdb-sources += {s}")
        lines.append("")
        total += len(srcs)

    out = os.path.join(HERE, "sources.mk")
    with open(out, "w") as fh:
        fh.write("\n".join(lines).replace("\n\n\n", "\n\n"))
    print(f"gen-sources: wrote {out} ({total} sources, "
          f"{len(include_dirs)} include dirs)", file=sys.stderr)


if __name__ == "__main__":
    main()
