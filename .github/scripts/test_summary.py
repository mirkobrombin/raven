#!/usr/bin/env python3
"""Write the run summary: how each stage ended, the test counts of each shard and the tests that failed.

The stages come from the `needs` context in RESULTS; the tests from the JUnit files under DIRECTORY. The exit status is the
run's, so this step is the one required check branch protection needs.

usage: RESULTS='{...}' test_summary.py DIRECTORY
"""

import json
import os
import pathlib
import sys
import xml.etree.ElementTree as ET

MARKS = {"success": "passed", "failure": "**failed**", "cancelled": "cancelled", "skipped": "skipped"}


def shards(directory):
    for path in sorted(pathlib.Path(directory).glob("*/junit-*.xml")):
        suite = ET.parse(path).getroot()
        cases = list(suite.iter("testcase"))
        failed = [case.get("name") for case in cases if case.find("failure") is not None or case.find("error") is not None]
        skipped = sum(1 for case in cases if case.find("skipped") is not None)
        yield path.stem.removeprefix("junit-"), len(cases), failed, skipped


def main():
    stages = json.loads(os.environ.get("RESULTS", "{}"))
    print("## Stages\n")
    print("| Stage | Result |")
    print("| --- | --- |")
    for name, stage in stages.items():
        print(f"| {name} | {MARKS.get(stage['result'], stage['result'])} |")

    results = list(shards(sys.argv[1]))
    if results:
        print("\n## Tests\n")
        print("| Shard | Tests | Passed | Failed | Skipped |")
        print("| --- | ---: | ---: | ---: | ---: |")
        for shard, total, failed, skipped in results:
            print(f"| {shard} | {total} | {total - len(failed) - skipped} | {len(failed)} | {skipped} |")
        failures = sorted(name for _, _, failed, _ in results for name in failed)
        if failures:
            print("\n### Failed\n")
            for name in failures:
                print(f"- `{name}`")

    broken = [name for name, stage in stages.items() if stage["result"] in ("failure", "cancelled")]
    if broken:
        print(f"\n{', '.join(broken)} did not pass.")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
