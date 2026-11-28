"""Runs the translator's benchmarks: translate_bench for translation, the cache and the boundary, then entry.c's loop as rounds that
interleave the translated, native and Rosetta builds so load on the machine falls on all of them alike. Ends with the summary.

usage: bench_run.py BUILD_DIRECTORY RESULTS_DIRECTORY [LARGE_X64_IMAGE] [RUNS]
"""

import subprocess
import sys
from pathlib import Path


def main():
    build, results = Path(sys.argv[1]), Path(sys.argv[2])
    image = sys.argv[3] if len(sys.argv) > 3 else str(build / "guest/arguments.exe")
    runs = int(sys.argv[4]) if len(sys.argv) > 4 else 30
    results.mkdir(parents=True, exist_ok=True)
    with (results / "translate_bench.log").open("w") as log:
        subprocess.run([str(build / "translate_bench"), str(build / "guest"), str(results), image, str(runs)], check=True, stdout=log)
    series = {}
    for _ in range(runs):
        output = subprocess.run([str(build / "translate_bench"), str(build / "guest"), "--entry-round"], check=True,
                                capture_output=True, text=True).stdout
        for line in output.splitlines():
            name, value = line.split()
            series.setdefault(name, []).append(float(value))
        for name, program in (("entry_native_arm64_ns_per_call", "bench_entry_native"),
                              ("entry_rosetta_x86_64_ns_per_call", "bench_entry_rosetta")):
            output = subprocess.run([str(build / program), "1", "2000"], check=True, capture_output=True, text=True).stdout
            series.setdefault(name, []).append(float(output.splitlines()[-1].split(",")[1]))
    for name, values in series.items():
        with (results / f"{name}.csv").open("w") as out:
            out.write("run,ns\n" + "".join(f"{i},{value}\n" for i, value in enumerate(values)))
    subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("bench_summary.py")), str(results)], check=True)


if __name__ == "__main__":
    main()
