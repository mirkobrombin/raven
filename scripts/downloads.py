"""The pinned files the provisioning recipes download, fetched one way.

A file is kept where it belongs only once its checksum is the pinned one. Download servers answer with a gateway error, stall, or now and
then send a page or a cut file in place of the one asked for: each address gets a few spaced attempts, a copy that does not match counts as
a failed attempt, and the next address is tried after the last one. A missing file (an answer below 500) does not come back at that
address. A copy already in place is checked the same way and refused when it differs, unless the caller replaces it."""

import hashlib
from pathlib import Path
import time
import urllib.error
import urllib.request

ATTEMPTS = 4


def digest(path, algorithm="sha256"):
    # Read in blocks rather than through hashlib.file_digest, which the Python 3.9 of the portable build host does not have.
    state = hashlib.new(algorithm)
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            state.update(block)
    return state.hexdigest()


def _copy(url, partial, size):
    request = urllib.request.Request(url, headers={"Accept": "application/octet-stream"})
    received = 0
    with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as output:
        while chunk := response.read(1024 * 1024):
            received += len(chunk)
            if size is not None and received > size:
                raise ValueError(f"{url} sends more than the pinned {size} bytes")
            output.write(chunk)


def fetch(urls, archive, checksum, what, *, algorithm="sha256", size=None, replace=False):
    """Puts `what` at `archive` from the first of `urls` (one address or several holding the same file) that sends it with `checksum`."""
    archive = Path(archive)
    if archive.exists():
        if digest(archive, algorithm) == checksum:
            return archive
        if not replace:
            raise RuntimeError(f"{what} checksum differs: {archive} is incomplete or another version; delete it to download it again")
    urls = (urls,) if isinstance(urls, str) else tuple(urls)
    partial = archive.with_name(archive.name + ".part")
    failure = None
    try:
        for url in urls:
            for attempt in range(ATTEMPTS):
                try:
                    _copy(url, partial, size)
                    if digest(partial, algorithm) == checksum:
                        partial.replace(archive)
                        return archive
                    failure = RuntimeError(f"{url} sent {what} with another checksum")
                except urllib.error.HTTPError as error:
                    error.close()
                    failure = error
                    if error.code < 500:
                        break
                except (OSError, ValueError) as error:
                    failure = error
                if attempt + 1 < ATTEMPTS:
                    time.sleep(15 * (attempt + 1))
    finally:
        partial.unlink(missing_ok=True)
    raise RuntimeError(f"no address sent {what} with its pinned checksum (last: {failure})")
