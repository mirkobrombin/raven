import hashlib
from pathlib import Path
import shutil


def mark_modified(path):
    path = Path(path)
    notice = b"/* Modified for Raven on 2026-10-25; see the source patches and provisioning script. */\n"
    data = path.read_bytes()
    if not data.startswith(notice):
        path.write_bytes(notice + data)


def mark_patched_sources(source, patches):
    for patch in patches:
        for line in patch.read_text().splitlines():
            if not line.startswith("+++ b/"):
                continue
            name = line[6:].split()[0]
            path = source / name
            if not path.resolve().is_relative_to(source.resolve()):
                raise RuntimeError(f"patch path leaves the source directory: {name}")
            mark_modified(path)


def preserve_notices(source, output):
    source, output = Path(source), Path(output)
    for name in ("COPYING", "COPYING.LIB"):
        if not (source / name).is_file():
            raise RuntimeError(f"upstream license text is missing: {source / name}")
    notices = {}
    for path in sorted(source.rglob("*")):
        if not path.is_file() or not path.name.upper().startswith(("COPYING", "LICENSE", "COPYRIGHT", "NOTICE", "README", "AUTHORS")):
            continue
        relative = path.relative_to(source)
        target = output / "licenses" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        notices[relative.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return notices
