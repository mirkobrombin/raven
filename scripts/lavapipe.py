"""The manifest of lavapipe, Mesa's software Vulkan driver, which tests and benchmarks run on when no GPU is asked for.

Debian and Ubuntu name it lvp_icd.json; Fedora and Arch add the architecture, lvp_icd.x86_64.json.
"""

from pathlib import Path

DIRECTORY = Path("/usr/share/vulkan/icd.d")


def icd():
    """The installed manifest, or the Debian path when none is installed, so callers can test exists()."""
    for name in ("lvp_icd.json", "lvp_icd.x86_64.json"):
        if (DIRECTORY / name).exists():
            return DIRECTORY / name
    return DIRECTORY / "lvp_icd.json"
