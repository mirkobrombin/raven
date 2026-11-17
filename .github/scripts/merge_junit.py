#!/usr/bin/env python3
"""Join the JUnit reports of the test shards into the one report scripts/package-native.py bundles a build with.

Each shard runs a part of the suite on the same build tree; together they ran every test. A fixture's setup test also runs in each
shard that holds a test needing it, so a test may come more than once: the joined report keeps it once, a failed run over a passed one,
so the bundle step can check that the whole suite ran and passed.

usage: merge_junit.py OUTPUT REPORT...
"""

import sys
import xml.etree.ElementTree as ET


def main():
    output, reports = sys.argv[1], sys.argv[2:]
    if not reports:
        print("no shard reports given", file=sys.stderr)
        return 1
    joined = ET.Element("testsuite", name="raven")
    counts = dict(tests=0, failures=0, errors=0, skipped=0)
    cases = {}
    for report in reports:
        root = ET.parse(report).getroot()
        for case in root.iter("testcase"):
            name = case.get("name")
            failed = case.find("failure") is not None or case.find("error") is not None
            if name not in cases or failed:
                cases[name] = case
    for case in cases.values():
        joined.append(case)
        counts["tests"] += 1
        for kind, key in (("failure", "failures"), ("error", "errors"), ("skipped", "skipped")):
            if case.find(kind) is not None:
                counts[key] += 1
    for key, value in counts.items():
        joined.set(key, str(value))
    ET.ElementTree(joined).write(output, encoding="utf-8", xml_declaration=True)
    print(f"{counts['tests']} test cases from {len(reports)} reports, {counts['failures'] + counts['errors']} failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
