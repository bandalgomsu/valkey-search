#!/bin/bash
# phase.sh build N DIM DIR -> build index with module B, SAVE to DIR/dump.rdb
# phase.sh verify N DIR    -> load DIR/dump.rdb with module B; exit 0 only if it holds N indexed docs
set -e
V=/s/valkey92/src; CLI="$V/valkey-cli -p 7000"
start() { taskset -c 0-3 $V/valkey-server --port 7000 --save "" --dir $3 --dbfilename dump.rdb \
            --daemonize yes --logfile $3/server-$1.log --loadmodule /m/$2.so; }
waitup() { until $CLI PING 2>/dev/null | grep -q PONG; do sleep 1; done; }
ftinfo() { $CLI FT.INFO rd0 | grep -A1 -x "$1" | tail -1; }
stop() { $CLI SHUTDOWN NOSAVE >/dev/null 2>&1 || true; while pidof valkey-server >/dev/null; do sleep 0.5; done; }
case $1 in
build)
  N=$2; DIM=$3; D=$4; mkdir -p $D; rm -f $D/dump.rdb; stop
  start build B $D; waitup; t0=$(date +%s)
  python3 /s/bench1m/gen.py $N $DIM | $CLI --pipe | tail -1
  until [ "$(ftinfo num_docs)" = "$N" ]; do sleep 5; done
  echo "index built in $(( $(date +%s) - t0 ))s"; $CLI SAVE; stop; ls -la $D/dump.rdb ;;
verify)
  N=$2; D=$3; stop
  start verify B $D
  # A server that fails to load the RDB exits instead of answering PING.
  for _ in $(seq 600); do $CLI PING 2>/dev/null | grep -q PONG && break; pidof valkey-server >/dev/null || break; sleep 1; done
  pidof valkey-server >/dev/null || { echo "server exited while loading $D/dump.rdb"; exit 1; }
  until $CLI INFO persistence | grep -q "loading:0"; do sleep 1; done
  got=$(ftinfo num_docs); stop
  echo "RDB holds num_docs=$got (expected $N)"; [ "$got" = "$N" ] ;;
esac
