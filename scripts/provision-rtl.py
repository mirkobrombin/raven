#!/usr/bin/env python3
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request

from component_licenses import mark_modified, preserve_notices


REVISION = "dacf2a6c47fa562799f9c8a41ab6d5c558556558"
ARCHIVE_SHA256 = "5373bbbb60541cb2a49e0acb53360e1218d4f5d56c31f9187b341b9fc16daa69"


def main():
    parser = argparse.ArgumentParser(description="Build the pinned selective ReactOS guest RTL component")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(output), TMP=str(output), TEMP=str(output))
    repo = Path(__file__).resolve().parents[1]
    archive = args.archive.resolve() if args.archive else output / "reactos-source.tar.gz"
    if not archive.exists():
        urllib.request.urlretrieve(f"https://codeload.github.com/reactos/reactos/tar.gz/{REVISION}", archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise RuntimeError("ReactOS source archive checksum differs")
    source = output / "source"
    prefixes = ("sdk/include/", "sdk/lib/rtl/", "sdk/lib/pseh/include/", "dll/ntdll/include/", "sdk/tools/hpp/")
    selected = {"COPYING", "COPYING.LIB", "COPYING.ARM", "dll/ntdll/rtl/libsupp.c", "dll/ntdll/ldr/verifier.c",
                "sdk/lib/crt/mem/memset.c", "sdk/lib/crt/mem/memmove.c", "sdk/lib/crt/string/wcslen.c", "sdk/lib/crt/string/tcslen.h"}
    with tarfile.open(archive) as upstream:
        for member in upstream:
            parts = Path(member.name).parts[1:]
            name = "/".join(parts)
            if not member.isfile() or not (name.startswith(prefixes) or name in selected or ("/" not in name and name.startswith("COPYING"))):
                continue
            if not parts or ".." in parts or Path(name).is_absolute():
                raise RuntimeError("invalid upstream archive path")
            target = source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(upstream.extractfile(member).read())
    generated = output / "generated"
    objects = output / "objects"
    generated.mkdir(exist_ok=True)
    objects.mkdir(exist_ok=True)
    records = []

    def run(command, cwd=None):
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
        record = dict(command=command, cwd=str(cwd) if cwd else None, returncode=result.returncode,
                      stdout=result.stdout, stderr=result.stderr)
        if result.returncode:
            identity = hashlib.sha256(json.dumps(command).encode()).hexdigest()[:12]
            (output / ("failed-command-" + identity + ".json")).write_text(json.dumps(record, indent=2) + "\n")
            raise RuntimeError(result.stderr or "RTL build command failed")
        return record

    for tool in ("clang", "x86_64-w64-mingw32-gcc", "widl"):
        if not shutil.which(tool):
            raise RuntimeError(f"required RTL build tool is missing: {tool}")
    records.append(run(["clang", "-O2", str(source / "sdk/tools/hpp/hpp.c"), "-o", str(output / "hpp")]))
    for name in ("winnt", "ntdef", "devioctl"):
        records.append(run([str(output / "hpp"), name + ".template.h", str(generated / (name + ".h"))], source / "sdk/include/xdk"))
    idls = set()

    def idl(name):
        if name in idls:
            return
        idls.add(name)
        path = source / "sdk/include/psdk" / (name + ".idl")
        if not path.exists():
            raise RuntimeError(f"pinned IDL is missing: {name}")
        for dependency in re.findall(r'import\s+"([^"]+)\.idl"', path.read_text()):
            idl(dependency)
        records.append(run(["widl", "-m64", "-h", "-H", str(generated / (name + ".h")),
                            "-I", str(path.parent), str(path)]))

    for name in ("objidl", "oleidl", "urlmon", "propidl", "wtypes", "objidlbase"):
        idl(name)
    functions = []

    def extract(path, names, header):
        raw = path.read_text()
        text = raw[:raw.index("*/") + 2] + "\n\n" + header + "\n\n"
        for name in names:
            match = re.search(r'(?:KPROCESSOR_MODE|PPEB|NTSTATUS|BOOLEAN|VOID|PVOID|ULONG)\s+NTAPI\s+' + name + r'\s*\(', raw)
            if not match:
                raise RuntimeError(f"pinned RTL function is missing: {name}")
            start = match.start()
            end = raw.index("{", match.end()) + 1
            depth = 1
            while depth:
                depth += (raw[end] == "{") - (raw[end] == "}")
                end += 1
            body = raw[start:end] + "\n"
            functions.append(dict(source=path.relative_to(source).as_posix(), name=name,
                                  start_line=raw[:start].count("\n") + 1, end_line=raw[:end].count("\n") + 1,
                                  sha256=hashlib.sha256(body.encode()).hexdigest()))
            text += body + "\n"
        return text

    support = extract(source / "dll/ntdll/rtl/libsupp.c",
                      ["RtlpGetMode", "RtlGetCurrentPeb", "RtlpSetHeapParameters", "RtlGetNtGlobalFlags"], "#include <ntdll.h>")
    (output / "support.c").write_text(support)
    verifier_path = source / "dll/ntdll/ldr/verifier.c"
    raw = verifier_path.read_text()
    declarations = raw[raw.index("BOOL AVrfpInitialized"):raw.index("VOID\nNTAPI\nAVrfReadIFEO")]
    verifier = extract(verifier_path, ["AVrfInternalHeapFreeNotification"], "#include <ntdll.h>\n#include <reactos/verifier.h>")
    (output / "verifier.c").write_text(verifier.replace("\nVOID\nNTAPI\nAVrfInternalHeapFreeNotification",
                                                      "\n" + declarations + "\nVOID\nNTAPI\nAVrfInternalHeapFreeNotification"))
    # Critical sections wait on the process-wide keyed event, keyed by their address, as on Windows since Vista: an event handle of
    # their own would be closed by a program that closes a stale handle value, and the lock would then fail to wake its waiters.
    critical = (source / "sdk/lib/rtl/critical.c").read_text()
    creation = """        Status = NtCreateEvent(&hNewEvent,
                               EVENT_ALL_ACCESS,
                               NULL,
                               SynchronizationEvent,
                               FALSE);"""
    deletion = "    if (CriticalSection->LockSemaphore)\n    {\n        /* In case NtClose fails, return the status */"
    if creation not in critical or deletion not in critical:
        raise SystemExit("ReactOS critical.c changed: the keyed event patch no longer applies")
    critical = critical.replace(creation, "        Status = STATUS_NOT_SUPPORTED;")
    critical = critical.replace(deletion, "    if (CriticalSection->LockSemaphore && CriticalSection->LockSemaphore != INVALID_HANDLE_VALUE)\n    {\n"
                                          "        /* In case NtClose fails, return the status */")
    (output / "critical.c").write_text(critical)
    # A heap created with HEAP_CREATE_ENABLE_EXECUTE holds code, as the .NET runtime's compiler puts its stubs there: its blocks are committed
    # executable, as on Windows. ReactOS commits every heap page read-write.
    heap = (source / "sdk/lib/rtl/heap.c").read_text()
    commit = "MEM_COMMIT,\n"
    for function, flags in (("RtlpFindAndCommitPages(PHEAP Heap,", "Heap->Flags"), ("RtlpExtendHeap(PHEAP Heap,", "Heap->Flags"),
                            ("RtlCreateHeap(ULONG Flags,", "Flags"), ("RtlAllocateHeap(IN PVOID HeapPtr,", "Heap->Flags")):
        start = heap.find("\n" + function)
        at = heap.find(commit, start) if start >= 0 else -1
        protection = heap.find("PAGE_READWRITE);", at) if at >= 0 else -1
        if protection < 0 or heap[at + len(commit):protection].strip():
            raise SystemExit(f"ReactOS heap.c changed: the executable heap patch no longer applies to {function}")
        heap = heap[:protection] + f"(({flags}) & HEAP_CREATE_ENABLE_EXECUTE ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE));" + \
            heap[protection + len("PAGE_READWRITE);"):]
    (output / "heap.c").write_text(heap)
    (output / "unicode.c").write_text(extract(source / "sdk/lib/rtl/unicode.c", ["RtlInitUnicodeString"], "#include <rtl.h>"))
    for name in ("support.c", "verifier.c", "critical.c", "heap.c", "unicode.c"):
        mark_modified(output / name)
    includes = [generated, source / "sdk/lib/pseh/include", source / "sdk/include/vcruntime", source / "sdk/include/ndk",
                source / "sdk/include/psdk", source / "sdk/include/crt", source / "sdk/include/ddk",
                source / "sdk/include", source / "sdk/include/reactos", source / "sdk/lib/rtl", source / "dll/ntdll/include"]
    flags = ["-c", "-O2", "-ffreestanding", "-ffunction-sections", "-fdata-sections", "-fgnu89-inline", "-mcx16",
             "-Wno-incompatible-pointer-types", "-D_NTOSKRNL_", "-DNO_RTL_INLINES", "-D_M_AMD64", "-D__REACTOS__",
             f"-ffile-prefix-map={source}=reactos", f"-ffile-prefix-map={repo}=raven", f"-ffile-prefix-map={output}=component"]
    paths = [output / "critical.c", output / "heap.c"] + [source / "sdk/lib/rtl" / (name + ".c") for name in
             ("heapuser", "heapdbg", "heappage", "bitmap", "bitmap64", "mem", "slist", "error", "appverifier", "avltable", "splaytree")]
    paths += [source / "sdk/lib/crt/mem/memset.c", source / "sdk/lib/crt/mem/memmove.c", source / "sdk/lib/crt/string/wcslen.c",
              output / "support.c", output / "verifier.c", output / "unicode.c", repo / "native/upstream/reactos-rtl/initialize.c",
              repo / "native/upstream/reactos-rtl/kernel.c"]

    def compile(path):
        definitions = [] if path.name in ("support.c", "verifier.c") else ["-D_NTSYSTEM_", "-D_NTDLLBUILD_"]
        return run(["x86_64-w64-mingw32-gcc", *flags, *definitions, *["-I" + str(path) for path in includes],
                    str(path), "-o", str(objects / (path.stem + ".o"))])

    with ThreadPoolExecutor(max_workers=2) as pool:
        records.extend(pool.map(compile, paths))
    records.append(run(["x86_64-w64-mingw32-gcc", "-shared", "-nostdlib", "-Wl,--exclude-all-symbols,--gc-sections,--entry,0,--image-base,0x1a1000000,--no-insert-timestamp",
                        "-Wl,--out-implib," + str(output / "raven-rtl.lib"), "-o", str(output / "raven-rtl.dll"),
                        *[str(objects / (path.stem + ".o")) for path in paths], str(repo / "native/upstream/reactos-rtl/raven-rtl.def"), "-lntdll", "-lgcc"]))
    result = dict(revision=REVISION, archive_sha256=ARCHIVE_SHA256, functions=functions, commands=records,
                  license_notices=preserve_notices(source, output),
                  component_sha256=hashlib.sha256((output / "raven-rtl.dll").read_bytes()).hexdigest(),
                  source_hashes={path.relative_to(source).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in source.rglob("*") if path.is_file()})
    (output / "provenance.json").write_text(json.dumps(result, indent=2) + "\n")
    print(output / "raven-rtl.dll")


if __name__ == "__main__":
    main()
