#!/usr/bin/env python3
"""Check that the audio APIs put the right sound on the sound server, and record the right sound from it.

usage: check-audio-tones.py WORKDIR NAME=RAVEN [NAME=RAVEN ...]

Builds testdata/native_loader/tone_client.c for both widths and runs each of its modes under every RAVEN given. A playback mode plays
one second of a tone into a null sink whose monitor is recorded; a recording mode records one second from a second null sink that is
fed a 700 Hz tone. Each result is checked for its frequency, length, gaps and share of the energy at that frequency. The mix mode
plays two DirectSound buffers at once, 1000 Hz panned left and 1500 Hz panned right, and checks each channel for its own tone.

Needs a PulseAudio or PipeWire server (pactl, parecord, paplay), MinGW-w64 for both widths and numpy. It loads two null sinks and
unloads them when it ends. A playback result is "silent" when the program ran but nothing reached the sink.
"""

from pathlib import Path
import math
import os
import re
import struct
import subprocess
import sys
import time
import wave

import numpy

FLAGS = ["-minline-all-stringops", "-O2", "-nostdlib", "-fno-stack-protector", "-fno-builtin", "-mno-stack-arg-probe"]
LIBRARIES = ["-ldsound", "-lwinmm", "-lole32", "-luuid", "-luser32", "-lkernel32", "-lgcc"]
COMPILERS = {64: ("x86_64-w64-mingw32-gcc", "raven_entry"), 32: ("i686-w64-mingw32-gcc", "_raven_entry")}
# Mode, and the tone expected on the left and right channel.
PLAYBACK = [("wasapi", 440, 440), ("waveout", 660, 660), ("dsound", 880, 880), ("playsound", 550, 550), ("mix", 1000, 1500)]
RECORDING = ["cap-wasapi", "cap-wavein", "cap-dsound"]
FED = 700
RATE = 48000


def measure(samples, rate, frequency):
    """The dominant frequency, seconds of sound, silent 10 ms windows inside it, share of energy near `frequency` and peak in dBFS."""
    window = rate // 100
    count = len(samples) // window
    peaks = numpy.abs(samples[:count * window].reshape(count, window)).max(axis=1)
    loud = numpy.flatnonzero(peaks > 0.01)
    if not len(loud):
        return None
    first, last = loud[0], loud[-1] + 1
    active = samples[first * window:last * window]
    spectrum = numpy.abs(numpy.fft.rfft(active * numpy.hanning(len(active)))) ** 2
    bins = numpy.fft.rfftfreq(len(active), 1 / rate)
    near = (bins > frequency * 0.97) & (bins < frequency * 1.03)
    return {"frequency": float(bins[spectrum.argmax()]), "seconds": (last - first) / 100, "gaps": int((peaks[first:last] <= 0.01).sum()),
            "share": float(spectrum[near].sum() / spectrum.sum()), "peak": 20 * math.log10(float(numpy.abs(active).max()))}


def verdict(found, frequency):
    if not found:
        return "silent"
    good = (abs(found["frequency"] - frequency) < frequency * 0.02 and 0.9 <= found["seconds"] <= 1.2 and not found["gaps"]
            and found["share"] > 0.9)
    return ("ok" if good else "WRONG") + " {frequency:.0f} Hz, {seconds:.2f} s, {gaps} gaps, {share:.0%} at tone, {peak:.1f} dBFS".format(**found)


def run(name, raven, program, arguments, work):
    # Each build gets a prefix of its own: a build installs its components there and another build would load them.
    environment = os.environ | {"PULSE_SINK": "tone_out", "PULSE_SOURCE": "tone_mic.monitor", "RAVEN_PREFIX": str(work / f"prefix-{name}")}
    environment.pop("RAVEN_AUDIO", None)
    try:
        result = subprocess.run([raven, "--run-process", "--execution=native", "--timeout-ms=30000", str(program), "--", *arguments],
                                env=environment, capture_output=True, text=True, timeout=60, cwd=work)
    except subprocess.TimeoutExpired:
        return "timed out"
    output = result.stdout + result.stderr
    returned = re.search(r"Native entry returned (\d+)", output)
    if returned and returned.group(1) == "0":
        return None
    missing = re.search(r"unimplemented native[^\n]*", output)
    return missing.group(0) if missing else f"step {returned.group(1)}" if returned else "no result: " + output.strip()[-120:]


def main():
    work = Path(sys.argv[1]).resolve()
    ravens = [(name, str(Path(raven).resolve())) for name, raven in (item.split("=", 1) for item in sys.argv[2:])]
    if not ravens:
        sys.exit(f"usage: {sys.argv[0]} WORKDIR NAME=RAVEN...")
    work.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent.parent / "testdata/native_loader/tone_client.c"
    programs = {}
    for width, (compiler, entry) in COMPILERS.items():
        programs[width] = work / f"tone{width}.exe"
        subprocess.run([compiler, *FLAGS, f"-Wl,--entry,{entry},--no-insert-timestamp", "-o", programs[width], source, *LIBRARIES], check=True)
    feed = work / "feed.wav"
    with wave.open(str(feed), "wb") as file:
        file.setnchannels(1); file.setsampwidth(2); file.setframerate(RATE)
        file.writeframes(struct.pack(f"<{RATE * 4}h", *(int(16000 * math.sin(2 * math.pi * FED * index / RATE)) for index in range(RATE * 4))))
    modules = []
    rows = []
    try:
        for sink in ("tone_out", "tone_mic"):
            modules.append(subprocess.run(["pactl", "load-module", "module-null-sink", f"sink_name={sink}"], check=True, capture_output=True,
                                          text=True).stdout.strip())
        for name, raven in ravens:
            for width, program in programs.items():
                for mode, left, right in PLAYBACK:
                    recording = work / f"{name}-{width}-{mode}.wav"
                    recorder = subprocess.Popen(["parecord", "--device=tone_out.monitor", "--file-format=wav", "--format=s16le",
                                                 f"--rate={RATE}", "--channels=2", "--latency-msec=20", str(recording)])
                    time.sleep(0.5)
                    failure = run(name, raven, program, [mode], work)
                    time.sleep(1)
                    recorder.terminate(); recorder.wait()
                    if failure:
                        rows.append((name, width, mode, "fails: " + failure))
                        continue
                    with wave.open(str(recording)) as file:
                        samples = numpy.frombuffer(file.readframes(file.getnframes()), dtype="<i2").reshape(-1, 2) / 32768
                    results = [verdict(measure(samples[:, channel], RATE, tone), tone) for channel, tone in ((0, left), (1, right))]
                    rows.append((name, width, mode, results[0] if left == right and results[0] == results[1] else
                                 f"left {results[0]}; right {results[1]}"))
                for mode in RECORDING:
                    recording = work / f"{name}-{width}-{mode}.raw"
                    recording.unlink(missing_ok=True)
                    player = subprocess.Popen(["paplay", "--device=tone_mic", str(feed)])
                    time.sleep(0.5)
                    failure = run(name, raven, program, [mode, "Z:" + str(recording).replace("/", "\\")], work)
                    player.terminate(); player.wait()
                    if failure or not recording.exists():
                        rows.append((name, width, mode, "fails: " + (failure or "no file written")))
                        continue
                    data = recording.read_bytes()
                    rate, channels, bits, is_float = struct.unpack("<4I", data[:16])
                    kind = "<f4" if is_float else "<i2"
                    samples = numpy.frombuffer(data[16:], dtype=kind).reshape(-1, channels)[:, 0] / (1 if is_float else 32768)
                    rows.append((name, width, mode, verdict(measure(samples, rate, FED), FED)))
    finally:
        for module in modules:
            subprocess.run(["pactl", "unload-module", module])
    print("| Build | Width | Mode | Result |\n|---|---|---|---|")
    for row in rows:
        print("| {} | {} | {} | {} |".format(*row))
    return 0 if all((row[3].startswith("ok") or row[3].startswith("left ok") and "; right ok" in row[3]) and "WRONG" not in row[3] for row in rows if row[0] == ravens[-1][0]) else 1


if __name__ == "__main__":
    sys.exit(main())
