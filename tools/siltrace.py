#!/usr/bin/env python3
"""siltrace.py - read .siltrace files written by the SIL runtime and compute timing statistics.

  siltrace.py stats <file.siltrace> [--csv out.csv]
  siltrace.py dump  <file.siltrace> [--limit N]

Statistics (per core): per task activations, response time (activate->terminate/wait) and net
execution time (sum of run segments) in virtual time, host CPU cost per run; per runnable count,
net time, period and jitter; per ISR count, latency (raise->start) and execution time; idle share.
Terminology follows the MICROSAR OS timing note (CET = net execution time, RT = response time,
DT = period).
"""
from __future__ import annotations

import argparse
import csv
import statistics
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

EVENTS = [
    "none", "task_activate", "task_start", "task_preempt", "task_resume", "task_terminate", "task_wait", "task_release",
    "task_act_fail", "irq_raise", "isr_start", "isr_end", "alarm_set", "alarm_cancel", "alarm_expire", "event_set",
    "res_get", "res_release", "spin_req", "spin_got", "spin_rel", "xsig_send", "xsig_recv", "barrier_wait",
    "barrier_pass", "idle_start", "idle_end", "runnable_start", "runnable_stop", "int_lock", "int_unlock", "clock_jump",
    "os_error", "hook", "user", "bus_tx", "bus_rx",
]
NAME_KINDS = ["core", "task", "isr", "alarm", "resource", "spinlock", "barrier", "runnable", "counter", "user"]
REC = struct.Struct("<QQIIHBBI")  # t_ns, host_ns, id, aux, type, core, ctx_kind, ctx_id (32 bytes)


@dataclass
class Record:
    t: int
    host: int
    id: int
    aux: int
    type: str
    core: int
    ctx_kind: int
    ctx_id: int


class Trace:
    def __init__(self) -> None:
        self.names: Dict[Tuple[int, int], str] = {}
        self.records: List[Record] = []
        self.num_cores = 0

    @staticmethod
    def load(path: str) -> "Trace":
        t = Trace()
        with open(path, "rb") as f:
            magic = f.read(9)
            if magic != b"SILTRACE1":
                raise SystemExit(f"{path}: not a .siltrace file")
            t.num_cores = struct.unpack("<I", f.read(4))[0]
            num_names = struct.unpack("<I", f.read(4))[0]
            for _ in range(num_names):
                kind, ident, ln = struct.unpack("<III", f.read(12))
                t.names[(kind, ident)] = f.read(ln).decode("utf-8", "replace")
            count = struct.unpack("<Q", f.read(8))[0]
            data = f.read(count * REC.size)
            for i in range(count):
                t_ns, host, ident, aux, typ, core, ck, cid = REC.unpack_from(data, i * REC.size)
                t.records.append(Record(t_ns, host, ident, aux, EVENTS[typ] if typ < len(EVENTS) else str(typ), core, ck, cid))
        return t

    def name(self, kind: str, ident: int) -> str:
        return self.names.get((NAME_KINDS.index(kind), ident), f"{kind}{ident}")


def pct(v: List[float], p: float) -> float:
    if not v:
        return 0.0
    s = sorted(v)
    k = min(len(s) - 1, int(round((len(s) - 1) * p)))
    return s[k]


def stats(tr: Trace, csv_path: Optional[str]) -> None:
    rows: List[Dict[str, object]] = []
    end = max((r.t for r in tr.records), default=0)
    # tasks
    act_time: Dict[Tuple[int, int], int] = {}
    run_start: Dict[Tuple[int, int], Tuple[int, int]] = {}
    cet: Dict[Tuple[int, int], int] = defaultdict(int)
    hostc: Dict[Tuple[int, int], int] = defaultdict(int)
    task_rt: Dict[Tuple[int, int], List[int]] = defaultdict(list)
    task_cet: Dict[Tuple[int, int], List[int]] = defaultdict(list)
    task_host: Dict[Tuple[int, int], List[int]] = defaultdict(list)
    task_act: Dict[Tuple[int, int], int] = defaultdict(int)
    task_preempt: Dict[Tuple[int, int], int] = defaultdict(int)
    # ISRs
    raise_t: Dict[Tuple[int, int], int] = {}
    isr_start: Dict[Tuple[int, int], Tuple[int, int]] = {}
    isr_lat: Dict[Tuple[int, int], List[int]] = defaultdict(list)
    isr_cet: Dict[Tuple[int, int], List[int]] = defaultdict(list)
    isr_host: Dict[Tuple[int, int], List[int]] = defaultdict(list)
    # runnables
    rn_start: Dict[Tuple[int, int], Tuple[int, int]] = {}
    rn_cet: Dict[Tuple[int, int], List[int]] = defaultdict(list)
    rn_host: Dict[Tuple[int, int], List[int]] = defaultdict(list)
    rn_starts: Dict[Tuple[int, int], List[int]] = defaultdict(list)
    idle: Dict[int, int] = defaultdict(int)
    idle_start: Dict[int, int] = {}
    errors = 0
    for r in sorted(tr.records, key=lambda x: (x.t, x.host)):
        k = (r.core, r.id)
        if r.type == "task_activate":
            act_time.setdefault(k, r.t)
            task_act[k] += 1
        elif r.type in ("task_start", "task_resume"):
            run_start[k] = (r.t, r.host)
        elif r.type in ("task_preempt", "task_terminate", "task_wait"):
            if k in run_start:
                cet[k] += r.t - run_start[k][0]
                hostc[k] += r.host - run_start[k][1]
                del run_start[k]
            if r.type == "task_preempt":
                task_preempt[k] += 1
            else:
                if k in act_time:
                    task_rt[k].append(r.t - act_time[k])
                    del act_time[k]
                task_cet[k].append(cet[k])
                task_host[k].append(hostc[k])
                cet[k] = 0
                hostc[k] = 0
        elif r.type == "task_release":
            act_time.setdefault(k, r.t)
        elif r.type == "irq_raise":
            raise_t.setdefault(k, r.t)
        elif r.type == "isr_start":
            isr_start[k] = (r.t, r.host)
            if k in raise_t:
                isr_lat[k].append(r.t - raise_t[k])
                del raise_t[k]
        elif r.type == "isr_end":
            if k in isr_start:
                isr_cet[k].append(r.t - isr_start[k][0])
                isr_host[k].append(r.host - isr_start[k][1])
                del isr_start[k]
        elif r.type == "runnable_start":
            rn_start[k] = (r.t, r.host)
            rn_starts[k].append(r.t)
        elif r.type == "runnable_stop":
            if k in rn_start:
                rn_cet[k].append(r.t - rn_start[k][0])
                rn_host[k].append(r.host - rn_start[k][1])
                del rn_start[k]
        elif r.type == "idle_start":
            idle_start[r.core] = r.t
        elif r.type == "idle_end":
            if r.core in idle_start:
                idle[r.core] += r.t - idle_start[r.core]
                del idle_start[r.core]
        elif r.type == "os_error":
            errors += 1
    for c in idle_start:
        idle[c] += end - idle_start[c]

    def us(v: float) -> str:
        return f"{v / 1000.0:10.1f}"

    print(f"trace: {len(tr.records)} records, {tr.num_cores} cores, {end / 1e9:.6f} s virtual, {errors} OS errors")
    for c in range(tr.num_cores):
        load = 100.0 * (1.0 - idle[c] / end) if end else 0.0
        print(f"core {c}: idle {idle[c] / 1e9:.6f} s, load {load:.1f} %")
    print("\nTASKS (virtual time in us)        count   RT max   RT avg  CET max  CET avg  preempt  host/run us")
    for k in sorted(task_act, key=lambda x: (x[0], tr.name('task', x[1]))):
        rt, ce, ho = task_rt[k], task_cet[k], task_host[k]
        name = f"c{k[0]} {tr.name('task', k[1])}"
        print(f"{name:34s}{task_act[k]:6d}{us(max(rt) if rt else 0)}{us(statistics.mean(rt) if rt else 0)}"
              f"{us(max(ce) if ce else 0)}{us(statistics.mean(ce) if ce else 0)}{task_preempt[k]:9d}{us(statistics.mean(ho) if ho else 0)}")
        rows.append({"kind": "task", "core": k[0], "name": tr.name('task', k[1]), "count": task_act[k], "rt_max_us": (max(rt) if rt else 0) / 1000,
                     "cet_max_us": (max(ce) if ce else 0) / 1000, "cet_avg_us": (statistics.mean(ce) if ce else 0) / 1000,
                     "preempt": task_preempt[k], "host_avg_us": (statistics.mean(ho) if ho else 0) / 1000})
    print("\nISRS                              count  lat max  lat avg  CET max  CET avg  host/run us")
    for k in sorted(isr_cet, key=lambda x: (x[0], tr.name('isr', x[1]))):
        la, ce, ho = isr_lat[k], isr_cet[k], isr_host[k]
        name = f"c{k[0]} {tr.name('isr', k[1])}"
        print(f"{name:34s}{len(ce):6d}{us(max(la) if la else 0)}{us(statistics.mean(la) if la else 0)}{us(max(ce))}{us(statistics.mean(ce))}{us(statistics.mean(ho))}")
        rows.append({"kind": "isr", "core": k[0], "name": tr.name('isr', k[1]), "count": len(ce), "lat_max_us": (max(la) if la else 0) / 1000,
                     "cet_max_us": max(ce) / 1000, "cet_avg_us": statistics.mean(ce) / 1000, "host_avg_us": statistics.mean(ho) / 1000})
    print("\nRUNNABLES                         count  CET max  CET avg  DT avg   jitter   host avg us  host p99 us")
    for k in sorted(rn_cet, key=lambda x: (x[0], tr.name('runnable', x[1]))):
        ce, ho, st = rn_cet[k], rn_host[k], rn_starts[k]
        periods = [b - a for a, b in zip(st, st[1:])]
        dt = statistics.mean(periods) if periods else 0
        jit = (max(periods) - min(periods)) if periods else 0
        name = f"c{k[0]} {tr.name('runnable', k[1])}"
        print(f"{name:34s}{len(ce):6d}{us(max(ce))}{us(statistics.mean(ce))}{us(dt)}{us(jit)}{us(statistics.mean(ho))}{us(pct(ho, 0.99))}")
        rows.append({"kind": "runnable", "core": k[0], "name": tr.name('runnable', k[1]), "count": len(ce), "cet_max_us": max(ce) / 1000,
                     "cet_avg_us": statistics.mean(ce) / 1000, "dt_avg_us": dt / 1000, "jitter_us": jit / 1000,
                     "host_avg_us": statistics.mean(ho) / 1000, "host_p99_us": pct(ho, 0.99) / 1000})
    if csv_path:
        keys = sorted({k for r in rows for k in r})
        with open(csv_path, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=keys)
            w.writeheader()
            for r in rows:
                w.writerow(r)
        print(f"\nwrote {csv_path}")


def dump(tr: Trace, limit: int) -> None:
    kinds = {"task_activate": "task", "task_start": "task", "task_preempt": "task", "task_resume": "task", "task_terminate": "task",
             "task_wait": "task", "task_release": "task", "task_act_fail": "task", "irq_raise": "isr", "isr_start": "isr", "isr_end": "isr",
             "alarm_set": "alarm", "alarm_cancel": "alarm", "alarm_expire": "alarm", "event_set": "task", "res_get": "resource",
             "res_release": "resource", "spin_req": "spinlock", "spin_got": "spinlock", "spin_rel": "spinlock", "runnable_start": "runnable",
             "runnable_stop": "runnable", "barrier_wait": "barrier", "barrier_pass": "barrier"}
    for i, r in enumerate(sorted(tr.records, key=lambda x: (x.t, x.host))):
        if i >= limit:
            print("...")
            break
        kind = kinds.get(r.type)
        name = tr.name(kind, r.id) if kind else str(r.id)
        print(f"{r.t / 1000:14.3f} us  core{r.core}  {r.type:16s} {name:40s} aux={r.aux}")


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stats")
    s.add_argument("file")
    s.add_argument("--csv", default=None)
    d = sub.add_parser("dump")
    d.add_argument("file")
    d.add_argument("--limit", type=int, default=200)
    args = ap.parse_args(argv)
    tr = Trace.load(args.file)
    if args.cmd == "stats":
        stats(tr, args.csv)
    else:
        dump(tr, args.limit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
