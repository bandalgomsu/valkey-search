# Usage: gen.py N DIM  -> RESP stream: FT.CREATE + N HSETs (random FLOAT32 vectors)
import sys, struct, random
n, dim = int(sys.argv[1]), int(sys.argv[2])
out = sys.stdout.buffer
def cmd(*a):
    b = [x if isinstance(x, bytes) else str(x).encode() for x in a]
    out.write(b"*%d\r\n" % len(b))
    for x in b: out.write(b"$%d\r\n%s\r\n" % (len(x), x))
cmd("FT.CREATE","rd0","ON","HASH","PREFIX","1","rd0-","SCHEMA","embedding","VECTOR","HNSW","10",
    "TYPE","FLOAT32","DIM",dim,"DISTANCE_METRIC","L2","M","16","EF_CONSTRUCTION","200")
r = random.Random(1); fmt = "%df" % dim
for i in range(n):
    cmd("HSET", "rd0-%d" % i, "embedding", struct.pack(fmt, *[r.random() for _ in range(dim)]))
