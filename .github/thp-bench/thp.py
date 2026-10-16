# Usage: thp.py PID -> hugepage-advised mapping totals + rollup AnonHugePages
import sys, re
pid = sys.argv[1]; cur = None; hg = {"n":0,"size":0,"rss":0,"ahp":0}
for line in open(f"/proc/{pid}/smaps"):
    m = re.match(r"^([0-9a-f]+)-([0-9a-f]+) ", line)
    if m: cur = {"size": int(m[2],16)-int(m[1],16)}; continue
    k, _, v = line.partition(":")
    if k in ("Rss","AnonHugePages"): cur[k] = int(v.split()[0])
    if k == "VmFlags" and "hg" in v.split():
        hg["n"]+=1; hg["size"]+=cur["size"]>>10; hg["rss"]+=cur["Rss"]; hg["ahp"]+=cur["AnonHugePages"]
roll = [l.split()[1] for l in open(f"/proc/{pid}/smaps_rollup") if l.startswith("AnonHugePages")][0]
print({"hg_mappings": hg, "rollup_AnonHugePages_kB": int(roll)})
