#!/usr/bin/env bash
#
# Build a data image holding pre-generated TPC-H parquet files and a benchmark
# group that queries them, for any set of scale factors.
#
#   miniosv/mk-tpch.sh --src /scratch/ilya --img /scratch/tpch.img 1 10
#   scripts/run.py --image-path build/duckdb.x64/loader.img -m 32G \
#       --emulated-nvme /scratch/tpch.img --args "benchmark --sf 10 '^Q06$'"
#
# --src holds one tpch<SF> directory per scale factor, each with the eight
# tables as <table>.parquet. They are hardlinked into the staging tree when the
# staging tree is on the same filesystem, so nothing is copied twice.
#
# The generated benchmarks take the scale factor as a runtime argument, so one
# image with several SFs on it serves all of them:
#
#   benchmark --sf 1  '^Q06$'      one query
#   benchmark --sf 10 '^Q[0-9]+$'  all 22
#
# Answers are only shipped for sf1 and sf100, so only those runs verify their
# results; the others are timed but unchecked.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
duckdb_root=$(cd "$here/.." && pwd)
miniosv_root=$(cd "$here/../../.." && pwd)

src=""
img=""
stage=""
size=""
tables="customer lineitem nation orders part partsupp region supplier"

usage() {
    echo "usage: $(basename "$0") --src DIR --img IMAGE [--stage DIR] [--size N] SF..." >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --src)   src=$2;   shift 2 ;;
        --img)   img=$2;   shift 2 ;;
        --stage) stage=$2; shift 2 ;;
        --size)  size=$2;  shift 2 ;;
        -h|--help) usage ;;
        -*) echo "$(basename "$0"): unknown option $1" >&2; usage ;;
        *) break ;;
    esac
done

[ -n "$src" ] && [ -n "$img" ] && [ $# -ge 1 ] || usage
sfs=("$@")
stage=${stage:-$(dirname "$img")/tpch-stage}

rm -rf "$stage"
mkdir -p "$stage/benchmark/tpch-parquet" "$stage/extension/tpch/dbgen"

for sf in "${sfs[@]}"; do
    dir=$src/tpch$sf
    [ -d "$dir" ] || { echo "$(basename "$0"): no such directory: $dir" >&2; exit 1; }
    mkdir -p "$stage/tpch$sf"
    for t in $tables; do
        [ -f "$dir/$t.parquet" ] || { echo "$(basename "$0"): missing $dir/$t.parquet" >&2; exit 1; }
        cp -l "$dir/$t.parquet" "$stage/tpch$sf/" 2>/dev/null ||
            cp "$dir/$t.parquet" "$stage/tpch$sf/"
    done
done

cp -r "$duckdb_root/extension/tpch/dbgen/queries" "$stage/extension/tpch/dbgen/"
mkdir -p "$stage/extension/tpch/dbgen/answers"
for sf in 1 100; do
    answers=$duckdb_root/extension/tpch/dbgen/answers/sf$sf
    [ -d "$answers" ] && cp -r "$answers" "$stage/extension/tpch/dbgen/answers/"
done

# The template. ${sf} is substituted as each line is read, so `argument sf`
# has to come before every use of it, and the load block picks it up too.
{
    echo '# name: ${FILE_PATH}'
    echo '# description: TPC-H over pre-generated parquet at scale factor ${sf}'
    echo '# group: [tpch-parquet]'
    echo
    echo 'argument sf 1'
    echo
    echo 'require tpch'
    echo
    echo 'require parquet'
    echo
    echo 'name Q${QUERY_NUMBER_PADDED}'
    echo 'group tpch-parquet'
    echo 'subgroup sf${sf}'
    echo
    echo 'load'
    for t in $tables; do
        echo "create view $t as select * from '/db/tpch\${sf}/$t.parquet';"
    done
    echo
    echo 'run extension/tpch/dbgen/queries/q${QUERY_NUMBER_PADDED}.sql'
    echo
    echo 'result extension/tpch/dbgen/answers/sf1/q${QUERY_NUMBER_PADDED}.csv sf=1'
    echo
    echo 'result extension/tpch/dbgen/answers/sf100/q${QUERY_NUMBER_PADDED}.csv sf=100'
} > "$stage/benchmark/tpch-parquet/tpch_parquet.benchmark.in"

for n in $(seq -w 1 22); do
    {
        echo "# name: benchmark/tpch-parquet/q$n.benchmark"
        echo "# description: Run query $n from the TPC-H benchmark over parquet"
        echo "# group: [tpch-parquet]"
        echo
        echo "template benchmark/tpch-parquet/tpch_parquet.benchmark.in"
        echo "QUERY_NUMBER=${n#0}"
        echo "QUERY_NUMBER_PADDED=$n"
    } > "$stage/benchmark/tpch-parquet/q$n.benchmark"
done

if [ -z "$size" ]; then
    kb=$(du -sk --apparent-size "$stage" | cut -f1)
    size=$(( kb / 1024 * 12 / 10 + 512 ))M
fi

"$miniosv_root/scripts/mkdata.sh" "$img" "$stage" "$size"

echo
echo "scale factors on $img: ${sfs[*]}"
echo "run one with:"
echo "  scripts/run.py --image-path build/duckdb.x64/loader.img -m 16G \\"
echo "      --emulated-nvme $img --args \"benchmark --sf ${sfs[0]} '^Q06\$'\""
