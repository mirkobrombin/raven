"""Runs hvf_vm_cost as a fresh process ROUNDS times per configuration, interleaved, and prints each field's median, minimum, maximum and
standard deviation, with the process's wall time from start to exit and vm.loadavg before and after.

usage: hvf_vm_cost_run.py BUILD_DIRECTORY [ROUNDS]

Configurations: none (this binary returning at once), bare (hv_vm_create and hv_vm_destroy), vm with 256 MiB and 1024 MiB of guest RAM.
"""

import os
import re
import statistics
import subprocess
import sys
import time


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    build = sys.argv[1]
    rounds = int(sys.argv[2]) if len(sys.argv) == 3 else 30
    program = os.path.join(build, "hvf_vm_cost")
    guest = os.path.join(build, "guest")
    configurations = {
        "none": [program, "none"],
        "bare": [program, "bare"],
        "vm 256 MiB": [program, "vm", guest, "256"],
        "vm 1024 MiB": [program, "vm", guest, "1024"],
    }
    samples = {name: {} for name in configurations}
    print(f"loadavg before: {' '.join(f'{v:.2f}' for v in os.getloadavg())}")
    for _ in range(rounds):
        for name, command in configurations.items():
            start = time.perf_counter_ns()
            done = subprocess.run(command, capture_output=True, text=True, check=True)
            wall = (time.perf_counter_ns() - start) / 1e3
            fields = {key: float(value) for key, value in re.findall(r"(\w+)=([\d.]+)", done.stdout)}
            fields["process_wall_us"] = wall
            for key, value in fields.items():
                samples[name].setdefault(key, []).append(value)
    print(f"loadavg after: {' '.join(f'{v:.2f}' for v in os.getloadavg())}")
    print(f"{rounds} processes per configuration; times in us, memory (phys_footprint) in KiB")
    for name, fields in samples.items():
        print(name)
        for key, values in fields.items():
            values.sort()
            print(f"  {key:16s} median {statistics.median(values):9.1f}  min {values[0]:9.1f}  max {values[-1]:9.1f}  "
                  f"stdev {statistics.pstdev(values):8.1f}")


if __name__ == "__main__":
    main()
