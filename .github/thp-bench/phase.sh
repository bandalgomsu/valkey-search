#!/bin/bash
# phase.sh build N DIM DIR -> build index with module B, SAVE to DIR/dump.rdb
# phase.sh verify N DIR    -> load DIR/dump.rdb with module B; exit 0 only if it holds N indexed docs
set -e
V=/s/valkey92/src; CLI="$V/valkey-cli -p 7000"
start() { taskset -c 0-3 $V/valkey-server --port 7000 --save "" --dir $3 --dbfilename dump.rdb \
            --daemonize yes --logfile $3/server-$1.log --loadmodule /m/$2.so; }
# Wait up to 10 minutes for PONG; fail with the server log if the server dies or never answers.
waitup() {
  for _ in $(seq 600); do
    $CLI PING 2>/dev/null | grep -q PONG && return 0
    pidof valkey-server >/dev/null || { echo "::error::valkey-server exited during startup"; tail -50 "$1"; return 1; }
    sleep 1
  done
  echo "::error::valkey-server did not answer PING within 600s"; tail -50 "$1"; return 1
}
ftinfo() { $CLI FT.INFO rd0 | grep -A1 -x "$1" | tail -1; }
stop() { $CLI SHUTDOWN NOSAVE >/dev/null 2>&1 || true; while pidof valkey-server >/dev/null; do sleep 0.5; done; }
progress() {
  local t0=$1
  while sleep 60; do
    echo "[$(( ($(date +%s) - t0) / 60 ))m] dbsize=$($CLI DBSIZE 2>/dev/null) num_docs=$(ftinfo num_docs 2>/dev/null)"
    pidof valkey-server >/dev/null || { echo "::error::valkey-server exited while loading"; return 1; }
  done
}
case $1 in
build)
  N=$2; DIM=$3; D=$4; LOG=$D/server-build.log; mkdir -p $D; rm -f $D/dump.rdb; stop
  start build B $D; waitup $LOG; t0=$(date +%s)
  progress $t0 & PROGRESS=$!
  trap 'kill $PROGRESS 2>/dev/null || true' EXIT
  python3 /s/bench1m/gen.py $N $DIM | $CLI --pipe | tail -1
  until [ "$(ftinfo num_docs)" = "$N" ]; do
    pidof valkey-server >/dev/null || { echo "::error::valkey-server exited while indexing"; tail -50 $LOG; exit 1; }
    sleep 5
  done
  echo "index built in $(( $(date +%s) - t0 ))s"; $CLI SAVE; stop; ls -la $D/dump.rdb ;;
verify)
  N=$2; D=$3; LOG=$D/server-verify.log; stop
  start verify B $D; waitup $LOG
  until $CLI INFO persistence | grep -q "loading:0"; do
    pidof valkey-server >/dev/null || { echo "::error::valkey-server exited while loading the RDB"; tail -50 $LOG; exit 1; }
    sleep 1
  done
  got=$(ftinfo num_docs); stop
  echo "RDB holds num_docs=$got (expected $N)"; [ "$got" = "$N" ] ;;
esac
