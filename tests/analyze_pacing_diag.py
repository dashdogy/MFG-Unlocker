#!/usr/bin/env python3
"""pacediag.1: summarise the flip queue's display-side diagnostics (source/native/xess/pacing_diag.h).

Reads the CSV chunks a game writes with OptiScaler.ini [XeFG] XeFGPacingDiagnostics = on
(<log name>.pacediag.<start>.g<generation>.<n>.csv) and joins, per queue generation:
  Q (each output the meter released: gate, index, ready, slot, release ...),
  P (each gated Present: gate -> the native chain's present id, how long the native Present held the thread),
  D (each displayed present: present id -> its sync time and refresh counts),
  A (AMD's flip meter read back), G (GPU engine running time).

For every multiplier (outputs per batch) it reports, per output index:
  release gaps     - our planned flips (release_ns) one after another;
  displayed gaps   - what the native chain reports reached the screen (sync time of consecutive present ids);
  flip delay       - displayed sync time minus our release (what happens after our gate);
  hold / late      - release minus readiness, and outputs ready after their slot;
plus presents never reported displayed, present-call durations, hand-over cadence, GPU engine busy shares and the AMD
meter states. Deterministic text, optional JSON (--json). --self-test checks it on synthetic data.

Usage: python -I tests/analyze_pacing_diag.py <csv files or folders> [--json out.json]
"""
import argparse
import csv
import glob
import json
import math
import os
import re
import sys
import tempfile

KINDS = {
    "Q": ["gate", "index", "outputs", "flags", "submit_ns", "ready_ns", "slot_ns", "signal_ns", "release_ns",
          "interval_ns", "pass_ns", "game_wait_ns", "game_slept_ns", "host_slept_ns", "idle_ns", "target_ns"],
    "P": ["gate", "present_id", "entered_ns", "returned_ns", "sync", "flags", "generated", "frame", "site", "thread",
          "result"],
    "D": ["present_count", "present_refresh", "sync_refresh", "sync_ns", "advanced", "result"],
    "A": ["mode", "count", "flags", "status", "output", "result", "applied", "applied_count"],
    "G": ["node", "global_running", "process_running", "wall_ns", "global_status", "process_status"],
}
FLAG_FIRST, FLAG_HELD, FLAG_LATE, FLAG_READY = 1 << 0, 1 << 1, 1 << 2, 1 << 10
NAME = re.compile(r"\.pacediag\.(?P<start>\d{8}-\d{6})\.g(?P<generation>\d+)\.(?P<chunk>\d+)\.csv$", re.I)


def quantile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * q
    low = int(math.floor(position))
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def summary_ms(values):
    """p5/p50/p95/max/sd in milliseconds of nanosecond values (None when empty)."""
    if not values:
        return {"n": 0}
    mean = sum(values) / len(values)
    sd = math.sqrt(sum((v - mean) ** 2 for v in values) / len(values))
    return {"n": len(values), "p5": round(quantile(values, 0.05) / 1e6, 3), "p50": round(quantile(values, 0.5) / 1e6, 3),
            "p95": round(quantile(values, 0.95) / 1e6, 3), "max": round(max(values) / 1e6, 3),
            "sd": round(sd / 1e6, 3)}


def read_rows(paths):
    """Rows per (start, generation), each kind a list of dicts, in file order (chunks ascending)."""
    files = []
    for path in paths:
        if os.path.isdir(path):
            files.extend(glob.glob(os.path.join(path, "*.pacediag.*.csv")))
        else:
            files.extend(glob.glob(path))
    keyed = []
    for name in sorted(set(files)):
        match = NAME.search(os.path.basename(name))
        if match:
            keyed.append(((match["start"], int(match["generation"])), int(match["chunk"]), name))
        else:
            keyed.append((("unknown", 0), 0, name))
    keyed.sort()
    queues = {}
    for key, _, name in keyed:
        rows = queues.setdefault(key, {kind: [] for kind in KINDS})
        with open(name, newline="", encoding="utf-8", errors="replace") as handle:
            for record in csv.reader(handle):
                if not record or record[0].startswith("#") or record[0] not in KINDS:
                    continue
                columns = KINDS[record[0]]
                try:
                    values = [int(x) for x in record[1:2 + len(columns)]]
                except ValueError:
                    continue
                if len(values) != 1 + len(columns):
                    continue
                row = {"t": values[0]}
                row.update(zip(columns, values[1:]))
                rows[record[0]].append(row)
    return queues


def analyse_queue(rows):
    q_rows = [r for r in rows["Q"] if r["outputs"] >= 2]
    by_gate = {r["gate"]: r for r in q_rows}
    present_of_gate = {}
    present_rows = {}
    for p in rows["P"]:
        if p["present_id"] >= 0:
            present_rows[p["present_id"]] = p
            if p["gate"] > 0:
                present_of_gate[p["gate"]] = p["present_id"]
    displayed = {}
    for d in rows["D"]:
        if d["present_count"] > 0 and d["present_count"] not in displayed:
            displayed[d["present_count"]] = d
    segments = {}
    previous = None
    for r in sorted(q_rows, key=lambda x: x["signal_ns"]):
        seg = segments.setdefault(r["outputs"], {
            "outputs": r["outputs"], "batches": 0, "handover": [], "release_gap": {}, "display_gap": {},
            "flip_delay": {}, "hold": {}, "late": {}, "count": {}, "undisplayed": 0, "joined": 0})
        index = r["index"]
        seg["count"][index] = seg["count"].get(index, 0) + 1
        if r["flags"] & FLAG_FIRST:
            seg["batches"] += 1
        if previous and previous["outputs"] == r["outputs"] and r["release_ns"] > previous["release_ns"]:
            gap = r["release_ns"] - previous["release_ns"]
            if gap < 250_000_000:
                seg["release_gap"].setdefault(index, []).append(gap)
        if r["ready_ns"] > 0 and r["release_ns"] >= r["ready_ns"]:
            seg["hold"].setdefault(index, []).append(r["release_ns"] - r["ready_ns"])
        if r["slot_ns"] > 0 and r["ready_ns"] > r["slot_ns"]:
            seg["late"][index] = seg["late"].get(index, 0) + 1
        present_id = present_of_gate.get(r["gate"])
        if present_id is not None:
            shown = displayed.get(present_id)
            if shown is None:
                if displayed and min(displayed) < present_id < max(displayed):
                    seg["undisplayed"] += 1
            else:
                seg["joined"] += 1
                if shown["sync_ns"] > 0:
                    seg["flip_delay"].setdefault(index, []).append(shown["sync_ns"] - r["release_ns"])
                before = displayed.get(present_id - 1)
                if before is not None and shown["sync_ns"] > before["sync_ns"] > 0:
                    seg["display_gap"].setdefault(index, []).append(shown["sync_ns"] - before["sync_ns"])
        previous = r
    firsts = sorted((r for r in q_rows if r["flags"] & FLAG_FIRST and r["submit_ns"] > 0), key=lambda x: x["submit_ns"])
    for a, b in zip(firsts, firsts[1:]):
        if a["outputs"] == b["outputs"] and 0 < b["submit_ns"] - a["submit_ns"] < 250_000_000:
            handover = b["submit_ns"] - a["submit_ns"]
            segments[b["outputs"]]["handover"].append(handover)
            # The game's own time per frame: the hand-over less what it was measured waiting on the pacing (its Present
            # or, on OptiScaler's route, the real frame's own hold), our low-latency sleep and its own measured sleep.
            waited = max(b["game_wait_ns"], 0) + max(b["game_slept_ns"], 0) + max(b["host_slept_ns"], 0)
            if b["game_wait_ns"] >= 0 or b["host_slept_ns"] >= 0:
                segments[b["outputs"]].setdefault("work", []).append(max(handover - waited, 0))
    result = []
    for outputs in sorted(segments):
        seg = segments[outputs]
        per_index = []
        for index in range(outputs):
            count = seg["count"].get(index, 0)
            per_index.append({
                "index": index, "outputs": count,
                "release_gap_ms": summary_ms(seg["release_gap"].get(index, [])),
                "displayed_gap_ms": summary_ms(seg["display_gap"].get(index, [])),
                "flip_delay_ms": summary_ms(seg["flip_delay"].get(index, [])),
                "hold_ms": summary_ms(seg["hold"].get(index, [])),
                "late_share": round(seg["late"].get(index, 0) / count, 4) if count else None,
            })
        result.append({"outputs": outputs, "batches": seg["batches"], "joined_presents": seg["joined"],
                       "undisplayed_presents": seg["undisplayed"], "handover_ms": summary_ms(seg["handover"]),
                       "game_work_ms": summary_ms(seg.get("work", [])), "per_index": per_index})
    present_hold = {}
    for p in rows["P"]:
        if p["returned_ns"] > p["entered_ns"] > 0:
            present_hold.setdefault(p["site"], []).append(p["returned_ns"] - p["entered_ns"])
    refresh_steps = {}
    ids = sorted(displayed)
    for a, b in zip(ids, ids[1:]):
        if b == a + 1:
            step = displayed[b]["present_refresh"] - displayed[a]["present_refresh"]
            refresh_steps[str(step)] = refresh_steps.get(str(step), 0) + 1
    gpu = {}
    for g in rows["G"]:
        node = gpu.setdefault(g["node"], {"global": 0, "process": 0, "wall": 0})
        node["global"] += max(g["global_running"], 0)
        node["process"] += max(g["process_running"], 0)
        node["wall"] += g["wall_ns"]
    gpu_nodes = []
    for node, sums in sorted(gpu.items()):
        if sums["wall"] > 0:
            gpu_nodes.append({"node": node, "global_share_100ns": round(sums["global"] * 100 / sums["wall"], 4),
                              "process_share_100ns": round(sums["process"] * 100 / sums["wall"], 4)})
    gpu_nodes.sort(key=lambda x: (-x["global_share_100ns"], x["node"]))
    amd = {}
    for a in rows["A"]:
        key = "mode=%d count=%d applied=%d/%d status=%d" % (a["mode"], a["count"], a["applied"], a["applied_count"],
                                                           a["status"])
        amd[key] = amd.get(key, 0) + 1
    return {"segments": result, "present_call_ms": {str(k): summary_ms(v) for k, v in sorted(present_hold.items())},
            "displayed_presents": len(displayed), "present_rows": len(rows["P"]), "queue_rows": len(q_rows),
            "refresh_steps": dict(sorted(refresh_steps.items(), key=lambda kv: int(kv[0]))),
            "gpu_nodes": gpu_nodes[:6], "amd_meter": dict(sorted(amd.items()))}


def analyse(paths):
    queues = read_rows(paths)
    return {"queues": [{"start": key[0], "generation": key[1], **analyse_queue(rows)} for key, rows in sorted(queues.items())]}


def fmt(s):
    if not s or not s.get("n"):
        return "-"
    return "p50 %.3f p95 %.3f max %.3f sd %.3f (n %d)" % (s["p50"], s["p95"], s["max"], s["sd"], s["n"])


def text(report):
    lines = []
    for queue in report["queues"]:
        lines.append("queue %s g%d: %d outputs, %d gated presents, %d displayed" % (
            queue["start"], queue["generation"], queue["queue_rows"], queue["present_rows"], queue["displayed_presents"]))
        for seg in queue["segments"]:
            lines.append("  %dx: %d batches, hand-over %s, joined %d, undisplayed %d" % (
                seg["outputs"], seg["batches"], fmt(seg["handover_ms"]), seg["joined_presents"],
                seg["undisplayed_presents"]))
            lines.append("      game work per frame %s" % fmt(seg["game_work_ms"]))
            for item in seg["per_index"]:
                lines.append("    index %d: release gap %s" % (item["index"], fmt(item["release_gap_ms"])))
                lines.append("             displayed gap %s" % fmt(item["displayed_gap_ms"]))
                lines.append("             flip delay %s" % fmt(item["flip_delay_ms"]))
                lines.append("             hold %s, late %s" % (fmt(item["hold_ms"]), item["late_share"]))
        lines.append("  present call by site (0 first, 1 extra, 2 real): " + "; ".join(
            "%s %s" % (k, fmt(v)) for k, v in queue["present_call_ms"].items()))
        lines.append("  refresh steps between consecutive displayed presents: %s" % queue["refresh_steps"])
        lines.append("  gpu nodes (share assuming 100 ns units): %s" % queue["gpu_nodes"])
        lines.append("  amd meter read back: %s" % queue["amd_meter"])
    return "\n".join(lines)


def self_test():
    """Synthetic 2x queue: even releases 5 ms apart, the real frame (index 1) displayed 2 ms after release, the
    generated one (index 0) 0.5 ms after, one present never displayed."""
    folder = tempfile.mkdtemp(prefix="pacediag-selftest-")
    path = os.path.join(folder, "rtxmfg-game-0000000000000000.pacediag.20261108-020000.g1.1.csv")
    rows = []
    t0 = 1_000_000_000
    present = 100
    for batch in range(200):
        for index in range(2):
            gate = batch * 2 + index + 1
            release = t0 + gate * 5_000_000
            ready = release - 1_000_000
            submit = t0 + batch * 10_000_000 if index == 0 else 0
            flags = (FLAG_FIRST if index == 0 else 0) | FLAG_READY | FLAG_HELD
            wait = 3_000_000 if index == 0 else -1
            rows.append(["Q", release, gate, index, 2, flags, submit, ready, release, release, release, 5_000_000, -1,
                         wait, 0, -1, 0, 0])
            present += 1
            rows.append(["P", release - 4_000_000, gate, present, release - 4_100_000, release - 4_000_000, 0, 512, 1,
                         batch, index * 2, 1234, 0])
            if present == 150:
                continue  # never displayed
            delay = 500_000 if index == 0 else 2_000_000
            rows.append(["D", release + delay + 100_000, present, 1000 + present, 1000 + present, release + delay, 1, 0])
    rows.append(["G", t0, 0, 50_000, 40_000, 100_000_000, 0, 0])
    rows.append(["A", t0, 2, 1, 5, 0, 0, 0, 2, 1])
    with open(path, "w", newline="", encoding="utf-8") as handle:
        handle.write("# pacediag.1 generation=1 chunk=1 dropped_before=0\n")
        writer = csv.writer(handle)
        writer.writerows(rows)
    report = analyse([folder])
    os.remove(path)
    os.rmdir(folder)
    queue = report["queues"][0]
    seg = queue["segments"][0]
    checks = [
        (seg["outputs"] == 2, "one 2x segment"),
        (seg["batches"] == 200, "200 batches"),
        (seg["undisplayed_presents"] == 1, "one undisplayed present"),
        (seg["per_index"][0]["release_gap_ms"]["p50"] == 5.0, "index 0 release gap 5 ms"),
        (seg["per_index"][1]["flip_delay_ms"]["p50"] == 2.0, "index 1 flip delay 2 ms"),
        (seg["per_index"][0]["flip_delay_ms"]["p50"] == 0.5, "index 0 flip delay 0.5 ms"),
        (seg["per_index"][1]["displayed_gap_ms"]["p50"] == 6.5, "index 1 displayed 6.5 ms after index 0"),
        (seg["per_index"][0]["displayed_gap_ms"]["p50"] == 3.5, "index 0 displayed 3.5 ms after the real frame"),
        (seg["per_index"][1]["hold_ms"]["p50"] == 1.0, "hold 1 ms"),
        (seg["handover_ms"]["p50"] == 10.0, "hand-over 10 ms"),
        (seg["game_work_ms"]["p50"] == 7.0, "game work 10 - 3 ms"),
        (queue["gpu_nodes"][0]["global_share_100ns"] == 0.05, "gpu share"),
        (queue["amd_meter"] == {"mode=2 count=1 applied=2/1 status=0": 1}, "amd row"),
        (queue["refresh_steps"].get("1", 0) > 0, "refresh steps"),
    ]
    failed = [name for ok, name in checks if not ok]
    json.dumps(report, allow_nan=False)
    print("self-test: %d checks, %d failed%s" % (len(checks), len(failed), (": " + ", ".join(failed)) if failed else ""))
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="*")
    parser.add_argument("--json")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.paths:
        parser.error("give the pacediag CSV files or their folder")
    report = analyse(args.paths)
    print(text(report))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=1, sort_keys=True, allow_nan=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
