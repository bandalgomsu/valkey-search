# summarize.py DIR TITLE -> Markdown table of parent (A) vs candidate (B) from pair-*.json runs
import glob, json, os, re, sys

d, title = sys.argv[1], sys.argv[2]
runs = [json.load(open(f)) for f in sorted(glob.glob(f"{d}/pair/pair-*.json"))]
if not runs:
    sys.exit(f"no results in {d}/pair")


def avg(fn):
    return sum(fn(r) for r in runs) / len(runs)


def change(a, b):
    return f"{100 * (b - a) / a:+.2f}%"


def read_kv(pattern):
    out = {}
    for f in sorted(glob.glob(f"{d}/pair/{pattern}")):
        for line in open(f):
            k, _, v = line.strip().partition(":")
            if v:
                out.setdefault(k, []).append(int(v))
    return {k: sum(v) / len(v) for k, v in out.items()}


def ahp(label):
    vals = []
    for f in glob.glob(f"{d}/pair/thp-{label}-*.txt"):
        m = re.search(r"'rollup_AnonHugePages_kB': (\d+)", open(f).read())
        if m:
            vals.append(int(m.group(1)))
    return max(vals) if vals else 0


rows = []
for key, name in (("mean_ms", "average"), ("p50_ms", "p50"), ("p90_ms", "p90"), ("p99_ms", "p99")):
    a, b = avg(lambda r: r["A"][key]), avg(lambda r: r["B"][key])
    rows.append((f"FT.SEARCH {name} latency", f"{a:.3f} ms", f"{b:.3f} ms", change(a, b)))
a, b = avg(lambda r: r["server"]["A"]["usec_per_call"]), avg(lambda r: r["server"]["B"]["usec_per_call"])
rows.append(("FT.SEARCH server time per call", f"{a:.1f} µs", f"{b:.1f} µs", change(a, b)))
for key, name in (("p50_us", "p50"), ("p99_us", "p99")):
    a, b = avg(lambda r: r["server"]["A"][key]), avg(lambda r: r["server"]["B"][key])
    rows.append((f"FT.SEARCH server {name} latency", f"{a:.0f} µs", f"{b:.0f} µs", change(a, b)))
faster = " / ".join(f"{100 * r['paired_B_over_A']['B_faster_frac']:.1f}%" for r in runs)
rows.append(("Queries where candidate was faster", "-", faster, "-"))
mem_a, mem_b = read_kv("mem-A-*.txt"), read_kv("mem-B-*.txt")
for key, name in (("used_memory", "`used_memory`"), ("used_memory_rss", "Server RSS")):
    if key in mem_a and key in mem_b:
        a, b = mem_a[key] / 2**20, mem_b[key] / 2**20
        rows.append((f"{name} after measurement", f"{a:.2f} MiB", f"{b:.2f} MiB", change(a, b)))
rows.append(("`AnonHugePages`", f"{ahp('A'):,} kB", f"{ahp('B'):,} kB", "-"))

print(f"#### {title}\n")
print(f"Average of {len(runs)} runs (CPU sets swapped between runs), {runs[0]['pairs']:,}+ query pairs per run.\n")
print("| Workload / metric | Parent | Candidate | Change |")
print("|---|---:|---:|---:|")
for r in rows:
    print("| " + " | ".join(r) + " |")
print()
