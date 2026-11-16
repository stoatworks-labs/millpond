#!/usr/bin/env python3
"""One-character mutants of the Faraday shaders, each against the Faraday checks.

    tools/mutate.py [--checks mathieu,onset,...] [--only ID,ID] [--size WxH]

A check that cannot tell the shader it measures from one with a `+` where the
`-` should be is not measuring that shader. So each mutant below changes ONE
character of one of the four Faraday shaders (source/Shaders.cpp), the plugin
is rebuilt with it, and every Faraday check runs against it under a time
limit. A mutant is caught if any check fails or runs out of time (a mutant
that makes the water blow up can make a check crawl rather than fail). One
that every check passes is a survivor, and is either equivalent -- no
observable change, said so below -- or a hole in the checks.

It works on a copy of the tree in a temporary directory, never in place: a
mutant left behind in source/ by an interrupted run would ship. The copy's
Shaders.cpp object is deleted before every build, because make compares mtimes
to the second and two mutants written within one second would otherwise share
an object.

The table records the result as of the last run; AGENTS.md has it too.
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parent.parent

# Run in this order: the cheap ones first.
CHECKS = ["mathieu", "half", "wavelength", "seed", "saturation", "rate", "clock", "resize",
          "state-shaken", "strobe", "pattern", "onset"]

# Seconds a check may take before it counts as caught. The GPU runs each in
# under 4 s at 320x180; this is a guard against a mutant that hangs, never a
# pass/fail threshold on speed.
LIMIT = 120

# (id, shader, old, new). `old` must occur exactly once in that shader's
# literal and `new` must differ from it in exactly one character.
MUTANTS = [
    # a. the envelope
    ("E1", "kFaradayEnvelopeShader", "state.y / OmegaC", "state.y * OmegaC"),
    ("E2", "kFaradayEnvelopeShader", "state.x * state.x + rate", "state.x * state.x - rate"),
    ("E3", "kFaradayEnvelopeShader", "RateScale * state.y", "RateScale * state.z"),
    # c. the pattern kernel
    ("K1", "kFaradayKernelShader", "return 1.0 - exp", "return 1.0 + exp"),
    ("K2", "kFaradayKernelShader", "( k - centre )", "( k + centre )"),
    ("K3", "kFaradayKernelShader", "notch( k, 1.41421356237", "notch( k, 2.41421356237"),
    ("K4", "kFaradayKernelShader", "notch( k, 1.73205080757", "notch( k, 2.73205080757"),
    ("K5", "kFaradayKernelShader", "if( Pattern == 1 )", "if( Pattern == 0 )"),
    ("K6", "kFaradayKernelShader", "Norm * kernel", "Norm + kernel"),
    # e. the saturating state
    ("S1", "kFaradayStateShader", "EnvelopeTexture, cell, 0 ).x", "EnvelopeTexture, cell, 0 ).y"),
    ("S2", "kFaradayStateShader", "SatRate * envelope", "SatRate + envelope"),
    ("S3", "kFaradayStateShader", "state.y * keep", "state.y / keep"),
    ("S4", "kFaradayStateShader", "-1.0 + z * ( 0.5", "-1.0 + z * ( 0.6"),
    # 3'. the shaken evolve
    ("V1", "kFaradayEvolveShader", "k * k + WallDamping", "k * k - WallDamping"),
    ("V2", "kFaradayEvolveShader", "Accel * k * depthFactor", "Accel / k * depthFactor"),
    ("V3", "kFaradayEvolveShader", "0.144337567 * h", "0.244337567 * h"),
    ("V4", "kFaradayEvolveShader", "omega2 - q * drive.x", "omega2 + q * drive.x"),
    ("V5", "kFaradayEvolveShader", "omega2 - q * drive.y", "omega2 - q * drive.x"),
    ("V6", "kFaradayEvolveShader", "( w1 + w2 )", "( w1 - w2 )"),
    ("V7", "kFaradayEvolveShader", "corr * ( w2 - w1 )", "corr * ( w2 + w1 )"),
    ("V8", "kFaradayEvolveShader", "gamma * gamma ) - d * d", "gamma * gamma ) + d * d"),
    ("V9", "kFaradayEvolveShader", "x * ( -1.0 / 2.0", "x * ( -1.0 / 3.0"),
    ("V10", "kFaradayEvolveShader", "x * ( -1.0 / 6.0", "x * ( -1.0 / 7.0"),
    ("V11", "kFaradayEvolveShader", "abs( disc ) < 1.0", "abs( disc ) < 9.0"),
    ("V12", "kFaradayEvolveShader", "decay * cs.x", "decay / cs.x"),
    ("V13", "kFaradayEvolveShader", "decay * cs.y / psi", "decay * cs.y * psi"),
    ("V27", "kFaradayEvolveShader", "r2 * ( 1.0 / 120.0", "r2 * ( 1.0 / 130.0"),
    ("V28", "kFaradayEvolveShader", "return vec2( -s, c )", "return vec2( +s, c )"),
    ("V14", "kFaradayEvolveShader", "exp( psi - gamma * h )", "exp( psi + gamma * h )"),
    ("V15", "kFaradayEvolveShader", "0.5 * ( slow + fast )", "0.5 * ( slow - fast )"),
    ("V16", "kFaradayEvolveShader", "( d + gamma * h ) * eta", "( d - gamma * h ) * eta"),
    ("V17", "kFaradayEvolveShader", "2.0 * gamma * d", "3.0 * gamma * d"),
    ("V18", "kFaradayEvolveShader", "( gamma * h + d ) * rate", "( gamma * h - d ) * rate"),
    ("V19", "kFaradayEvolveShader", "ivec2( i, 0 )", "ivec2( i, 1 )"),
    ("V20", "kFaradayEvolveShader", "for( int i = 0;", "for( int i = 1;"),
    ("V21", "kFaradayEvolveShader", "expNeg( gamma * h )", "expNeg( gamma / h )"),
    ("V22", "kFaradayEvolveShader", "rate / RateScale", "rate * RateScale"),
    ("V23", "kFaradayEvolveShader", "vec2( diff.y, -diff.x )", "vec2( diff.y, +diff.x )"),
    ("V24", "kFaradayEvolveShader", "A2.x - B2.y", "A2.x + B2.y"),
    ("V25", "kFaradayEvolveShader", ") - kv.y * A2", ") + kv.y * A2"),
    ("V26", "kFaradayEvolveShader", "cell.x == GridSize.x / 2", "cell.x == GridSize.x / 3"),
    # 3'. the seed
    ("D1", "kFaradayEvolveShader", "SeedAmp > 0.0", "SeedAmp < 0.0"),
    ("D2", "kFaradayEvolveShader", "SeedAmp * sqrt", "SeedAmp / sqrt"),
    ("D3", "kFaradayEvolveShader", "exp( -0.25 * band", "exp( -0.05 * band"),
    ("D4", "kFaradayEvolveShader", "ivec2( cell.y, 0 ), 0 ).xy * vec2( 1.0, -1.0 )", "ivec2( cell.y, 0 ), 0 ).xy * vec2( 1.0, +1.0 )"),
    ("D7", "kFaradayEvolveShader", "hermitian( ivec2( opposite.x, cell.y ) )", "hermitian( ivec2( opposite.x, cell.x ) )"),
    ("D5", "kFaradayEvolveShader", "( first ? 1.0 : -1.0 )", "( first ? 1.0 : +1.0 )"),
    ("D6", "kFaradayEvolveShader", "( k - Wavenumber )", "( k + Wavenumber )"),
]


def literal_span(text, symbol):
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', text, re.S)
    if match is None:
        raise SystemExit(f"mutate: {symbol} not found in Shaders.cpp")
    return match.start(1), match.end(1)


def apply(text, shader, old, new):
    if len(old) != len(new) or sum(1 for a, b in zip(old, new) if a != b) != 1:
        raise SystemExit(f"mutate: '{old}' -> '{new}' is not a one-character change")
    start, end = literal_span(text, shader)
    body = text[start:end]
    if body.count(old) != 1:
        raise SystemExit(f"mutate: '{old}' occurs {body.count(old)} times in {shader}, not once")
    return text[:start] + body.replace(old, new) + text[end:]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build(work):
    for obj in (work / "build").rglob("Shaders.cpp.o"):
        obj.unlink()
    result = run(["cmake", "--build", str(work / "build"), "-j", str(os.cpu_count() or 4), "--target", "mptest"])
    return result.returncode == 0, result.stdout + result.stderr


def check(work, name, size):
    try:
        result = run([str(work / "build" / "mptest"), "--" + name, "--size", size], timeout=LIMIT)
    except subprocess.TimeoutExpired:
        return "timeout"
    return "pass" if result.returncode == 0 else "fail"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checks", default=",".join(CHECKS))
    parser.add_argument("--only", default="")
    parser.add_argument("--size", default="320x180")
    args = parser.parse_args()
    checks = [c for c in args.checks.split(",") if c]
    only = {m for m in args.only.split(",") if m}
    mutants = [m for m in MUTANTS if not only or m[0] in only]

    with tempfile.TemporaryDirectory(prefix="millpond-mutate.") as tmp:
        work = pathlib.Path(tmp)
        # The working tree as it is, uncommitted changes included, and the
        # submodule by link.
        files = run(["git", "-C", str(REPO), "ls-files"]).stdout.split()
        for f in files:
            if f.startswith("external/") or not (REPO / f).is_file():
                continue
            dest = work / "src" / f
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / f, dest)
        (work / "src" / "external").mkdir(parents=True, exist_ok=True)
        (work / "src" / "external" / "ffgl").symlink_to(REPO / "external" / "ffgl")

        arch = os.uname().machine
        result = run(["cmake", "-S", str(work / "src"), "-B", str(work / "build"),
                      "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_OSX_ARCHITECTURES={arch}"])
        if result.returncode != 0:
            print(result.stdout + result.stderr)
            return 2

        shaders = work / "src" / "source" / "Shaders.cpp"
        pristine = shaders.read_text()
        for m in MUTANTS:
            apply(pristine, m[1], m[2], m[3])  # every entry valid before anything runs

        ok, log = build(work)
        if not ok:
            print(log[-3000:])
            return 2
        baseline = {c: check(work, c, args.size) for c in checks}
        if any(v != "pass" for v in baseline.values()):
            print("mutate: the unmutated build does not pass:", baseline)
            return 2
        print(f"unmutated: all {len(checks)} checks pass at {args.size}\n")

        survivors = []
        for ident, shader, old, new in mutants:
            shaders.write_text(apply(pristine, shader, old, new))
            started = time.time()
            ok, _log = build(work)
            if not ok:
                print(f"{ident:4s} {shader[8:-6]:9s} '{old}' -> '{new}': does not build (C++)")
                continue
            caught = []
            for c in checks:
                verdict = check(work, c, args.size)
                if verdict != "pass":
                    caught.append(c if verdict == "fail" else c + " (timeout)")
            took = time.time() - started
            if caught:
                print(f"{ident:4s} {shader[8:-6]:9s} '{old}' -> '{new}': caught by {', '.join(caught)}  [{took:.0f} s]")
            else:
                survivors.append(ident)
                print(f"{ident:4s} {shader[8:-6]:9s} '{old}' -> '{new}': SURVIVED  [{took:.0f} s]")
            sys.stdout.flush()
        shaders.write_text(pristine)

    print(f"\nmutants: {len(mutants)}, caught {len(mutants) - len(survivors)}, survived {len(survivors)}"
          + (f": {', '.join(survivors)}" if survivors else ""))
    return 0 if not survivors else 1


if __name__ == "__main__":
    sys.exit(main())
