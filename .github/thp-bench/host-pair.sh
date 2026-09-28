#!/bin/bash
# host-pair.sh DIM TAG CPUS_A CPUS_B  (runs on the runner host, not in the container)
# Runs pair.sh in the bench container. If perf was set up, it also counts TLB events
# for each server process, starting when pair.py begins the measured window.
set -e
DIM=$1; TAG=$2; CA=$3; CB=$4
W=$RUNNER_TEMP/work; PAIR=$W/data-$DIM/pair; MARK=$PAIR/pair-$TAG.json.measuring
docker exec -e CLIENT_CPUS=2-3 bench /s/bench1m/pair.sh "/s/bench1m/data-$DIM" "$DIM" \
  "$WARMUP_S" "$MEASURE_S" "$CA" "$CB" "$TAG" "$EF_RUNTIME" &
pair=$!
perfs=()
if [ -s "$W/perf-events" ]; then
  PERF=$(cat "$W/perf-bin"); EVENTS=$(cat "$W/perf-events")
  while [ ! -f "$MARK" ] && kill -0 $pair 2>/dev/null; do sleep 0.2; done
  if [ -f "$MARK" ]; then
    for x in "A 7001" "B 7002"; do set -- $x
      pid=$(pgrep -f "valkey-server .*\*:$2( |$)" | head -1)
      sudo "$PERF" stat -x, -e "$EVENTS" -p "$pid" -o "$PAIR/perf-$1-$TAG.csv" -- sleep "$MEASURE_S" &
      perfs+=($!)
    done
  fi
fi
wait $pair
for p in "${perfs[@]}"; do wait "$p" || echo "::warning::perf stat exited with an error"; done
