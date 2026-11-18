"""Times every workload natively, translated in this process and translated in a vCPU, interleaved round by round on one machine.

usage: hvf_bench_run.py BUILD_DIRECTORY RESULTS_DIRECTORY [ROUNDS] [THERMAL_PROGRAM] [WORKLOAD,...]

Each round runs each configuration once as its own process: one first call (translation included), then RUNS timed runs of CALLS
workload calls, each at least 50 ms long on this machine. The first timed run warms the core up and is dropped in every configuration
alike; the process's value is the median of the others. Configurations: native (bench_native.c), process-loop and process-call (Raven's translator in this process, plain
mode), vm-loop and vm-call (the same in a Hypervisor.framework vCPU with ACTLR_EL1.EnTSO); hvf_bench.cpp says what loop and call
mean. Writes measurements.jsonl (one line per process), summary.csv (ratios to native's median of the same session) and state.json
(vm.loadavg and the thermal state before and after). Then times a guest write to translated code (hvf_bench write) into
write_fault.txt.
"""

import csv
import json
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path

# Calls per timed run: at least 50 ms natively (entry about 330 ns per call, crc32 13 us, fnv 34 us, histogram 52 us).
WORKLOADS = {"entry": 200000, "crc32": 4000, "fnv": 1500, "histogram": 1000}
# The SSE workloads (testdata/sse_workloads.c), run when named.
SSE_WORKLOADS = {"saxpy": 60000, "dot": 10000, "copy": 40000}
# The x87 workloads (testdata/x87_workloads.c), run when named. The native build's long double is a double: its checksum differs from
# x86's, and only the translated configurations must agree. Fewer calls than the others: translated they take 50 ms or more; natively
# they do not.
X87_WORKLOADS = {"ldot": 100, "lpoly": 200}
RUNS = 6
CONFIGURATIONS = ["native", "process-loop", "process-call", "vm-loop", "vm-call"]


def machine_state(thermal):
    state = {"time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
             "loadavg": subprocess.run(["sysctl", "-n", "vm.loadavg"], capture_output=True, text=True).stdout.strip()}
    if thermal:
        completed = subprocess.run([thermal], capture_output=True, text=True)
        state["thermal"] = completed.stdout.strip() if completed.returncode == 0 else f"failed ({completed.returncode})"
    return state


def command(build, workload, calls, configuration):
    image = str(build / "guest" / f"hvf_{workload}.exe")
    if configuration == "native":
        return [str(build / f"hvf_native_{workload}"), str(RUNS), str(calls)]
    engine, method = configuration.split("-")
    return [str(build / "hvf_bench"), engine, image, str(RUNS), str(calls), method]


def parse(output):
    values = {"runs_ns_per_call": []}
    for line in output.splitlines():
        parts = line.split()
        if parts[0] == "first":
            values["first_ns"] = float(parts[1])
        elif parts[0] == "run":
            values["runs_ns_per_call"].append(float(parts[2]))
        elif parts[0] == "entries":
            values["entries"] = int(parts[1])
        elif parts[0] == "checksum":
            values["checksum"] = int(parts[1])
    if "first_ns" not in values or len(values["runs_ns_per_call"]) != RUNS:
        raise RuntimeError("no timing in output:\n" + output)
    values["warm_ns_per_call"] = statistics.median(values["runs_ns_per_call"][1:])
    return values


def main():
    build, results = Path(sys.argv[1]), Path(sys.argv[2])
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    thermal = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else None
    if len(sys.argv) > 5:
        chosen = sys.argv[5].split(",")
        WORKLOADS.clear()
        WORKLOADS.update({name: {**SSE_WORKLOADS, **X87_WORKLOADS, "entry": 200000, "crc32": 4000, "fnv": 1500, "histogram": 1000}[name]
                          for name in chosen})
    results.mkdir(parents=True, exist_ok=True)
    state = {"machine": subprocess.run(["sysctl", "-n", "hw.model"], capture_output=True, text=True).stdout.strip(),
             "os": platform.mac_ver()[0], "rounds": rounds, "calls": WORKLOADS, "runs": RUNS, "start": machine_state(thermal)}
    series = {}
    with (results / "measurements.jsonl").open("w") as log:
        for round_index in range(rounds):
            for workload, calls in WORKLOADS.items():
                for configuration in CONFIGURATIONS:
                    completed = subprocess.run(command(build, workload, calls, configuration), capture_output=True, text=True)
                    if completed.returncode != 0:
                        raise RuntimeError(f"{configuration} {workload} failed ({completed.returncode}):\n"
                                           f"{completed.stdout}\n{completed.stderr}")
                    values = parse(completed.stdout)
                    log.write(json.dumps({"round": round_index, "workload": workload, "configuration": configuration,
                                          "calls": calls, **values}) + "\n")
                    for metric in ("warm_ns_per_call", "first_ns"):
                        series.setdefault((workload, configuration, metric), []).append(values[metric])
                    series.setdefault((workload, configuration, "checksum"), []).append(values["checksum"])
            print(f"round {round_index + 1} of {rounds} done", flush=True)
    completed = subprocess.run([str(build / "hvf_bench"), "write", str(build / "guest" / "hvf_client.exe"), "10", "20000"],
                               capture_output=True, text=True, check=True)
    (results / "write_fault.txt").write_text(completed.stdout)
    state["end"] = machine_state(thermal)
    (results / "state.json").write_text(json.dumps(state, indent=2) + "\n")
    for workload in WORKLOADS:
        checksums = {configuration: set(series[(workload, configuration, "checksum")]) for configuration in CONFIGURATIONS
                     if not (workload in X87_WORKLOADS and configuration == "native")}
        if len(set().union(*checksums.values())) != 1:
            raise RuntimeError(f"{workload}: the engines disagree on the checksum: {checksums}")
    with (results / "summary.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(["workload", "configuration", "metric", "n", "median", "p95", "stddev", "min", "max",
                         "median_ratio_to_native"])
        for workload in WORKLOADS:
            for metric in ("warm_ns_per_call", "first_ns"):
                native = statistics.median(series[(workload, "native", metric)])
                for configuration in CONFIGURATIONS:
                    values = sorted(series[(workload, configuration, metric)])
                    p95 = values[min(len(values) - 1, round(0.95 * (len(values) - 1)))]
                    writer.writerow([workload, configuration, metric, len(values), f"{statistics.median(values):.3f}", f"{p95:.3f}",
                                     f"{statistics.pstdev(values):.3f}", f"{values[0]:.3f}", f"{values[-1]:.3f}",
                                     f"{statistics.median(values) / native:.3f}"])
    print((results / "summary.csv").read_text())
    print(json.dumps(state, indent=2))


if __name__ == "__main__":
    main()
