# Usage: pair.py PORT_A PORT_B DIM WARMUP_S MEASURE_S OUT_JSON [EF_RUNTIME=100]
# One client sends the same query to A and B back to back, flipping the order each iteration,
# so host noise and drift hit both builds equally. Reports per-build percentiles and paired B/A ratios.
import sys, socket, struct, random, time, json, statistics, re
pa, pb, dim, warm, meas, out = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5]), sys.argv[6]
ef = int(sys.argv[7]) if len(sys.argv) > 7 else 100
def enc(*a):
    b = [x if isinstance(x, bytes) else str(x).encode() for x in a]
    return b"*%d\r\n" % len(b) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in b)
r = random.Random(2); fmt = "%df" % dim
qs = [enc("FT.SEARCH","rd0","*=>[KNN 10 @embedding $vec EF_RUNTIME %d]" % ef,"NOCONTENT",
          "PARAMS","2","vec",struct.pack(fmt,*[r.random() for _ in range(dim)]),"DIALECT","2") for _ in range(1000)]
class Conn:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port)); self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.f = self.s.makefile("rb")
    def read(self):
        l = self.f.readline(); t = l[:1]
        if t == b"*":
            return [self.read() for _ in range(int(l[1:]))]
        elif t == b"$":
            n = int(l[1:])
            return self.f.read(n + 2)[:-2] if n >= 0 else None
        elif t == b"-": raise RuntimeError(l)
        return l[1:].strip()
    def cmd(self, *a): self.s.sendall(enc(*a)); return self.read()
    def q(self, m):
        t0 = time.perf_counter_ns(); self.s.sendall(m); self.read(); return time.perf_counter_ns() - t0
A, B = Conn(pa), Conn(pb)
def run(dur, rec):
    i = 0; end = time.perf_counter() + dur
    while time.perf_counter() < end:
        m = qs[i % 1000]
        if i % 2 == 0: a = A.q(m); b = B.q(m)
        else: b = B.q(m); a = A.q(m)
        if rec is not None: rec.append((a, b))
        i += 1
run(warm, None)
for c in (A, B): c.cmd("CONFIG", "RESETSTAT")
open(out + ".measuring", "w").close()  # lets an external profiler start with the measured window
pairs = []; run(meas, pairs)
def pct(v, q): return v[min(len(v)-1, int(q*len(v)))]
def stats(v):
    v = sorted(v)
    return {"mean_ms": sum(v)/len(v)/1e6, **{k: pct(v, q)/1e6 for k, q in
            (("p50_ms",.5),("p90_ms",.9),("p99_ms",.99),("p999_ms",.999))}}
ratios = sorted(b/a for a, b in pairs)
res = {"pairs": len(pairs), "A": stats([a for a, _ in pairs]), "B": stats([b for _, b in pairs]),
       "paired_B_over_A": {"median": statistics.median(ratios), "p10": pct(ratios,.1), "p90": pct(ratios,.9),
                           "B_faster_frac": sum(1 for x in ratios if x < 1)/len(ratios)}}
def server_stats(c):
    info = c.cmd("INFO", "latencystats").decode() + c.cmd("INFO", "commandstats").decode()
    lat = re.search(r"latency_percentiles_usec_ft\.search:(\S+)", info, re.I)
    cmd = re.search(r"cmdstat_ft\.search:calls=(\d+),usec=\d+,usec_per_call=([\d.]+)", info, re.I)
    d = dict(kv.split("=") for kv in lat.group(1).split(",")) if lat else {}
    return {"calls": int(cmd.group(1)), "usec_per_call": float(cmd.group(2)), **{k + "_us": float(v) for k, v in d.items()}}
res["server"] = {"A": server_stats(A), "B": server_stats(B)}; res["ef_runtime"] = ef
json.dump(res, open(out, "w"), indent=1); print(json.dumps(res))
