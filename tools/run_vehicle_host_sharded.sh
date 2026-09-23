#!/usr/bin/env bash
# Full vehicle-host scoreboard in parallel: N shard processes, totals summed.
# Usage: tools/run_vehicle_host_sharded.sh [N=16] [outdir]
# Per-shard logs keep every study section; SCORE lines are summed here.
set -u
N="${1:-16}"
OUT="${2:-${TMPDIR:-/tmp}/vehicle_host_shards}"
EXE="$(dirname "$0")/../build/Release/expression_vehicle_host_test.exe"
mkdir -p "$OUT"
start=$(date +%s)
for ((k = 0; k < N; k++)); do
    BF6_SHARD="$k/$N" "$EXE" > "$OUT/shard_$k.txt" 2>&1 &
done
wait
end=$(date +%s)
cat "$OUT"/shard_*.txt | awk '/^SCORE /{cs+=$2; cu+=$3; ts+=$4; tu+=$5; q+=$6; h+=$7}
  END {printf "CONTROL %d sound, %d unresolved uses\nTEST    %d sound, %d unresolved uses\nrays %d, hits %d\n", cs, cu, ts, tu, q, h}'
echo "shards: $N   wall: $((end - start))s   logs: $OUT"
