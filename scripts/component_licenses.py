from pathlib import Path


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
