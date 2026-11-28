"""Summarizes the per-run CSVs translate_bench and the bench_entry programs write: one row per metric with n, median, p95 and stddev.

usage: bench_summary.py RESULTS_DIRECTORY
"""

import csv
import math
import statistics
import sys
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    rank = fraction * (len(ordered) - 1)
    low, high = math.floor(rank), math.ceil(rank)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def main():
    directory = Path(sys.argv[1])
    rows = []
    for path in sorted(directory.glob("*.csv")):
        if path.name == "summary.csv":
            continue
        with path.open() as source:
            reader = csv.reader(source)
            header = next(reader)
            values = [float(row[1]) for row in reader if len(row) > 1]
        if not values:
            continue
        rows.append([path.stem, header[1], len(values), statistics.median(values), percentile(values, 0.95),
                     statistics.pstdev(values), min(values), max(values)])
    with (directory / "summary.csv").open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(["metric", "unit", "n", "median", "p95", "stddev", "min", "max"])
        for row in rows:
            writer.writerow(row[:3] + [f"{value:.3f}" for value in row[3:]])
    for row in rows:
        print(f"{row[0]:45} {row[1]:>4} n={row[2]:<3} median={row[3]:12.3f} p95={row[4]:12.3f} stddev={row[5]:10.3f}")


if __name__ == "__main__":
    main()
