#!/usr/bin/env python3
"""Tests of shard_reports.py on the artifacts of a run whose second shard failed and passed when run again: the report taken is the
re-run's, though the failed attempt's artifact has the higher ID, and merge_junit.py joins a passing report from what it downloaded."""

import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shard_reports


def junit(shard, failed=()):
    cases = "".join(f'<testcase name="{name}">' + ("<failure message=\"Failed\"/>" if name in failed else "") + "</testcase>"
                    for name in (f"native_shard{shard}_a", f"native_shard{shard}_b"))
    return f'<?xml version="1.0"?><testsuite tests="2" failures="{len(failed)}">{cases}</testsuite>'


def artifact_zip(shard, report):
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w") as archive:
        archive.writestr(f"junit-{shard}.xml", report)
        archive.writestr("Testing/Temporary/LastTest.log", f"shard {shard}\n")
    return data.getvalue()


# Listed as GitHub lists the artifacts of CI run 46091327134: Test 2/6 failed at 01:23 and passed when run again at 01:33.
ARTIFACTS = [
    dict(id=15242300494, name="tests-2", created_at="2026-11-28T01:23:01Z"),
    dict(id=15241214889, name="tests-2", created_at="2026-11-28T01:33:37Z"),
    dict(id=15241594569, name="tests-1", created_at="2026-11-28T01:25:05Z"),
    dict(id=15241668549, name="tests-ntsync", created_at="2026-11-28T01:20:41Z"),
    dict(id=15241482651, name="build", created_at="2026-11-28T01:18:22Z"),
    dict(id=15241476300, name="component-controls", created_at="2026-11-28T01:07:51Z"),
]
ZIPS = {
    15242300494: artifact_zip(2, junit(2, failed={"native_shard2_b"})),
    15241214889: artifact_zip(2, junit(2)),
    15241594569: artifact_zip(1, junit(1)),
    15241668549: artifact_zip("ntsync", junit("ntsync")),
}


class Reports(unittest.TestCase):
    def setUp(self):
        self.asked = []
        self.listing = ARTIFACTS
        self.gh = shard_reports.gh
        shard_reports.gh = self.answer
        self.folder = Path(tempfile.mkdtemp())
        self.argv = sys.argv

    def tearDown(self):
        shard_reports.gh = self.gh
        sys.argv = self.argv

    def answer(self, *arguments):
        self.asked.append(arguments)
        if arguments[0] == "--paginate":
            self.assertEqual(arguments[1:], ("repos/{owner}/{repo}/actions/runs/46091327134/artifacts", "--jq", ".artifacts[]"))
            return "".join(json.dumps(artifact) + "\n" for artifact in self.listing).encode()
        identifier = int(arguments[0].split("/")[-2])
        self.assertEqual(arguments[0], f"repos/{{owner}}/{{repo}}/actions/artifacts/{identifier}/zip")
        return ZIPS[identifier]

    def download(self):
        sys.argv = ["shard_reports.py", "46091327134", str(self.folder)]
        return shard_reports.main()

    def test_takes_the_report_uploaded_last(self):
        self.assertEqual(self.download(), 0)
        fetched = [arguments[0].split("/")[-2] for arguments in self.asked[1:]]
        self.assertEqual(fetched, ["15241594569", "15241214889", "15241668549"])
        self.assertEqual((self.folder / "tests-2/junit-2.xml").read_text(), junit(2))
        self.assertTrue((self.folder / "tests-2/Testing/Temporary/LastTest.log").is_file())
        self.assertFalse((self.folder / "build").exists())

    def test_joined_report_passes(self):
        self.assertEqual(self.download(), 0)
        joined = self.folder / "joined.xml"
        reports = sorted(str(path) for path in self.folder.glob("*/junit-*.xml"))
        subprocess.run([sys.executable, str(Path(shard_reports.__file__).with_name("merge_junit.py")), str(joined), *reports],
                       check=True, stdout=subprocess.DEVNULL)
        root = ET.parse(joined).getroot()
        self.assertEqual((root.get("tests"), root.get("failures"), root.get("errors")), ("6", "0", "0"))

    def test_run_without_test_results(self):
        self.listing = [artifact for artifact in ARTIFACTS if not artifact["name"].startswith("tests-")]
        self.assertEqual(self.download(), 1)
        self.assertEqual(len(self.asked), 1)


if __name__ == "__main__":
    unittest.main()
