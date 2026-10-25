<p align="center"><img src="docs/logo.svg" alt="Raven" width="320"></p>

<p align="center"><b>A Windows your apps recognize.</b></p>

[Website and documentation](https://useraven.org)

Raven runs Windows applications on Linux. You install them with their own installers, the same ones you would use on a PC, and they start: no virtual machine, no winetricks, no DLL overrides.

The program's code runs directly on your CPU, inside a sandbox where it cannot reach Linux. Every call it makes to the system is answered by Raven, which plays the part of the Windows kernel and its services: processes and threads, files, the registry, windows and input. Graphics go to Wayland and Vulkan, with DXVK and vkd3d-proton for Direct3D.

## Requirements

- Linux on x86_64
- CMake 3.20 or newer, a C++20 compiler and Python 3.11 or newer
- MinGW-w64 for x86_64 and i686, to build the Windows test programs
- For desktop applications: Wayland, xkbcommon, pixman, FreeType and Vulkan

The tested setup also uses libpng 1.6.59, a CPU build of Skia with the Liberation fonts, DXVK and vkd3d-proton; the `scripts/provision-*.py` tools download the pinned versions.

## Installation

Raven is built from source for now:

```sh
git clone https://github.com/mirkobrombin/raven.git
cd raven
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j2
```

Then run an installer, or any program:

```sh
export RAVEN_PREFIX="$HOME/.local/share/raven/my-app"
build/native/raven --run-process --execution=native --timeout-ms=600000 --desktop=wayland setup.exe
```

`RAVEN_PREFIX` is the Windows installation the program sees, with its own `C:` drive and registry; give each application its own. Raven takes a snapshot of the prefix before every installer, and `build/native/raven --restore-prefix` brings it back.

To run the tests:

```sh
ctest --test-dir build --output-on-failure -j1
```

Some tests need software Raven cannot redistribute; point `RAVEN_MINESWEEPER`, `RAVEN_NETFX_INSTALLER`, `RAVEN_NETFX35_PACKAGE`, `RAVEN_XNA_INSTALLER` and `RAVEN_REDIST_DIRECTORY` at your own copies, or those tests are skipped. Each component under [native/upstream](native/upstream) records where it comes from and its license.

## What works today

| Area | Status |
|---|---|
| 32 and 64-bit programs | Both, side by side in the same prefix |
| Installers | NSIS, Inno Setup and Windows Installer (MSI) packages, with custom actions, rollback and elevation |
| Visual C++ 2005 to 2022 | x86 and x64, alone or all together, reinstalled, older over newer |
| .NET Framework 4.8 | Installs from Microsoft's offline installer; console, WinForms and WPF programs run |
| .NET Framework 3.5 | Enabled from its package as Windows does; CLR 2.0 programs run |
| DirectX June 2010, XNA 4.0, OpenAL | Install from their official redistributables |
| Direct3D 8 and 9 | Through DXVK, including 32-bit programs |
| Direct3D 11 | Through DXVK: every shader stage, compute, MSAA, deferred contexts |
| Direct3D 12 | Through vkd3d-proton, including ray tracing |
| Side-by-side assemblies | Manifests, WinSxS and activation contexts |

Raven's own system libraries stay in charge even when an installer writes its own kernel32 or user32, while the redistributables an installer brings are the ones that load.

## Tested applications

Each application is installed with its own installer into a fresh prefix and then started. Every stage that works adds 25%: install, start, main window, and a real workflow where one is automated.

| Application | Install | Start | Main window | Workflow | Score |
|---|---|---|---|---|---|
| Notepad++ 8.7.1 | yes | yes | yes | editing | 100% |
| 7-Zip 24.09 (EXE installer) | yes | yes | yes | not automated yet | 75% |
| 7-Zip 24.09 (MSI) | yes | yes | yes | not automated yet | 75% |
| Audacity 3.6.4 | yes | yes | yes | not automated yet | 75% |
| KeePass 2.57 | yes | yes | no | not automated yet | 50% |
| VLC 3.0.21 | yes | no | no | not automated yet | 25% |
| ClassiCube 1.3.8 | portable | yes | yes | singleplayer | 75% |
| Minesweeper | portable | yes | yes | gameplay | 75% |

KeePass needs the .NET Framework 4.8, which a fresh prefix does not have yet. The Notepad++ workflow saves a file, closes and reopens it, appends to it, uses the clipboard and switches theme. `scripts/benchmark-app-matrix.py` runs the whole table again.

## Benchmarks

The same programs on the same machine, against Wine 11.0 and Soda 11.0-32-experimental.

![Process completion time](docs/benchmark-cli.svg)

![Time until the main window appears](docs/benchmark-apps.svg)

![Direct3D 11 frame rate](docs/benchmark-frames.svg)

ClassiCube 1.3.8, fixed seed 1, four alternating rounds:

| Runtime | Median FPS | Samples (FPS) |
|---|---:|---|
| Raven | 71.5 | 72, 73, 54, 71 |
| Wine 11.0 | 63.0 | 65, 68, 61, 61 |
| Soda 11.0-32 exp. | 63.0 | 64, 64, 61, 62 |

These replace the earlier ClassiCube series; its scene configuration was not verified against the retained samples. Raven uses its default execution mode.

| Component | Value |
|---|---|
| CPU | Intel Core i7-13700H (14 cores, 20 threads, up to 5.0 GHz); 8 logical CPUs available to the benchmark environment |
| Memory | 31 GiB |
| Kernel | Linux 7.2.11 |
| Userland | Ubuntu 26.04.1 LTS |
| Graphics | Mesa 26.0.8 lavapipe (software Vulkan), two rendering threads |
| Display | labwc 0.9.3, headless, one isolated instance per run |

The workloads: a program that exits at once, a SQLite query over eight million rows, a Direct3D 11 triangle checked pixel by pixel, 2,000 Direct3D 11 frames at 256 by 256, the time to the main window of Notepad++ and of the ClassiCube launcher, and ClassiCube 1.3.8 itself (Direct3D 11 build, world seed 1, 640 by 480, view distance 128, frames counted from the fifth to the fifteenth second after the world loads).

Every runtime starts from a copy of a prefix it initialised beforehand. Each workload runs three times and the charts show the median; ClassiCube runs four rounds alternating the runtimes, so a change in machine load weighs on both. The Soda Direct3D 11 triangle did not finish within its 600-second limit. Software Vulkan keeps the numbers reproducible anywhere; a hardware GPU raises them for every runtime.

The [earlier chart data](docs/benchmarks.json), with CPU time and memory, remains available; its ClassiCube entry is superseded by the table above. The scripts `scripts/benchmark-wine.py`, `benchmark-apps.py`, `benchmark-frames.py` and `benchmark-classicube.py` run these workloads. The ClassiCube runner groups trials by runtime; use separate one-trial runs to alternate runtimes.

## License

Raven's Linux runtime is licensed under [GPL-3.0-or-later](LICENSE). Its own guest adapters listed in [LICENSES.json](LICENSES.json) remain MIT-licensed. Imported and derived components keep their own licenses and notices (GPL, LGPL, MIT, BSD, libpng, Unicode and Skia). The bitmap font from Go Mono carries the [Bigelow & Holmes notice](pkg/wl/LICENSE-Go-Mono). Earlier MIT grants remain valid.

The Linux `raven` executable incorporates GPL UTF-8 conversion code and LGPL window-rectangle code. [LICENSES.md](LICENSES.md) records the component boundaries and distribution requirements; [packaging](docs/packaging.md) describes how to include the corresponding sources and notices with a build.
