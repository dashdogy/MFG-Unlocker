#!/usr/bin/env python3
"""latmon.1: PC latency per real frame from the latency probe's CSV (source/native/latency_probe.h).

PC latency here: from the game's simulation-start marker of a real frame to the moment the first output carrying that
frame was flipped on screen (latency_first), and to the real frame's own flip (latency_real). Parts:
  game     simulation start -> the game's Present (frame generation's hand-over)
  fg       hand-over -> the first output's Present on the output layer
  display  that output Present -> its flip (DXGI frame statistics: the vblank the present count was shown at)

Batches: XeFG labels each native Present with its frame (xefg_output); DLSS-G's outputs between one hand-over and the
next form that frame's batch (--dlssg-lag shifts them by whole batches). A flip time is exact when the statistics
sample's SyncRefreshCount equals the PresentRefreshCount it reports, inferred (from the refresh period) otherwise; only
exact flips are used unless --allow-inferred.

PresentMon (optional, --presentmon): run it on the game's process with QPC times, for example
  PresentMon-2.6.0-x64.exe --process_id <pid> --output_file pm.csv --v1_metrics --qpc_time --track_frame_type
and the analyzer matches each output Present to PresentMon's row by QPC and compares the flip times. Column names are
detected (printed in the result); --pm-time-column / --pm-display-column override them.

Usage:
  python analyze_latency_probe.py <probe.csv> [--presentmon pm.csv] [--frames out.csv] [--json out.json]
  python analyze_latency_probe.py --self-test
"""
from __future__ import annotations

import argparse
import bisect
import csv
import io
import json
import math
import statistics
import sys
from collections import Counter, defaultdict, deque
from dataclasses import dataclass, field

# latency_probe_ring.h
ROLE_STREAMLINE_BASE, ROLE_XEFG_OUTPUT, ROLE_XEMFG_HOST, ROLE_PROVIDER, ROLE_D3D11, ROLE_NESTED = 1, 2, 4, 8, 16, 32
STATS_COUNT, STATS_FRAME, STATS_DISJOINT = 1, 2, 4
MARKER_SIM_START, MARKER_PRESENT_START = 0, 4
MARKER_INPUT = (6, 13)  # legacy input sample, controller input sample
MARKER_NAMES = {0: "simulation_start", 1: "simulation_end", 2: "render_submit_start", 3: "render_submit_end",
                4: "present_start", 5: "present_end", 6: "input_sample", 7: "trigger_flash", 8: "pc_latency_ping",
                13: "controller_input_sample", 22: "num_presents_in_batch"}


@dataclass
class Event:
    kind: str
    qpc: int
    thread: int
    sub: int
    a: int
    b: int
    c: int
    d: int
    index: int


@dataclass
class Output:
    event: Event
    chain: int
    role: int
    count: int | None = None
    flip: int | None = None
    flip_exact: bool = False
    xefg_frame: int | None = None
    generated: bool | None = None


@dataclass
class Probe:
    frequency: int
    pid: int | None
    open_qpc: int | None
    open_filetime: int | None
    events: list[Event] = field(default_factory=list)


def read_probe(text: str) -> Probe:
    meta: dict[str, int] = {}
    rows = []
    for line in text.splitlines():
        if line.startswith("#"):
            for token in line[1:].split():
                if "=" in token:
                    key, _, value = token.partition("=")
                    if value.lstrip("-").isdigit():
                        meta[key] = int(value)
            continue
        if line.strip():
            rows.append(line)
    if "qpc_frequency" not in meta or meta["qpc_frequency"] <= 0:
        raise ValueError("no qpc_frequency in the probe header")
    reader = csv.DictReader(io.StringIO("\n".join(rows)))
    probe = Probe(meta["qpc_frequency"], meta.get("pid"), meta.get("open_qpc"), meta.get("open_filetime"))
    for index, row in enumerate(reader):
        probe.events.append(Event(row["kind"], int(row["qpc"]), int(row["thread"]), int(row["sub"]), int(row["a"]),
                                  int(row["b"]), int(row["c"]), int(row["d"]), index))
    return probe


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    rank = fraction * (len(ordered) - 1)
    low = math.floor(rank)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def summary(values: list[float]) -> dict:
    finite = [v for v in values if math.isfinite(v)]
    if not finite:
        return {"count": 0}
    return {"count": len(finite), "mean_ms": round(statistics.fmean(finite), 4),
            "median_ms": round(percentile(finite, 0.5), 4), "p1_ms": round(percentile(finite, 0.01), 4),
            "p99_ms": round(percentile(finite, 0.99), 4), "min_ms": round(min(finite), 4),
            "max_ms": round(max(finite), 4)}


def flip_times(outputs: list[Output], stats_by_output: dict[int, Event], frequency: int = 10_000_000) -> dict:
    """Fills each output's present count and flip time; returns the statistics coverage."""
    samples: dict[int, list[tuple[int, int, int, int]]] = defaultdict(list)
    coverage = Counter()
    for output in outputs:
        stats = stats_by_output.get(output.event.index)
        if stats is None:
            coverage["no_stats_event"] += 1
            continue
        if stats.sub & STATS_COUNT:
            output.count = stats.a >> 32
        if stats.sub & STATS_FRAME:
            coverage["frame_statistics"] += 1
            samples[output.chain].append((stats.a & 0xFFFFFFFF, stats.b, stats.c, stats.d))
        elif stats.sub & STATS_DISJOINT:
            coverage["disjoint"] += 1
        else:
            coverage["failed"] += 1
    # The refresh period from exact samples (SyncRefreshCount == PresentRefreshCount) of each chain.
    periods: list[float] = []
    for chain_samples in samples.values():
        exact = sorted({(c, d) for (_p, b, c, d) in chain_samples if b == c and d > 0})
        for (c0, d0), (c1, d1) in zip(exact, exact[1:]):
            if c1 > c0:
                periods.append((d1 - d0) / (c1 - c0))
    period = percentile(periods, 0.5) if periods else float("nan")
    flips: dict[tuple[int, int], tuple[int, bool]] = {}
    by_count: dict[tuple[int, int], Output] = {}
    for output in outputs:
        if output.count is not None:
            by_count[(output.chain, output.count)] = output
    for chain, chain_samples in samples.items():
        for present, refresh, sync_refresh, sync_qpc in chain_samples:
            if present == 0 or sync_qpc <= 0:
                continue
            key = (chain, present)
            previous = flips.get(key)
            if refresh == sync_refresh:
                coverage["exact_samples"] += 1
                if previous and previous[1] and previous[0] != sync_qpc:
                    coverage["exact_disagreements"] += 1
                if not previous or not previous[1]:
                    flips[key] = (sync_qpc, True)
            elif sync_refresh > refresh:
                coverage["inferred_samples"] += 1
                if previous is None and math.isfinite(period):
                    flips[key] = (sync_qpc - int(round((sync_refresh - refresh) * period)), False)
            else:
                coverage["refresh_count_backwards"] += 1
    # latmon.4: a flip named in a refresh that began before its Present was an immediate (tearing) flip inside that
    # refresh: within one refresh it is placed at the Present's return (not exact, not an anchor); further off the
    # sample is not that present's.
    tolerance = period if math.isfinite(period) else frequency / 50
    for key, (qpc, _exact) in sorted(flips.items()):
        output = by_count.get(key)
        if output is None or qpc >= output.event.qpc:
            continue
        if output.event.qpc - qpc <= tolerance:
            flips[key] = (max(output.event.a, output.event.qpc), False)
            coverage["clamped"] += 1
        else:
            del flips[key]
            coverage["refused"] += 1
    # latmon.3: the presents between two exact anchors flipped between them, evenly (DLSS-G hands a batch over at once
    # and the driver meters its flips, so a sample after each Present names about one flip per batch). latmon.4: evenly
    # from the last placed flip, never before a present began (after a long game frame the presenter idles).
    for chain in samples:
        anchors = sorted((present, qpc) for (c, present), (qpc, exact) in flips.items() if c == chain and exact)
        for (p0, t0), (p1, t1) in zip(anchors, anchors[1:]):
            gap = p1 - p0
            step = (t1 - t0) / gap if gap else 0
            if not (2 <= gap <= 32 and 0.00025 * frequency <= step <= 0.1 * frequency):
                continue
            from_present, from_flip = p0, t0
            for present in range(p0 + 1, p1):
                known = flips.get((chain, present))
                if known is not None:
                    if from_flip <= known[0] <= t1:
                        from_present, from_flip = present, known[0]
                    continue
                placed = from_flip + (t1 - from_flip) * (present - from_present) // (p1 - from_present)
                output = by_count.get((chain, present))
                if output is not None:
                    placed = max(placed, output.event.qpc)
                if placed > t1:
                    continue
                flips[(chain, present)] = (placed, False)
                from_present, from_flip = present, placed
                coverage["interpolated"] += 1
    for output in outputs:
        if output.count is None:
            continue
        hit = flips.get((output.chain, output.count))
        if hit and hit[0] > 0:
            output.flip, output.flip_exact = hit
    coverage["refresh_period_ticks"] = round(period, 3) if math.isfinite(period) else None
    return dict(coverage)


def take_mark(marks: deque[list], event: Event) -> int | None:
    """The newest untaken present-start marker set before the game's Present began (its thread first); with none,
    the first set while the call ran. It and every older marker are taken."""
    def newest(same_thread: bool) -> list | None:
        best = None
        for mark in marks:
            if mark[3] or mark[0] > event.qpc or (same_thread and mark[1] != event.thread):
                continue
            if best is None or mark[0] > best[0]:
                best = mark
        return best
    pick = newest(True) or newest(False)
    if pick is None:
        for mark in marks:
            if not mark[3] and event.qpc < mark[0] <= event.a and (pick is None or mark[0] < pick[0]):
                pick = mark
    if pick is None:
        return None
    for mark in marks:
        if mark[0] <= pick[0]:
            mark[3] = True
    return pick[2]


def analyze(probe: Probe, dlssg_lag: int = 0, allow_inferred: bool = False) -> tuple[dict, list[dict]]:
    frequency = probe.frequency

    def ms(ticks: float) -> float:
        return ticks * 1000.0 / frequency

    kinds = Counter(e.kind for e in probe.events)
    dropped = sum(e.a for e in probe.events if e.kind == "dropped")
    markers = [e for e in probe.events if e.kind == "marker"]
    marker_counts = Counter(MARKER_NAMES.get(e.sub, str(e.sub)) for e in markers)
    sim_starts: dict[int, list[int]] = defaultdict(list)
    input_sample: dict[int, int] = {}
    for marker in markers:
        if marker.sub == MARKER_SIM_START:
            # The first per frame counts; latmon.4: the same frame id over a second later is a new frame (reused id).
            starts = sim_starts[marker.a]
            if not starts or marker.qpc - starts[-1] > frequency:
                starts.append(marker.qpc)
        elif marker.sub in MARKER_INPUT:
            input_sample.setdefault(marker.a, marker.qpc)

    def start_of(frame: int, hand_over_qpc: int) -> int | None:
        starts = sim_starts.get(frame)
        if not starts:
            return None
        return starts[max(bisect.bisect_right(starts, hand_over_qpc) - 1, 0)]

    # latmon.4: the game's Present gets the newest present-start marker set before the call began, on its own thread when
    # that thread sets them (latency_monitor.h TakeMark): its row is written when the call returns, after markers the
    # game may already have set for later frames on another thread.
    marks: deque[list] = deque(maxlen=32)  # [qpc, thread, frame, taken]
    hand_overs: list[tuple[Event, int | None]] = []
    outputs: list[Output] = []
    stats_by_output: dict[int, Event] = {}
    pending_output: dict[int, Output] = {}
    xefg_label: dict[int, Event] = {}
    dlssg_presented = Counter()
    for event in probe.events:
        if event.kind == "marker" and event.sub == MARKER_PRESENT_START:
            marks.append([event.qpc, event.thread, event.a, False])
        elif event.kind == "game_present" and not event.sub & ROLE_NESTED:
            hand_overs.append((event, take_mark(marks, event)))
        elif event.kind == "xefg_output":
            xefg_label[event.thread] = event
        elif event.kind == "output_present":
            output = Output(event, event.c, event.sub)
            label = xefg_label.pop(event.thread, None)
            if label is not None and event.sub & ROLE_XEFG_OUTPUT:
                output.xefg_frame, output.generated = label.a, bool(label.sub)
            outputs.append(output)
            pending_output[event.thread] = output
        elif event.kind == "frame_stats":
            output = pending_output.pop(event.thread, None)
            if output is not None:
                stats_by_output[output.event.index] = event
        elif event.kind == "dlssg_state":
            dlssg_presented[event.a] += 1

    coverage = flip_times(outputs, stats_by_output, frequency)
    hand_overs.sort(key=lambda item: item[0].qpc)
    hand_over_qpcs = [h.qpc for h, _ in hand_overs]

    batches: dict[int, list[Output]] = defaultdict(list)  # index into hand_overs -> outputs
    window_counts = Counter()
    # XeFG: its frame labels; the batch belongs to the game Present that presented its real frame.
    xefg = [o for o in outputs if o.xefg_frame is not None]
    by_label: dict[int, list[Output]] = defaultdict(list)
    for output in xefg:
        by_label[output.xefg_frame].append(output)
    for label, group in by_label.items():
        real = [o for o in group if not o.generated]
        if not real:
            continue
        position = bisect.bisect_right(hand_over_qpcs, real[0].event.qpc) - 1
        if position >= 0 and hand_overs[position][0].qpc <= real[0].event.qpc <= hand_overs[position][0].a:
            batches[position].extend(group)
    # DLSS-G beneath Streamline: the outputs between one hand-over and the next.
    streamline = sorted((o for o in outputs if o.role & ROLE_STREAMLINE_BASE), key=lambda o: o.event.qpc)
    for output in streamline:
        position = bisect.bisect_right(hand_over_qpcs, output.event.qpc) - 1 - dlssg_lag
        if 0 <= position < len(hand_overs):
            batches[position].append(output)
    if streamline:
        for position in range(len(hand_overs)):
            window_counts[sum(1 for o in batches.get(position, []) if o.role & ROLE_STREAMLINE_BASE)] += 1

    frames: list[dict] = []
    latency_first, latency_real, game_part, fg_part, display_part, input_first = [], [], [], [], [], []
    missing = Counter()
    for position, (hand_over, frame) in enumerate(hand_overs):
        group = sorted(batches.get(position, []), key=lambda o: o.event.qpc)
        if not group:
            missing["no_outputs"] += 1
            continue
        start = start_of(frame, hand_over.qpc) if frame is not None else None
        if start is None:
            missing["no_simulation_start"] += 1
            continue
        first, last = group[0], group[-1]
        usable = (lambda o: o.flip is not None and (o.flip_exact or allow_inferred))
        if not usable(first):
            missing["first_flip_unknown"] += 1
            continue
        # latmon.4: the estimator's plausibility rules (latency_monitor.h Complete), by reason.
        if first.flip - start <= 0 or hand_over.qpc - start < 0:
            missing["implausible_before_start"] += 1
            continue
        if first.event.qpc - hand_over.qpc < 0 or first.flip - first.event.qpc < 0:
            missing["implausible_before_present"] += 1
            continue
        if first.flip - start > frequency:
            missing["implausible_too_long"] += 1
            continue
        record = {"frame": frame, "outputs": len(group), "simulation_start_qpc": start,
                  "hand_over_qpc": hand_over.qpc, "first_output_qpc": first.event.qpc,
                  "first_flip_qpc": first.flip, "first_flip_exact": first.flip_exact,
                  "latency_first_ms": round(ms(first.flip - start), 4),
                  "game_ms": round(ms(hand_over.qpc - start), 4),
                  "fg_ms": round(ms(first.event.qpc - hand_over.qpc), 4),
                  "display_ms": round(ms(first.flip - first.event.qpc), 4)}
        latency_first.append(record["latency_first_ms"])
        game_part.append(record["game_ms"])
        fg_part.append(record["fg_ms"])
        display_part.append(record["display_ms"])
        if usable(last):
            record["latency_real_ms"] = round(ms(last.flip - start), 4)
            latency_real.append(record["latency_real_ms"])
        if frame in input_sample:
            record["input_to_first_flip_ms"] = round(ms(first.flip - input_sample[frame]), 4)
            input_first.append(record["input_to_first_flip_ms"])
        frames.append(record)

    output_roles = Counter()
    for output in outputs:
        output_roles["streamline_base" if output.role & ROLE_STREAMLINE_BASE
                     else "xefg_output" if output.role & ROLE_XEFG_OUTPUT else "other"] += 1
    result = {
        "qpc_frequency": frequency, "pid": probe.pid, "events": dict(sorted(kinds.items())),
        "dropped_events": dropped, "markers": dict(sorted(marker_counts.items())),
        "hand_overs": len(hand_overs), "hand_overs_with_frame": sum(1 for _, f in hand_overs if f is not None),
        "outputs": dict(sorted(output_roles.items())),
        "flip_statistics": coverage,
        "outputs_with_flip": {"exact": sum(1 for o in outputs if o.flip is not None and o.flip_exact),
                              "inferred": sum(1 for o in outputs if o.flip is not None and not o.flip_exact)},
        "dlssg_outputs_per_hand_over": {str(k): v for k, v in sorted(window_counts.items())},
        "dlssg_state_presented": {str(k): v for k, v in sorted(dlssg_presented.items())},
        "dlssg_lag_batches": dlssg_lag, "allow_inferred": allow_inferred,
        "frames_measured": len(frames), "frames_skipped": dict(sorted(missing.items())),
        "latency_first": summary(latency_first), "latency_real": summary(latency_real),
        "input_to_first_flip": summary(input_first),
        "parts": {"game": summary(game_part), "fg": summary(fg_part), "display": summary(display_part)},
    }
    return result, frames


def presentmon_compare(probe: Probe, text: str, time_column: str | None, display_column: str | None,
                       tolerance_ms: float) -> dict:
    reader = csv.DictReader(io.StringIO(text))
    columns = reader.fieldnames or []
    time_column = time_column or next((c for c in ("TimeInQPC", "CPUStartQPC", "PresentStartQPC") if c in columns), None)
    display_column = display_column or next((c for c in ("MsUntilDisplayed",) if c in columns), None)
    result = {"columns": {"time": time_column, "display": display_column}}
    if not time_column or not display_column:
        result["error"] = "need a QPC time column and a display column (PresentMon --v1_metrics --qpc_time)"
        return result
    frequency = probe.frequency
    pm: list[tuple[int, int]] = []
    for row in reader:
        if probe.pid is not None and "ProcessID" in row and row["ProcessID"].strip().isdigit() \
                and int(row["ProcessID"]) != probe.pid:
            continue
        try:
            present = int(float(row[time_column]))
            until = float(row[display_column])
        except (TypeError, ValueError):
            continue
        if math.isfinite(until):
            pm.append((present, present + int(round(until * frequency / 1000.0))))
    pm.sort()
    starts = [p for p, _ in pm]
    tolerance = tolerance_ms * frequency / 1000.0
    stats_by_output: dict[int, Event] = {}
    outputs: list[Output] = []
    pending: dict[int, Output] = {}
    for event in probe.events:
        if event.kind == "output_present":
            output = Output(event, event.c, event.sub)
            outputs.append(output)
            pending[event.thread] = output
        elif event.kind == "frame_stats" and event.thread in pending:
            stats_by_output[pending.pop(event.thread).event.index] = event
    flip_times(outputs, stats_by_output, frequency)
    differences, matched = [], 0
    for output in outputs:
        position = bisect.bisect_left(starts, output.event.qpc)
        best = None
        for candidate in (position - 1, position):
            if 0 <= candidate < len(pm) and abs(pm[candidate][0] - output.event.qpc) <= tolerance:
                if best is None or abs(pm[candidate][0] - output.event.qpc) < abs(pm[best][0] - output.event.qpc):
                    best = candidate
        if best is None:
            continue
        matched += 1
        if output.flip is not None and output.flip_exact:
            differences.append((output.flip - pm[best][1]) * 1000.0 / frequency)
    result.update({"presentmon_rows": len(pm), "outputs_matched": matched,
                   "flip_minus_presentmon": summary(differences),
                   "abs_flip_difference": summary([abs(d) for d in differences])})
    return result


SYNTHETIC_FREQUENCY = 10_000_000
SYNTHETIC_VBLANK = SYNTHETIC_FREQUENCY // 240
SYNTHETIC_FRAMES = 120


def synthetic_model(frame: int) -> tuple[int, int, list[tuple[int, int]]]:
    """Frame `frame` of the synthetic run: simulation start, hand-over, and each output's (Present, flip)."""
    vblank = SYNTHETIC_VBLANK
    start = frame * 4 * vblank            # 60 fps base
    hand_over = start + 50_000             # 5 ms of game work
    first_vblank = (hand_over + 40_000) // vblank + 1
    outputs = [(hand_over + 40_000 + index * vblank, (first_vblank + index) * vblank) for index in range(4)]
    return start, hand_over, outputs


def synthetic(route: str) -> str:
    """A deterministic probe file: 60 fps base, 4 outputs per batch, 240 Hz; output k's Present 4 ms after the
    hand-over plus k vblanks, its flip at the next vblank. route "dlssg": DLSS-G's presenter thread beneath Streamline;
    "xefg": XeFG's generated outputs on its presenter thread, the real one inside the game's Present; "cross" (latmon.4):
    as dlssg, with the game's Present on a render thread, its row written after the next frame's markers."""
    vblank = SYNTHETIC_VBLANK
    lines = ["# latmon.1 latency probe (synthetic)",
             f"# qpc_frequency={SYNTHETIC_FREQUENCY} pid=4242 open_qpc=0 open_filetime=0", "kind,qpc,thread,sub,a,b,c,d"]
    game_thread, presenter, chain = 10, 20, 0xABC
    count, refresh0 = 0, 1000
    flips: list[tuple[int, int]] = []  # (present count, flip qpc)
    deferred: list[str] = []
    for frame in range(1, SYNTHETIC_FRAMES + 1):
        start, hand_over, outputs = synthetic_model(frame)
        lines.append(f"marker,{start},{game_thread},0,{frame},1,0,0")
        lines.append(f"marker,{hand_over - 10},{game_thread},4,{frame},1,0,0")
        lines.extend(deferred)
        deferred = []
        returned = hand_over + 2_000
        for index, (present, flip) in enumerate(outputs):
            count += 1
            generated = index < 3
            if route == "xefg":
                thread = presenter if generated else game_thread
                role = ROLE_XEFG_OUTPUT | (0 if generated else ROLE_NESTED)
                lines.append(f"xefg_output,{present - 5},{thread},{1 if generated else 0},{frame},0,0,0")
            else:
                thread, role = presenter, ROLE_STREAMLINE_BASE
            lines.append(f"output_present,{present},{thread},{role},{present + 500},0,{chain},0")
            flips.append((count, flip))
            # The statistics read right after this Present: the latest flip so far and the latest vblank sampled.
            now = present + 600
            shown = [(c, f) for c, f in flips if f <= now]
            if shown:
                shown_count, shown_flip = shown[-1]
                sync = now // vblank
                lines.append(f"frame_stats,{now},{thread},{STATS_COUNT | STATS_FRAME},{(count << 32) | shown_count},"
                             f"{refresh0 + shown_flip // vblank},{refresh0 + sync},{sync * vblank}")
            else:
                lines.append(f"frame_stats,{now},{thread},{STATS_COUNT},{count << 32},0,0,0")
            returned = max(returned, present + 1_000) if route == "xefg" else returned
        role_game = ROLE_XEMFG_HOST if route == "xefg" else 0
        # The probe writes the game's Present when it returns (after any output inside it).
        if route == "cross":
            render_thread, late = 30, hand_over + 4 * vblank + 10_000
            deferred = [f"game_present,{hand_over},{render_thread},{role_game},{late},0,77,0",
                        f"dlssg_state,{hand_over + 3_000},{render_thread},0,4,0,0,0"]
            continue
        lines.append(f"game_present,{hand_over},{game_thread},{role_game},{returned},0,77,0")
        lines.append(f"dlssg_state,{hand_over + 3_000},{game_thread},0,4,0,0,0")
    lines.extend(deferred)
    return "\n".join(lines) + "\n"


def self_test() -> int:
    failures = 0

    def check(condition: bool, what: str) -> None:
        nonlocal failures
        if not condition:
            failures += 1
            print(f"FAIL: {what}")

    vblank_ms = 1000.0 / 240
    # Simulation start -> the first output's flip: 9 ms to its Present, then the next vblank (start is on a vblank).
    expected_first = (90_000 // SYNTHETIC_VBLANK + 1) * SYNTHETIC_VBLANK * 1000.0 / SYNTHETIC_FREQUENCY
    for route in ("dlssg", "xefg", "cross"):
        probe = read_probe(synthetic(route))
        result, frames = analyze(probe)
        first = result["latency_first"]
        check(result["frames_measured"] >= SYNTHETIC_FRAMES - 2, f"{route}: frames measured {result['frames_measured']}"
              f" skipped {result['frames_skipped']}")
        check(abs(first.get("median_ms", 0) - expected_first) < 0.01, f"{route}: latency_first {first} vs {expected_first}")
        check(abs(result["latency_real"].get("median_ms", 0) - (expected_first + 3 * vblank_ms)) < 0.01,
              f"{route}: latency_real {result['latency_real']}")
        check(abs(result["parts"]["game"].get("median_ms", 0) - 5.0) < 0.001, f"{route}: game part")
        check(abs(result["parts"]["fg"].get("median_ms", 0) - 4.0) < 0.001, f"{route}: fg part")
        check(result["outputs_with_flip"]["inferred"] == 0, f"{route}: every flip exact")
        check(result["dropped_events"] == 0, f"{route}: dropped")
        check(all(math.isfinite(v) for f in frames for k, v in f.items() if k.endswith("_ms")), f"{route}: finite")
        again = analyze(read_probe(synthetic(route)))[0]
        check(json.dumps(result, sort_keys=True) == json.dumps(again, sort_keys=True), f"{route}: deterministic")
        if route == "dlssg":
            check(result["dlssg_outputs_per_hand_over"].get("4", 0) >= SYNTHETIC_FRAMES - 1,
                  f"dlssg windows {result['dlssg_outputs_per_hand_over']}")
            # Shifting the batches by one moves every frame's first flip 16.7 ms later.
            shifted = analyze(probe, dlssg_lag=1)[0]["latency_first"]
            check(abs(shifted.get("median_ms", 0) - (expected_first + 4 * vblank_ms)) < 0.01, f"dlssg lag {shifted}")
            # PresentMon 0.002 ms after each output Present, its display 0.03 ms after our vblank.
            pm = ["Application,ProcessID,TimeInQPC,MsUntilDisplayed"]
            for frame in range(1, SYNTHETIC_FRAMES + 1):
                for present, flip in synthetic_model(frame)[2]:
                    shown = flip + 300
                    pm.append(f"game.exe,4242,{present + 20},"
                              f"{(shown - present - 20) * 1000.0 / SYNTHETIC_FREQUENCY:.6f}")
            pm.append("other.exe,1,123,1.0")
            compare = presentmon_compare(probe, "\n".join(pm) + "\n", None, None, 0.5)
            check(compare.get("outputs_matched", 0) == 4 * SYNTHETIC_FRAMES, f"presentmon matched {compare}")
            check(compare["presentmon_rows"] == 4 * SYNTHETIC_FRAMES, "presentmon other process filtered")
            check(abs(compare["flip_minus_presentmon"].get("median_ms", 1) + 0.03) < 0.001,
                  f"presentmon difference {compare['flip_minus_presentmon']}")
            missing = presentmon_compare(probe, "Application,TimeInSeconds\n", None, None, 0.5)
            check("error" in missing, "presentmon without QPC columns is refused")
    # A header without the clock is refused.
    try:
        read_probe("kind,qpc,thread,sub,a,b,c,d\n")
        check(False, "missing qpc_frequency accepted")
    except ValueError:
        pass
    print("self-test:", "PASS" if not failures else f"{failures} FAILURES")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("probe", nargs="?")
    parser.add_argument("--presentmon")
    parser.add_argument("--pm-time-column")
    parser.add_argument("--pm-display-column")
    parser.add_argument("--pm-tolerance-ms", type=float, default=0.5)
    parser.add_argument("--dlssg-lag", type=int, default=0)
    parser.add_argument("--allow-inferred", action="store_true")
    parser.add_argument("--frames")
    parser.add_argument("--json")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.probe:
        parser.error("a probe CSV is required")
    with open(args.probe, encoding="utf-8") as handle:
        probe = read_probe(handle.read())
    result, frames = analyze(probe, args.dlssg_lag, args.allow_inferred)
    if args.presentmon:
        with open(args.presentmon, encoding="utf-8-sig") as handle:
            result["presentmon"] = presentmon_compare(probe, handle.read(), args.pm_time_column,
                                                      args.pm_display_column, args.pm_tolerance_ms)
    text = json.dumps(result, indent=2, sort_keys=True)
    print(text)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
    if args.frames and frames:
        with open(args.frames, "w", encoding="utf-8", newline="") as handle:
            keys = sorted({k for f in frames for k in f})
            writer = csv.DictWriter(handle, fieldnames=keys)
            writer.writeheader()
            writer.writerows(frames)
    return 0


if __name__ == "__main__":
    sys.exit(main())
