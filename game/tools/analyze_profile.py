"""Summarize Saida Chrome traces without adding nested CPU/GPU scopes together."""
import argparse
import collections
import json
from pathlib import Path
import statistics


def distribution(values):
    values = sorted(values)
    if not values:
        return {}
    return {"samples": len(values), "mean_ms": statistics.mean(values),
            "median_ms": statistics.median(values),
            "p95_ms": values[min(len(values) - 1, int((len(values) - 1) * .95))],
            "max_ms": values[-1]}


def analyze(path, tail=None, skip_last=0):
    events = json.loads(path.read_text(encoding="utf-8"))["traceEvents"]
    frames = [e for e in events if e.get("cat") == "Frame"]
    if skip_last:
        frames = frames[:-skip_last]
    if tail:
        frames = frames[-tail:]
    start = frames[0]["ts"] if frames else 0
    end = frames[-1]["ts"] + frames[-1]["dur"] if frames else 0
    events = [e for e in events if start <= e.get("ts", -1) < end]
    scopes = collections.defaultdict(list)
    counters = collections.defaultdict(list)
    for e in events:
        if e.get("ph") == "X" and e.get("cat") in ("CPU", "GPU"):
            scopes[(e["cat"], e["name"])].append(e["dur"] / 1000)
        elif e.get("ph") == "C":
            counters[e["name"]].append(next(iter(e["args"].values())))
    rows = [{"category": cat, "name": name,
             "ms_per_frame": sum(values) / max(1, len(frames)),
             **distribution(values)} for (cat, name), values in scopes.items()]
    return {"trace": str(path), "frames": len(frames),
            "frame": distribution([e["dur"] / 1000 for e in frames]),
            "over_33_33_ms": sum(e["dur"] > 33333.333 for e in frames),
            "scopes": sorted(rows, key=lambda r: -r["ms_per_frame"]),
            "counters": {name: {"min": min(v), "max": max(v), "last": v[-1]}
                         for name, v in counters.items()}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--tail", type=int, help="Only the last N frames")
    parser.add_argument("--skip-last", type=int, default=0, help="Exclude final capture/readback frames")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.trace, args.tail, args.skip_last)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k not in ("scopes", "counters")}, indent=2))
    for category in ("CPU", "GPU"):
        print(category)
        for row in [r for r in result["scopes"] if r["category"] == category][:25]:
            print(f"{row['ms_per_frame']:8.3f} ms/frame  p95/call {row['p95_ms']:8.3f}  {row['name']}")
    for name, values in result["counters"].items():
        if any(key in name for key in ("Nodes", "Triangles", "Instances", "Lod", "Physics", "Indexed")):
            print(name, values)
