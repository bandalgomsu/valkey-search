#!/bin/bash
# pair.sh DIR DIM WARM MEAS CPUS_A CPUS_B TAG [EF=100]
# Runs A (parent) and B (candidate) side by side on one RDB and drives them with the paired client.
# Env: CLIENT_CPUS (default 4-7).
D=$1; DIM=$2; W=$3; M=$4; CA=$5; CB=$6; T=$7; EF=${8:-100}
CLIENT_CPUS=${CLIENT_CPUS:-4-7}
V=/s/valkey92/src; mkdir -p $D/pair
for x in "A 7001 $CA" "B 7002 $CB"; do set -- $x
  taskset -c $3 $V/valkey-server --port $2 --save "" --dir $D --dbfilename dump.rdb --daemonize yes \
    --logfile $D/pair/server-$1-$T.log --loadmodule /m/$1.so
done
for p in 7001 7002; do
  until $V/valkey-cli -p $p PING 2>/dev/null | grep -q PONG; do sleep 1; done
  until $V/valkey-cli -p $p INFO persistence | grep -q "loading:0"; do sleep 1; done
done
echo "$T: A on cpus $CA, B on cpus $CB, client on $CLIENT_CPUS, num_docs A=$($V/valkey-cli -p 7001 FT.INFO rd0 | grep -A1 -x num_docs | tail -1) B=$($V/valkey-cli -p 7002 FT.INFO rd0 | grep -A1 -x num_docs | tail -1)"
taskset -c $CLIENT_CPUS python3 /s/bench1m/pair.py 7001 7002 $DIM $W $M $D/pair/pair-$T.json $EF
for x in "A 7001" "B 7002"; do set -- $x
  pid=$(pgrep -f "valkey-server \*:$2")
  python3 /s/bench1m/thp.py $pid | tee $D/pair/thp-$1-$T.txt
  $V/valkey-cli -p $2 INFO memory | grep -E "^used_memory:|^used_memory_rss:" | tr -d '\r' | tee $D/pair/mem-$1-$T.txt
  $V/valkey-cli -p $2 SHUTDOWN NOSAVE >/dev/null 2>&1
done
while pidof valkey-server >/dev/null; do sleep 0.5; done
