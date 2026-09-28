# summarize.py DIR TITLE -> Markdown table of parent (A) vs candidate (B) from pair-*.json runs
import glob, json, os, re, sys

d, title = sys.argv[1], sys.argv[2]
paths = sorted(glob.glob(f"{d}/pair/pair-*.json"))
runs = [json.load(open(f)) for f in paths]
tags = [os.path.basename(f)[len("pair-"):-len(".json")] for f in paths]
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


def perf_per_query(label):
    """Average of per-run (event count / FT.SEARCH calls) from perf stat -x, output."""
    out = {}
    for run, tag in zip(runs, tags):
        f = f"{d}/pair/perf-{label}-{tag}.csv"
        if not os.path.exists(f):
            continue
        calls = run["server"][label]["calls"]
        for line in open(f):
            parts = line.strip().split(",")
            if line.startswith("#") or len(parts) < 3:
                continue
            try:
                value = float(parts[0])
            except ValueError:  # <not counted> / <not supported>
                continue
            out.setdefault(parts[2], []).append(value / calls)
    return {k: sum(v) / len(v) for k, v in out.items()}


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
perf_a, perf_b = perf_per_query("A"), perf_per_query("B")
for ev in [e for e in perf_a if e in perf_b]:
    a, b = perf_a[ev], perf_b[ev]
    rows.append((f"`{ev}` per query", f"{a:,.0f}", f"{b:,.0f}", change(a, b) if a else "-"))
if all(e in perf_a and e in perf_b for e in ("dTLB-loads", "dTLB-load-misses")) and perf_a["dTLB-loads"] and perf_b["dTLB-loads"]:
    a = 100 * perf_a["dTLB-load-misses"] / perf_a["dTLB-loads"]
    b = 100 * perf_b["dTLB-load-misses"] / perf_b["dTLB-loads"]
    rows.append(("dTLB load miss rate", f"{a:.3f}%", f"{b:.3f}%", change(a, b) if a else "-"))
if not perf_a:
    rows.append(("TLB counters (perf)", "n/a", "n/a", "-"))

print(f"#### {title}\n")
print(f"Average of {len(runs)} runs (CPU sets swapped between runs), {runs[0]['pairs']:,}+ query pairs per run.\n")
print("| Workload / metric | Parent | Candidate | Change |")
print("|---|---:|---:|---:|")
for r in rows:
    print("| " + " | ".join(r) + " |")
print()
