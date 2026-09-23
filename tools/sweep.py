#!/usr/bin/env python3
"""Move every parameter and fail if any of them made no difference to the frame.

**This is the only thing in the repo that catches a dead control**, and it is
not a theoretical risk. A GLSL uniform whose name does not match the C++ is
silently ignored -- `glGetUniformLocation` returns -1 and `glUniform` on -1 is
a documented no-op -- so a slider can be stone dead while everything compiles,
links, loads and renders. The measurement checks will not catch it either: they
only ever exercise the settings they set themselves.

## The context table

Millpond is driven by events, so a third of its controls do nothing until
something happens: the pebble's place, size and scatter matter only once a
pebble has been dropped, the skim's four only once a stone has been thrown,
and the two audio amounts only with audio playing. The table below supplies
exactly that and nothing more, as raw harness arguments -- `--drop 20` presses
Drop on frame 20, `--skim 20` throws a stone, `--beat` feeds a beat every half
second into the Audio buffer. An entry whose absence changes nothing is worse
than no entry: it makes the sweep look more careful than it is, so the table
was checked by emptying it: all eleven go dead without it.

Everything else is live on the base render, which is the plugin's own defaults
plus a harder rain -- ripples everywhere, so the light, the banks, the depth
and the constants all have water to act on.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose]
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

# Applied to every render: rain hard enough that 1.5 s of it has put ripples
# over most of the frame.
BASE = ["Rain=0.45"]

# What else has to be true for a parameter to be able to do anything, as raw
# harness arguments.
DROP = ["--drop", "20"]
SKIM = ["--skim", "20"]
BEAT = ["--beat"]
CONTEXT = {
    "Pebble X": DROP,
    "Pebble Y": DROP,
    "Pebble Size": DROP,
    "Splash": DROP,
    "Scatter": DROP,
    "Heading": SKIM,
    "Throw Speed": SKIM,
    "Skim Angle": SKIM,
    "Bounces": SKIM,
    "Audio Pebbles": BEAT,
    "Audio Rain": BEAT,
}

# The values every non-option parameter is swept across. The awkward numbers
# are load-bearing: a parameter the picture is periodic in can land 0, 0.5 and
# 1 on pixel-identical frames and report a working slider as dead. 0.137 and
# 0.611 are not rational multiples of anything swept here -- which matters more
# than usual for Angle Offset and Cross Angle, both of which are angles.
SWEEP_VALUES = [0.0, 0.137, 0.611, 1.0]

# Option parameters are swept across their elements instead. --list reports a
# parameter's kind but not its element count, so these track the enums in
# Controls.h by hand.
OPTION_RANGE = {
    "Banks": 2,
    "Detail": 4,
    "View": 4,
}

# Parameter kinds with no scalar worth sweeping.
SKIP_KINDS = {"buffer", "event", "text"}


def read_png(path):
    """Decode a PNG to raw bytes. Enough of the format for our own writer's
    output -- 8-bit RGBA, one IDAT, filter 0 on every row."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    pos = 8
    idat = b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IDAT":
            idat += body
        pos += 12 + length

    return zlib.decompress(idat)


def render(harness, out, settings, raw, verbose):
    args = [str(harness), "--out", str(out), "--size", "480x270", "--frames", "90"]
    args += raw
    for setting in settings:
        args += ["--set", setting]

    if verbose:
        print("   ", " ".join(args))

    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"mptest failed: {result.stderr.strip()}")

    return read_png(out)


def parameters(harness):
    """Name and kind of every parameter, in declaration order."""
    result = subprocess.run([str(harness), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"mptest --list failed: {result.stderr.strip()}")

    found = []
    for line in result.stdout.splitlines()[1:]:
        # id, name (may contain spaces), kind, default
        parts = line.split()
        if len(parts) < 4:
            continue
        kind = parts[-2]
        name = " ".join(parts[1:-2])
        found.append((name, kind))

    # The About block -- a text line then one browser button per link -- is
    # declared last and never touches a pixel, so sweeping it only buries a
    # real dead control. Truncating at "About" rather than naming each button
    # keeps this right as links are added; publishing a user guide adds one.
    for index, (entry_name, _kind) in enumerate(found):
        if entry_name == "About":
            return found[:index]

    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    harness = REPO / args.build / "mptest"
    if not harness.exists():
        print(f"no mptest at {harness} -- build first", file=sys.stderr)
        return 2

    dead = []
    checked = 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)

        for name, kind in parameters(harness):
            if kind in SKIP_KINDS:
                continue

            extra = CONTEXT.get(name, [])
            base = BASE

            if name in OPTION_RANGE:
                values = [float(i) for i in range(OPTION_RANGE[name])]
            else:
                values = SWEEP_VALUES

            frames = []
            for value in values:
                out = tmp / "sweep.png"
                frames.append(
                    render(harness, out, base + [f"{name}={value}"], extra, args.verbose))

            checked += 1
            if all(f == frames[0] for f in frames[1:]):
                dead.append(f"{name} ({kind})")
                print(f"  DEAD {name}")
            elif args.verbose:
                print(f"  ok   {name}")

    print()
    if dead:
        print(f"sweep: {checked} parameters, {len(dead)} made no difference:")
        for entry in dead:
            print(f"  - {entry}")
        return 1

    print(f"sweep: {checked} parameters, all live")
    return 0


if __name__ == "__main__":
    sys.exit(main())
