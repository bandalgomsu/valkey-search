#!/bin/bash
# pair.sh DIR DIM WARM MEAS CPUS_A CPUS_B TAG [EF=100]
# Runs server A and server B side by side on one RDB and drives them with the paired client.
# Env (all optional):
#   CLIENT_CPUS   client CPU set (default 4-7)
#   MOD_A, MOD_B  module under /m/ for each server (default A = parent, B = candidate)
#   ARGS_A, ARGS_B extra valkey-server arguments for each server
#   OUT_SUBDIR    results directory under DIR (default pair)
set -e
D=$1; DIM=$2; W=$3; M=$4; CA=$5; CB=$6; T=$7; EF=${8:-100}
CLIENT_CPUS=${CLIENT_CPUS:-4-7}
MOD_A=${MOD_A:-A}; MOD_B=${MOD_B:-B}
P=$D/${OUT_SUBDIR:-pair}
V=/s/valkey92/src; mkdir -p $P
for x in "A 7001 $CA $MOD_A" "B 7002 $CB $MOD_B"; do set -- $x
  extra_var=ARGS_$1
  taskset -c $3 $V/valkey-server --port $2 --save "" --dir $D --dbfilename dump.rdb --daemonize yes \
    --logfile $P/server-$1-$T.log --loadmodule /m/$4.so ${!extra_var}
done
# Wait up to 10 minutes per server for the RDB to load; fail with its log if it dies or stalls.
for x in "A 7001" "B 7002"; do set -- $x
  log=$P/server-$1-$T.log; ok=
  for _ in $(seq 600); do
    if $V/valkey-cli -p $2 INFO persistence 2>/dev/null | grep -q "^loading:0"; then ok=1; break; fi
    # Match both the startup argv (--port N) and the process title set later (*:N).
    pgrep -f "valkey-server .*(--port |\*:)$2( |$)" >/dev/null || break
    sleep 1
  done
  [ -n "$ok" ] || { echo "::error::server $1 (port $2) failed to load $D/dump.rdb"; tail -50 $log; exit 1; }
  echo "$T: server $1 loaded, num_docs=$($V/valkey-cli -p $2 FT.INFO rd0 | grep -A1 -x num_docs | tail -1)"
done
echo "$T: A ($MOD_A ${ARGS_A:-}) on cpus $CA, B ($MOD_B ${ARGS_B:-}) on cpus $CB, client on $CLIENT_CPUS"
taskset -c $CLIENT_CPUS python3 /s/bench1m/pair.py 7001 7002 $DIM $W $M $P/pair-$T.json $EF
for x in "A 7001" "B 7002"; do set -- $x
  pid=$(pgrep -f "valkey-server \*:$2")
  python3 /s/bench1m/thp.py $pid | tee $P/thp-$1-$T.txt
  $V/valkey-cli -p $2 INFO memory | grep -E "^used_memory:|^used_memory_rss:" | tr -d '\r' | tee $P/mem-$1-$T.txt
  $V/valkey-cli -p $2 SHUTDOWN NOSAVE >/dev/null 2>&1 || true
done
while pidof valkey-server >/dev/null; do sleep 0.5; done
