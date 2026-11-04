#!/usr/bin/env python3
"""Trace rays through a Direct3D 12 ray tracing pipeline on vkd3d-proton and Raven and compare the read-back pixels with a reference.

testdata/d3d12/raytrace.exe (or rayquery.exe, which traces inline with RayQuery in a compute shader) builds a bottom level acceleration structure with one triangle and a top level one with one instance, creates a
ray tracing pipeline from DXIL shaders (rt.hlsl: ray generation, closest hit and miss), traces one orthographic ray per pixel with
DispatchRays and writes the picture it read back. The triangle has the geometry and vertex colours of the rasterized workloads, so
check-d3d11-triangle.py compares the pixels with the same reference rasterizer. Nothing is drawn by the rasterizer: a miss, a wrong hit
or a missing barycentric weight fails the comparison.

Four more scenes use the same script by the name of their executable: rtscene.exe (two geometries, two instances with different transforms and
ids, and a compacted bottom level structure), rtanyhit.exe (an any hit shader cuts the triangle along the weight of its
second vertex and the miss shader gets the background from a callable shader) and rtprocedural.exe (one box, and an intersection shader makes
a yellow disc of radius one half of it).

The host driver has to offer VK_KHR_acceleration_structure and VK_KHR_ray_tracing_pipeline. Of the drivers on the development machine only
the software rasterizer does, so this test always runs on it; when the driver offers no ray tracing the test is skipped (exit code 77).

usage: native-d3d12-raytrace.py RAVEN RAYTRACE_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
from lavapipe import icd as lavapipe_icd


def check_disc(data):
    """The disc of radius one half, centred in the frame: yellow inside, the clear colour outside, a band of two pixels around its edge skipped."""
    if len(data) != 256 * 256 * 4:
        raise SystemExit(f"read back {len(data)} bytes, expected {256 * 256 * 4}")
    inside = outside = 0
    for y in range(256):
        for x in range(256):
            radius = (((x + 0.5) / 128 - 1) ** 2 + (1 - (y + 0.5) / 128) ** 2) ** 0.5
            if abs(radius - 0.5) * 128 < 2:
                continue
            got = tuple(data[(y * 256 + x) * 4:(y * 256 + x) * 4 + 4])
            expected = (255, 255, 0, 255) if radius < 0.5 else (26, 26, 76, 255)
            if max(abs(a - b) for a, b in zip(got, expected)) > 2:
                raise SystemExit(f"pixel {x},{y}: got {got}, expected {expected}")
            inside += radius < 0.5
            outside += radius >= 0.5
    print(f"disc matches the reference: {inside} interior pixels, {outside} clear pixels")


def check_scene(data):
    """Four solid triangles, each with the colour of its instance and geometry: red and green in the instance moved left, blue and yellow in
    the instance at half size moved right. A band of one and a half pixels around every edge is skipped."""
    if len(data) != 256 * 256 * 4:
        raise SystemExit(f"read back {len(data)} bytes, expected {256 * 256 * 4}")
    base = [[(-0.4, -0.4), (0.0, 0.4), (0.4, -0.4)], [(-0.4, 0.45), (0.0, 0.85), (0.4, 0.45)]]
    placed = [((-0.5, 0.0), 1.0), ((0.5, 0.0), 0.5)]
    colours = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]
    triangles = []
    for instance, (shift, scale) in enumerate(placed):
        for geometry, corners in enumerate(base):
            points = [((x * scale + shift[0] + 1) * 128, (1 - (y * scale + shift[1])) * 128) for x, y in corners]
            triangles.append((points, colours[instance * 2 + geometry] + (255,)))

    def distance(points, px, py):
        area = (points[1][0] - points[0][0]) * (points[2][1] - points[0][1]) - (points[2][0] - points[0][0]) * (points[1][1] - points[0][1])
        sign = 1 if area > 0 else -1
        nearest = None
        for i in range(3):
            (ax, ay), (bx, by) = points[i], points[(i + 1) % 3]
            length = ((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5
            value = sign * ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / length
            nearest = value if nearest is None else min(nearest, value)
        return nearest

    counts = [0] * 5
    for y in range(256):
        for x in range(256):
            px, py = x + 0.5, y + 0.5
            expected, slot, skip = (26, 26, 76, 255), 4, False
            for index, (points, colour) in enumerate(triangles):
                d = distance(points, px, py)
                if abs(d) <= 1.5:
                    skip = True
                elif d > 1.5:
                    expected, slot = colour, index
            if skip:
                continue
            got = tuple(data[(y * 256 + x) * 4:(y * 256 + x) * 4 + 4])
            if max(abs(a - b) for a, b in zip(got, expected)) > 2:
                raise SystemExit(f"pixel {x},{y}: got {got}, expected {expected}")
            counts[slot] += 1
    if min(counts) == 0:
        raise SystemExit(f"a triangle or the background was never checked: {counts}")
    print(f"scene matches the reference: {counts[:4]} pixels in the four triangles, {counts[4]} clear pixels")


def main():
    raven, workload, vkd3d, dxvk, work = map(Path, sys.argv[1:6])
    shutil.rmtree(work, ignore_errors=True)
    run, prefix = work / "run", work / "prefix"
    run.mkdir(parents=True)
    for name in ("d3d12.dll", "d3d12core.dll"):
        shutil.copy(vkd3d / name, run / name)
    shutil.copy(dxvk / "dxgi.dll", run / "dxgi.dll")
    stem = workload.stem
    shutil.copy(workload, run / f"{stem}.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / f"{stem}.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    drive = prefix / "drive_c"
    report = (drive / f"{stem}.txt").read_text() if (drive / f"{stem}.txt").exists() else "missing"
    if not result.returncode and report.startswith("SKIP"):
        print("skipped:", report)
        raise SystemExit(77)
    if result.returncode or report != "OK":
        raise SystemExit(f"ray tracing workload did not finish: exit {result.returncode}, report {report!r}, {result.stderr[-300:]!r}")
    if stem == "rtscene":
        check_scene((drive / f"{stem}.raw").read_bytes())
        compact, full = map(int, (drive / "rtscene_sizes.txt").read_text().split())
        if not 0 < compact <= full:
            raise SystemExit(f"the compacted size {compact} is not within the full size {full}")
        print(f"the bottom level structure was compacted from {full} to {compact} bytes and the picture came from the compacted copy")
        print("Direct3D 12 ray tracing (" + stem + ") used two geometries, two transformed instances and a compacted structure through vkd3d-proton and Raven's Vulkan bridge")
        return
    if stem == "rtprocedural":
        check_disc((drive / f"{stem}.raw").read_bytes())
        print("Direct3D 12 ray tracing (" + stem + ") ran an intersection shader through vkd3d-proton and Raven's Vulkan bridge and drew the disc")
        return
    arguments = ["256", "256", "26", "26", "76", "1.0", "0.5"] if stem == "rtanyhit" else []
    check = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("check-d3d11-triangle.py")), str(drive / f"{stem}.raw"), *arguments],
                           capture_output=True, text=True, timeout=120)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit(check.stderr.strip() or "the Direct3D 12 triangle does not match the reference")
    print("Direct3D 12 ray tracing (" + stem + ") ran through vkd3d-proton and Raven's Vulkan bridge and the traced triangle matches the reference")


if __name__ == "__main__":
    main()
