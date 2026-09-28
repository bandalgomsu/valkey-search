#!/bin/bash
# pair.sh DIR DIM WARM MEAS CPUS_A CPUS_B TAG [EF=100]
# Runs A (parent) and B (candidate) side by side on one RDB and drives them with the paired client.
# Env: CLIENT_CPUS (default 4-7).
set -e
D=$1; DIM=$2; W=$3; M=$4; CA=$5; CB=$6; T=$7; EF=${8:-100}
CLIENT_CPUS=${CLIENT_CPUS:-4-7}
V=/s/valkey92/src; mkdir -p $D/pair
for x in "A 7001 $CA" "B 7002 $CB"; do set -- $x
  taskset -c $3 $V/valkey-server --port $2 --save "" --dir $D --dbfilename dump.rdb --daemonize yes \
    --logfile $D/pair/server-$1-$T.log --loadmodule /m/$1.so
done
# Wait up to 10 minutes per server for the RDB to load; fail with its log if it dies or stalls.
for x in "A 7001" "B 7002"; do set -- $x
  log=$D/pair/server-$1-$T.log; ok=
  for _ in $(seq 600); do
    if $V/valkey-cli -p $2 INFO persistence 2>/dev/null | grep -q "^loading:0"; then ok=1; break; fi
    # Match both the startup argv (--port N) and the process title set later (*:N).
    pgrep -f "valkey-server .*(--port |\*:)$2( |$)" >/dev/null || break
    sleep 1
  done
  [ -n "$ok" ] || { echo "::error::server $1 (port $2) failed to load $D/dump.rdb"; tail -50 $log; exit 1; }
  echo "$T: server $1 loaded, num_docs=$($V/valkey-cli -p $2 FT.INFO rd0 | grep -A1 -x num_docs | tail -1)"
done
echo "$T: A on cpus $CA, B on cpus $CB, client on $CLIENT_CPUS"
taskset -c $CLIENT_CPUS python3 /s/bench1m/pair.py 7001 7002 $DIM $W $M $D/pair/pair-$T.json $EF
for x in "A 7001" "B 7002"; do set -- $x
  pid=$(pgrep -f "valkey-server \*:$2")
  python3 /s/bench1m/thp.py $pid | tee $D/pair/thp-$1-$T.txt
  $V/valkey-cli -p $2 INFO memory | grep -E "^used_memory:|^used_memory_rss:" | tr -d '\r' | tee $D/pair/mem-$1-$T.txt
  $V/valkey-cli -p $2 SHUTDOWN NOSAVE >/dev/null 2>&1 || true
done
while pidof valkey-server >/dev/null; do sleep 0.5; done
