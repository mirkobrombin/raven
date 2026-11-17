#!/usr/bin/env python3

import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

# The files that carry a project's license terms, which a release keeps with the libraries built from it.
NOTICES = ("LICENSE", "LICENCE", "COPYING", "NOTICE", "AUTHORS", "COPYRIGHT")


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def fetch(url, target, size=None):
    partial = target.with_suffix(target.suffix + ".partial")
    created = False
    try:
        with partial.open("xb") as output:
            created = True
            received = 0
            with urllib.request.urlopen(url, timeout=45) as response:
                while chunk := response.read(1024 * 1024):
                    received += len(chunk)
                    if size is not None and received > size:
                        raise RuntimeError(f"download exceeds its pinned size: {url}")
                    output.write(chunk)
        partial.rename(target)
    finally:
        if created:
            partial.unlink(missing_ok=True)


def pack(tree, root, archive):
    """A source tree as a gzip tar that does not depend on when or by whom it was made, without the version control data."""
    def clean(info):
        if "/.git" in "/" + info.name.split("/", 1)[-1] or info.name.endswith("/.git"):
            return None
        info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, "", ""
        return info
    partial = archive.with_suffix(archive.suffix + ".partial")
    with partial.open("wb") as raw, tarfile.open(fileobj=raw, mode="w:gz", format=tarfile.PAX_FORMAT) as output:
        for path in sorted(tree.rglob("*")):
            if ".git" in path.relative_to(tree).parts:
                continue
            info = clean(output.gettarinfo(str(path), f"{root}/{path.relative_to(tree).as_posix()}"))
            if info is None:
                continue
            if info.isfile():
                with path.open("rb") as data:
                    output.addfile(info, data)
            else:
                output.addfile(info)
    partial.rename(archive)


def source_material(destination, component, package):
    """The source of the libraries a release ships, with their license notices: DXVK from its tag's archive, vkd3d-proton from its
    tag's commit with every submodule it builds with, which the tag's archive leaves out."""
    pinned = package.get("source")
    if not pinned:
        return None
    archive = destination / f'{component}-{package["version"]}-source.tar.gz'
    notices = destination / "licenses" / component
    if "url" in pinned:
        if not archive.exists():
            fetch(pinned["url"], archive)
        if digest(archive) != pinned["sha256"]:
            raise RuntimeError(f"source archive checksum mismatch: {component}")
    elif not archive.exists():
        work = Path(tempfile.mkdtemp(prefix=".tmp-", dir=destination))
        try:
            tree = work / component
            subprocess.run(["git", "init", "-q", str(tree)], check=True)
            subprocess.run(["git", "-C", str(tree), "fetch", "-q", "--depth=1", pinned["repository"], pinned["commit"]], check=True)
            subprocess.run(["git", "-C", str(tree), "checkout", "-q", "FETCH_HEAD"], check=True)
            head = subprocess.run(["git", "-C", str(tree), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()
            if head != pinned["commit"]:
                raise RuntimeError(f"source commit differs: {component}")
            # The submodules' commits are recorded in the pinned commit, so the whole tree follows from it.
            subprocess.run(["git", "-C", str(tree), "submodule", "update", "-q", "--init", "--recursive"], check=True)
            pack(tree, f'{component}-{package["version"]}', archive)
        finally:
            shutil.rmtree(work, ignore_errors=True)
    if not notices.exists():
        with tarfile.open(archive, "r:gz") as source:
            for member in source:
                parts = Path(member.name).parts
                if member.isfile() and (parts[-1].split(".")[0].upper() in NOTICES or "LICENSES" in parts[:-1]):
                    target = notices / Path(*parts[1:])
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(member) as data:
                        target.write_bytes(data.read())
    return {"component": component, "version": package["version"], "archive": archive.name, "sha256": digest(archive),
            **pinned}


def provision(destination, manifest):
    destination.mkdir(parents=True, exist_ok=True)
    installed = []
    for component, package in manifest.items():
        archive = destination / package["archive"]
        if not archive.exists():
            partial = archive.with_suffix(archive.suffix + ".partial")
            created = False
            try:
                with partial.open("xb") as output:
                    created = True
                    received = 0
                    with urllib.request.urlopen(package["url"], timeout=45) as response:
                        while chunk := response.read(1024 * 1024):
                            received += len(chunk)
                            if received > package["size"]:
                                raise RuntimeError(f"archive exceeds pinned size: {component}")
                            output.write(chunk)
                if digest(partial) != package["sha256"]:
                    raise RuntimeError(f"archive checksum mismatch: {component}")
                partial.rename(archive)
            finally:
                if created:
                    partial.unlink(missing_ok=True)
        if digest(archive) != package["sha256"]:
            raise RuntimeError(f"cached archive checksum mismatch: {component}")

        expected = {
            f'{package["archive_root"]}/{folder}/{dll}': (architecture, dll)
            for architecture, folder in package["architectures"].items()
            for dll in package["dlls"]
        }
        if archive.suffix == ".zst":
            decoded = subprocess.run(["zstd", "-dc", str(archive)], check=True, capture_output=True)
            source = tarfile.open(fileobj=io.BytesIO(decoded.stdout), mode="r:")
        else:
            source = tarfile.open(archive, mode="r:gz")
        with source:
            found = set()
            for member in source:
                if member.name not in expected:
                    continue
                if not member.isfile() or member.name in found:
                    raise RuntimeError(f"invalid DLL archive entry: {member.name}")
                architecture, dll = expected[member.name]
                with source.extractfile(member) as data:
                    content = data.read()
                actual = hashlib.sha256(content).hexdigest()
                target = destination / component / architecture / dll
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    if digest(target) != actual:
                        raise RuntimeError(f"cached DLL differs: {target}")
                else:
                    with target.open("xb") as output:
                        output.write(content)
                installed.append({"component": component, "version": package["version"],
                                  "architecture": architecture, "path": str(target.relative_to(destination)),
                                  "sha256": actual})
                found.add(member.name)
            if found != expected.keys():
                raise RuntimeError(f"missing DLLs in {component}: {sorted(expected.keys() - found)}")
        print(f'{component} {package["version"]}: {len(found)} DLLs verified')
    (destination / "installed.json").write_text(json.dumps(installed, indent=2) + "\n")
    sources = [material for component, package in manifest.items() if (material := source_material(destination, component, package))]
    (destination / "provenance.json").write_text(json.dumps(dict(
        binaries=[{key: package[key] for key in ("version", "url", "sha256")} | {"component": component} for component, package in manifest.items()],
        sources=sources, installed=installed), indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description="Provision pinned Raven graphics DLLs without installing into a prefix")
    parser.add_argument("--destination", type=Path, required=True)
    arguments = parser.parse_args()
    manifest_path = Path(__file__).resolve().parent.parent / "native/graphics/backends.json"
    try:
        provision(arguments.destination.resolve(), json.loads(manifest_path.read_text()))
    except (OSError, RuntimeError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, f"graphics provisioning failed: {error}\n")


if __name__ == "__main__":
    main()
