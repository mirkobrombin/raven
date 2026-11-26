#!/usr/bin/env python3
"""Draw the README and website benchmark charts from docs/benchmarks.json.
Every workload of a chart gets its own panel with one bar per runtime: the median of the verified trials, labelled with that value and
with the successful and attempted trials. A runtime with no verified trial gets no bar and says so. The SVG files are written without a
date and with a fixed hash salt, so drawing the same data twice gives the same files.
usage: benchmark-charts.py DATA OUTPUT_DIRECTORY
"""
import json
from pathlib import Path
import sys

import matplotlib

matplotlib.use("svg")
import matplotlib.pyplot as plt

COLOURS = ("#365f85", "#8c6b3e", "#65725c")
TRIALS = "Median of verified trials; labels show successful/attempted trials. Software rendering with Mesa lavapipe."
ROUNDS = "Median of four alternating rounds; all samples retained. Software rendering with Mesa lavapipe."
CHARTS = (
    ("cli", "benchmark-cli.svg", "Process completion time", "seconds (lower is faster)", "{:.4f}", TRIALS),
    ("apps", "benchmark-apps.svg", "Main-window delay: Notepad++ 8.7.6 and ClassiCube 1.3.8", "seconds (lower is faster)", "{:.4f}", TRIALS),
    ("frames", "benchmark-frames.svg", "Direct3D 11 presentation rate, 256 x 256", "frames/s (higher is faster)", "{:.1f}", TRIALS),
    ("games", "benchmark-games.svg", "ClassiCube 1.3.8 singleplayer, seed 1, 640 x 480", "frames/s (higher is faster)", "{:.1f}", ROUNDS),
)


def panel(axes, workload, results, axis_label, value_format):
    runtimes = list(results)
    values = [results[runtime]["value"] for runtime in runtimes]
    top = max(value for value in values if value is not None)
    for index, (runtime, value) in enumerate(zip(runtimes, values)):
        result = results[runtime]
        if value is None:
            axes.text(index + 0.35, 0, "did not run", rotation=90, ha="center", va="bottom", color="#555555")
            continue
        axes.bar(index, value, width=0.6, color=COLOURS[index % len(COLOURS)])
        label = value_format.format(value) + f"\n{result['samples']}/{result['attempts']}"
        axes.text(index, value + top * 0.015, label, ha="center", va="bottom", fontsize=9)
    axes.set_title(workload)
    axes.set_ylabel(axis_label)
    axes.set_ylim(0, top * 1.25)
    axes.set_xlim(-0.5, len(runtimes) - 0.5)
    axes.set_xticks(range(len(runtimes)), runtimes, rotation=20, ha="right")
    axes.grid(axis="y", color="#e5e5e5")
    axes.set_axisbelow(True)
    axes.spines[["top", "right"]].set_visible(False)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    data = json.loads(Path(sys.argv[1]).read_text())
    output = Path(sys.argv[2])
    plt.rcParams["svg.hashsalt"] = "raven-benchmarks"
    for key, name, title, axis_label, value_format, note in CHARTS:
        workloads = data[key]
        figure, panels = plt.subplots(1, len(workloads), figsize=(12, 4.4), squeeze=False)
        for axes, (workload, results) in zip(panels[0], workloads.items()):
            panel(axes, workload, results, axis_label, value_format)
        figure.suptitle(title, fontsize=14)
        figure.text(0.5, 0.02, note, ha="center", fontsize=9)
        figure.tight_layout(rect=(0, 0.06, 1, 0.95))
        figure.savefig(output / name, metadata={"Date": None})
        plt.close(figure)


if __name__ == "__main__":
    main()
