"""Runs every workload under every engine and memory-ordering mode, interleaved round by round on one machine.

usage: linux_bench.py OURS_BUILD HARNESS_DIRECTORY FEX_INTERPRETER RESULTS [ROUNDS]

Engines:
  native       the arm64 build of the workload (baseline)
  ours-*       Raven's translator running the x64 PE: plain (needs hardware TSO), ldapr (LDAPR/STLR), dmb (barriers)
  fex-*        FEXInterpreter running the static x86-64 ELF: tso0 (TSO emulation off), tso1 (default, LRCPC/LRCPC2 when present),
               tso1-nolrcpc (TSO with LRCPC and LRCPC2 disabled, so barriers)
Each round runs each configuration once: one first call (translation or JIT included) and one timed run of CALLS calls.
"""

import csv
import os
import statistics
import subprocess
import sys
from pathlib import Path

WORKLOADS = {"entry": 2000, "crc32": 20, "fnv": 50, "histogram": 50}


def parse(output):
    first, warm, translated = None, None, None
    for line in output.splitlines():
        parts = line.split()
        if parts and parts[0] == "first":
            first = float(parts[1])
        elif parts and parts[0] == "run":
            warm = float(parts[2])
        elif parts and parts[0] == "translated":
            translated = [int(value) for value in parts[1:]]
    if first is None or warm is None:
        raise RuntimeError("no timing in output:\n" + output)
    return first, warm, translated


def main():
    ours, harness, results = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]); fex = None
    rounds = int(sys.argv[4]) if len(sys.argv) > 4 else 30
    results.mkdir(parents=True, exist_ok=True)
    fex_modes = {"rosetta": {}}
    series = {}
    translation = {}
    for round_index in range(rounds):
        for workload, calls in WORKLOADS.items():
            configurations = [("native", [str(harness / f"{workload}.native"), "1", str(calls)], None)]
            for mode in ("plain", "ldapr", "dmb"):
                configurations.append((f"ours-{mode}", [str(ours / "translate_bench"), str(ours / "guest"), "--workload",
                                                        str(ours / "guest" / f"{workload}.exe"), "1", str(calls), mode], None))
            for mode, environment in fex_modes.items():
                configurations.append((mode, [str(harness / f"{workload}.x86_64"), "1", str(calls)],
                                       dict(os.environ, **environment)))
            for name, command, environment in configurations:
                completed = subprocess.run(command, capture_output=True, text=True, env=environment)
                if completed.returncode != 0:
                    raise RuntimeError(f"{name} {workload} failed ({completed.returncode}):\n{completed.stdout}\n{completed.stderr}")
                first, warm, translated = parse(completed.stdout)
                series.setdefault((workload, name, "warm_ns_per_call"), []).append(warm)
                series.setdefault((workload, name, "first_call_ns"), []).append(first)
                if translated:
                    translation[(workload, name)] = translated
        print(f"round {round_index + 1} of {rounds} done", flush=True)
    rows = []
    for (workload, name, metric), values in sorted(series.items()):
        path = results / f"{workload}__{name}__{metric}.csv"
        with path.open("w") as out:
            out.write("run,ns\n" + "".join(f"{i},{value}\n" for i, value in enumerate(values)))
        rows.append((workload, name, metric, values))
    with (results / "linux_summary.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(["workload", "engine", "metric", "n", "median", "p95", "stddev", "min", "max", "median_ratio_to_native"])
        for workload, name, metric, values in rows:
            native = statistics.median(series[(workload, "native", metric)])
            ordered = sorted(values)
            p95 = ordered[min(len(ordered) - 1, round(0.95 * (len(ordered) - 1)))]
            writer.writerow([workload, name, metric, len(values), f"{statistics.median(values):.3f}", f"{p95:.3f}",
                             f"{statistics.pstdev(values):.3f}", f"{min(values):.3f}", f"{max(values):.3f}",
                             f"{statistics.median(values) / native:.3f}"])
    with (results / "ours_translation.csv").open("w") as out:
        out.write("workload,engine,guest_instructions,blocks,translate_ns\n")
        for (workload, name), (instructions, blocks, nanoseconds) in sorted(translation.items()):
            out.write(f"{workload},{name},{instructions},{blocks},{nanoseconds}\n")
    print((results / "linux_summary.csv").read_text())


if __name__ == "__main__":
    main()
