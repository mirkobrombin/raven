#!/usr/bin/env python3
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request


FONT_PACKAGE = "http://archive.ubuntu.com/ubuntu/pool/main/f/fonts-liberation/fonts-liberation_2.1.5-3build1_all.deb"
FONT_PACKAGE_SHA256 = "47f156bc945cfc69cf2f2a0c19b8c82b67a685738d3846673462de6a73266e1b"
FONTS = {
    "LiberationSans-Regular.ttf": "075e71be4c58beda172ecb574a799308f04c3d054c6a5b7434954d3b0f1b28d8",
    "LiberationMono-Regular.ttf": "d18235f1e874771f4eaec3ddd6ddb4db849d334bdc6930c5c2185d9344834e62",
    "LiberationSerif-Regular.ttf": "350f4ffd773cae53560c63dca3d89a5070595614fde12f772bf43feb63d81064",
}
# The release asset through the API too, which keeps answering when the release download page times out.
DEJAVU_RELEASE = ("https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2",
                  "https://api.github.com/repos/dejavu-fonts/dejavu-fonts/releases/assets/2070169")
DEJAVU_RELEASE_SHA256 = "fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7"
# Raven Sans stands in for the dialog face Tahoma. It is DejaVu Sans Condensed with every advance narrowed so the weighted
# average width of Latin text matches Tahoma's, and with Tahoma's vertical metrics so a cell has the same height. The
# targets are the published figures of @capsizecss/metrics 4.4.0 (Tahoma: ascent 2049, descent 423, average 917 regular
# and 1044 bold on 2048 units); 937 and 1058 are the same average measured on the pinned DejaVu faces.
DIALOG_FACES = {
    "RavenSans-Regular.ttf": ("DejaVuSansCondensed.ttf", "Regular", 917 / 937),
    "RavenSans-Bold.ttf": ("DejaVuSansCondensed-Bold.ttf", "Bold", 1044 / 1058),
}
DIALOG_ASCENT = 2049
DIALOG_DESCENT = 423
SKIA_REPOSITORY = "https://skia.googlesource.com/skia.git"
SKIA_COMMIT = "a21aae83d6e4e3da284100a30f0a5eafc443b60f"
SKIA_ARGUMENTS = """is_official_build = true
is_debug = false
cc = "clang"
cxx = "clang++"
skia_enable_ganesh = false
skia_enable_graphite = false
skia_enable_pdf = false
skia_enable_skottie = false
skia_enable_svg = false
skia_enable_tools = false
skia_use_partition_alloc = false
skia_use_gl = false
skia_use_x11 = false
skia_use_fontconfig = false
skia_use_freetype = true
skia_use_harfbuzz = false
skia_use_icu = false
skia_use_expat = false
skia_use_piex = false
skia_use_wuffs = false
skia_use_libjpeg_turbo_decode = false
skia_use_libjpeg_turbo_encode = false
skia_use_libpng_decode = false
skia_use_libpng_encode = false
skia_use_libwebp_decode = false
skia_use_libwebp_encode = false
skia_use_zlib = false
skia_use_perfetto = false
skia_enable_fontmgr_empty = true
skia_enable_fontmgr_custom_empty = true
skia_use_system_freetype2 = true
"""


def ar_member(archive, prefix):
    at = 8
    while at + 60 <= len(archive):
        name = archive[at:at + 16].decode().strip()
        size = int(archive[at + 48:at + 58].decode())
        if name.startswith(prefix):
            return name, archive[at + 60:at + 60 + size]
        at += 60 + size + (size & 1)
    raise RuntimeError(f"the font package has no {prefix} member")


def download(urls, path):
    # Download servers answer with a gateway error or stall now and then; a few spaced attempts ride it out, then the next address is tried.
    # A missing file does not come back.
    urls = (urls,) if isinstance(urls, str) else urls
    for index, url in enumerate(urls):
        for attempt in range(4):
            try:
                request = urllib.request.Request(url, headers={"Accept": "application/octet-stream"})
                with urllib.request.urlopen(request, timeout=120) as response, open(path, "wb") as file:
                    shutil.copyfileobj(response, file)
                return
            except OSError as error:
                if isinstance(error, urllib.error.HTTPError) and error.code < 500:
                    raise
                if attempt == 3:
                    if index == len(urls) - 1:
                        raise
                    break
                time.sleep(15 * (attempt + 1))


def provision_fonts(output):
    fonts = output / "fonts"
    fonts.mkdir(exist_ok=True)
    package = output / "fonts-liberation.deb"
    if not package.exists():
        partial = package.with_suffix(".part")
        download(FONT_PACKAGE, partial)
        partial.replace(package)
    data = package.read_bytes()
    if hashlib.sha256(data).hexdigest() != FONT_PACKAGE_SHA256:
        raise RuntimeError("font package checksum differs; delete it to download it again")
    name, payload = ar_member(data, "data.tar")
    tarball = output / name
    tarball.write_bytes(payload)
    subprocess.run(["tar", "-xf", tarball, "-C", fonts, "--strip-components=6", "--wildcards", "*/Liberation*-Regular.ttf"], check=True)
    tarball.unlink()
    for font, digest in FONTS.items():
        if hashlib.sha256((fonts / font).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"font checksum differs: {font}")
    return fonts


def dialog_face(source, target, style, ratio):
    from fontTools.ttLib import TTFont

    font = TTFont(source, recalcTimestamp=False)
    metrics = font["hmtx"].metrics
    # Only the advances change: outlines, side bearings on the left and hinting stay as designed.
    for glyph, (advance, bearing) in metrics.items():
        metrics[glyph] = (round(advance * ratio), bearing)
    font["hhea"].ascent = DIALOG_ASCENT
    font["hhea"].descent = -DIALOG_DESCENT
    font["hhea"].lineGap = 0
    table = font["OS/2"]
    table.usWinAscent = table.sTypoAscender = DIALOG_ASCENT
    table.usWinDescent = DIALOG_DESCENT
    table.sTypoDescender = -DIALOG_DESCENT
    table.sTypoLineGap = 0
    # Version 1 of the table defines the average on lower-case letters, which narrow by the same ratio.
    table.xAvgCharWidth = round(table.xAvgCharWidth * ratio)
    font["hhea"].recalc(font)
    # The Bitstream Vera and Arev terms allow modified fonts only under names without their words; the notices stay.
    names = font["name"]
    full = "Raven Sans" if style == "Regular" else f"Raven Sans {style}"
    replaced = {1: "Raven Sans", 2: style, 3: f"Raven Sans {style} 2.37", 4: full, 5: "Version 2.37",
        6: f"RavenSans-{style}", 8: "Raven"}
    names.names = [record for record in names.names if record.nameID not in (16, 17)]
    for record in names.names:
        if record.nameID in replaced:
            record.string = replaced[record.nameID]
    names.setName("Derived from DejaVu Sans Condensed 2.37 with narrower advances and taller cells.", 10, 3, 1, 0x409)
    for word in ("Bitstream Vera", "Arev", "DejaVu Sans Condensed"):
        for record in names.names:
            if record.nameID in (1, 3, 4, 6) and word in str(record):
                raise RuntimeError(f"the dialog face is still named after {word}")
    font.save(target)


def provision_dialog_faces(output, fonts):
    archive = output / "dejavu-fonts-ttf-2.37.tar.bz2"
    if not archive.exists():
        partial = archive.with_suffix(".part")
        download(DEJAVU_RELEASE, partial)
        partial.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != DEJAVU_RELEASE_SHA256:
        raise RuntimeError("DejaVu release checksum differs; delete it to download it again")
    source = output / "dejavu"
    source.mkdir(exist_ok=True)
    # Python reads the bzip2 archive itself, so the host needs no bzip2 program.
    with tarfile.open(archive, "r:bz2") as release:
        for name in [f"ttf/{face}" for face, _, _ in DIALOG_FACES.values()] + ["LICENSE"]:
            member = release.extractfile(f"dejavu-fonts-ttf-2.37/{name}")
            (source / Path(name).name).write_bytes(member.read())
    for target, (face, style, ratio) in DIALOG_FACES.items():
        dialog_face(source / face, fonts / target, style, ratio)
    shutil.copyfile(source / "LICENSE", fonts / "RavenSans-LICENSE.txt")
    shutil.rmtree(source)


def provision_skia(output, jobs):
    source = output / "skia"
    if not source.exists():
        subprocess.run(["git", "init", "-q", source], check=True)
        subprocess.run(["git", "-C", source, "fetch", "-q", "--depth=1", SKIA_REPOSITORY, SKIA_COMMIT], check=True)
        subprocess.run(["git", "-C", source, "checkout", "-q", "FETCH_HEAD"], check=True)
        subprocess.run([sys.executable, "tools/git-sync-deps"], cwd=source, check=True)
    subprocess.run([sys.executable, "bin/fetch-gn"], cwd=source, check=True)
    subprocess.run([sys.executable, "bin/fetch-ninja"], cwd=source, check=True)
    build = output / "cpu-fonts"
    build.mkdir(exist_ok=True)
    (build / "args.gn").write_text(SKIA_ARGUMENTS)
    subprocess.run([source / "bin/gn", "gen", build], cwd=source, check=True)
    subprocess.run([source / "third_party/ninja/ninja", "-C", build, f"-j{jobs}", "skia"], check=True)
    return source, build / "libskia.a"


def main():
    parser = argparse.ArgumentParser(description="Provide the pinned Liberation fonts, the Raven Sans dialog faces and the CPU build of Skia that text in guest windows needs")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--fonts-only", action="store_true", help="only the fonts, as Skia takes a long time to build")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for tool in ("tar", "git", "clang", "clang++"):
        if not shutil.which(tool):
            raise RuntimeError(f"required tool is missing: {tool}")
    fonts = provision_fonts(output)
    provision_dialog_faces(output, fonts)
    if args.fonts_only:
        print(f"configure Raven with -DRAVEN_FONT_DIRECTORY={fonts}")
        return
    source, library = provision_skia(output, args.jobs)
    print(f"configure Raven with -DRAVEN_SKIA_SOURCE={source} -DRAVEN_SKIA_LIBRARY={library} -DRAVEN_FONT_DIRECTORY={fonts}")


if __name__ == "__main__":
    os.umask(0o022)
    main()
