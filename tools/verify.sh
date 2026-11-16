#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# The build is universal on purpose. An arm64-only bundle builds and tests
# perfectly well here and then fails to load in an Intel Resolume, and the
# build log calls it a success either way -- so the architecture is checked
# with lipo, never with the log.
#
#     tools/verify.sh [BUILD_DIR]
#
# SKIP_SOFTWARE=1 leaves out the Faraday checks on Apple's software renderer
# (about twenty minutes); MUTANTS=1 adds the Faraday shader mutants (about
# eight). Both are part of the claim the README makes: skip them only while
# iterating.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$REPO/build-verify}"

cd "$REPO"

# oxbow, the FFGL test host. It sits beside this repo's checkout -- and from
# a git worktree `..` is the worktrees folder, not Projects/resolume, so the
# main checkout is found through git's common dir as well. OXBOW names the
# binary outright.
OXBOW_REPO=""
for candidate in "$REPO/../oxbow" \
                 "$(dirname "$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)")/../oxbow"; do
	if [ -d "$candidate/build" ]; then
		OXBOW_REPO="$candidate"
		break
	fi
done
OXBOW_REPO="${OXBOW_REPO:-$REPO/../oxbow}"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '\033[31mFAIL\033[0m %s\n' "$1"; exit 1; }

#---------------------------------------------------------------------------
# The GLSL reserved words, as identifiers.
#
# glslc catches these, but glslc is optional and a machine without it skips the
# whole shader step -- so the one trap most likely to be walked into gets its
# own grep that always runs. `sample` in particular is a natural name in a
# plugin that samples a surface, and `filter`, `patch` and `layout` are just as
# easy to reach for.
#
# It looks for a declaration, not a mention, so the words may still appear in
# prose above the code.
#---------------------------------------------------------------------------
reserved_words() {
	local words="patch sample input output filter common active half layout flat smooth noperspective"
	local bad=0 word

	for word in $words; do
		if grep -nE "(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|mat[234])[[:space:]]+$word[[:space:]]*[;=,)]" \
		            source/Shaders.cpp >/dev/null 2>&1; then
			printf '   "%s" is declared as an identifier and is a GLSL reserved word\n' "$word"
			grep -nE "(float|int|uint|bool|vec[234])[[:space:]]+$word[[:space:]]*[;=,)]" source/Shaders.cpp \
				| sed 's/^/      /'
			bad=$(( bad + 1 ))
		fi
	done

	[ "$bad" -eq 0 ] && printf '   none of the reserved words is used as an identifier\n'
	return "$bad"
}

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# Shaders the plugin assembles at run time. Millpond has none: every pass is
# one literal. Kept so a pass that grows into pieces has somewhere to go.
ASSEMBLED = {}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	# Adjacent string literals, joined: MSVC C2026 caps one literal at about
	# 16 KB, so a shader that outgrows it is split and has to be rejoined here.
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	# An int indexes the raw strings that are not assigned to a name, in source
	# order. A literal starts with #version. Anything else names a constant
	# above -- and a name that has moved is a KeyError here, not a silent skip.
	if isinstance( p, int ):       return unnamed[ p ]
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

#---------------------------------------------------------------------------
step "GLSL reserved words"
#---------------------------------------------------------------------------
reserved_words || fail "a GLSL reserved word is used as an identifier"

#---------------------------------------------------------------------------
step "Shaders"
#---------------------------------------------------------------------------
shaders_compile || fail "a shader does not compile"

#---------------------------------------------------------------------------
step "Demo shaders"
#---------------------------------------------------------------------------
# The browser demo's copy of the same GLSL.
#
# `demo/plugin.js` cannot include a C++ file, so it carries its own copy of
# every shader. This compares the two character for character -- reformatting
# counts, deliberately, because "it is only whitespace" is how a real change
# gets waved through. It says nothing about the demo's PORT of the CPU half;
# only a reader can check that.
#---------------------------------------------------------------------------
if [[ -f demo/tools/check_shaders.py ]]; then
	if python3 demo/tools/check_shaders.py >/tmp/millpond-demo-shaders.log 2>&1; then
		echo "ok   $( tail -1 /tmp/millpond-demo-shaders.log )"
	else
		tail -12 /tmp/millpond-demo-shaders.log
		fail "the demo's shaders have drifted -- see /tmp/millpond-demo-shaders.log"
	fi
else
	echo "   skipped: no demo/"
fi

#---------------------------------------------------------------------------
step "Submodule"
#---------------------------------------------------------------------------
if [[ ! -f external/ffgl/CMakeLists.txt ]]; then
	fail "FFGL SDK missing -- run: git submodule update --init --recursive"
fi
echo "ok   FFGL SDK present at $(git -C external/ffgl rev-parse --short HEAD)"

#---------------------------------------------------------------------------
step "Build (universal)"
#---------------------------------------------------------------------------
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)" >/dev/null
echo "ok   built"

#---------------------------------------------------------------------------
step "Bundle"
#---------------------------------------------------------------------------
bundle="$BUILD/Millpond.bundle"
binary="$bundle/Contents/MacOS/Millpond"

[[ -f "$binary" ]] || fail "no binary at $binary"

# Universal. The failure this catches ships a plugin that simply does not
# appear in half the Resolume installs it is given to.
arches="$(lipo -archs "$binary")"
[[ "$arches" == *arm64* ]]  || fail "no arm64 slice (got: $arches)"
[[ "$arches" == *x86_64* ]] || fail "no x86_64 slice (got: $arches)"

# The entry point. A bundle whose registration got dropped by the linker still
# loads and still exports this -- the OBJECT-library note in CMakeLists.txt is
# what actually guards the registration; this catches a build that produced no
# module at all.
# Captured, then matched from a herestring -- never `nm ... | grep -q`.
# Under `set -o pipefail` a `grep -q` that finds its match exits immediately,
# the writer upstream takes SIGPIPE, and the PIPELINE reports failure even
# though the symbol is there. It is output-size dependent, so it fires on the
# bigger binary first and looks intermittent. A herestring is not a pipeline,
# so nothing can SIGPIPE.
symbols=$( nm -gU "$binary" 2>/dev/null || true )
grep -q '_plugMain' <<<"$symbols" || fail "plugMain not exported"

echo "ok   Millpond: $arches, plugMain exported"

#---------------------------------------------------------------------------
step "Bundle metadata"
#---------------------------------------------------------------------------
plist="$bundle/Contents/Info.plist"
[[ -f "$plist" ]] || fail "no Info.plist in the bundle"

read_plist() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null || true; }

identifier="$( read_plist CFBundleIdentifier )"
executable="$( read_plist CFBundleExecutable )"
package="$( read_plist CFBundlePackageType )"
version="$( read_plist CFBundleVersion )"
declared="$( sed -n 's/^[[:space:]]*VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1 )"

[[ "$identifier" == "com.stoatworks.ffgl.millpond" ]] || fail "bundle id is '$identifier'"
[[ "$executable" == "Millpond" ]] || fail "CFBundleExecutable is '$executable'"
[[ "$package" == "BNDL" ]] || fail "CFBundlePackageType is '$package', not BNDL"
# The version drifts across the manifest, the plist and the About header more
# often than anything else in the fleet, so all three are compared rather than
# any one of them trusted.
[[ "$version" == "$declared" ]] || fail "plist version '$version' != CMakeLists '$declared'"
grep -q "versionFallback = \"v$declared\"" source/StoatworksAbout.h \
	|| fail "StoatworksAbout.h's versionFallback is not v$declared"

echo "ok   $identifier, $package, v$version -- plist, CMakeLists and About agree"

#---------------------------------------------------------------------------
step "Code signature"
#---------------------------------------------------------------------------
# Ad hoc, which is what a local build gets. The release workflow signs and
# notarises properly; this only proves the bundle is well enough formed to be
# signed at all, which a malformed one is not.
codesign --force --sign - --timestamp=none "$bundle" >/dev/null 2>&1 \
	|| fail "the bundle could not be ad-hoc signed"
codesign --verify --deep --strict "$bundle" >/dev/null 2>&1 \
	|| fail "the ad-hoc signature does not verify"
echo "ok   ad-hoc signed and verified"

#---------------------------------------------------------------------------
step "Host view"
#---------------------------------------------------------------------------
# What a host actually reads out of the bundle: the id, the name and the type.
# The FFGL name field is char[ 16 ] and is NOT null-terminated, so a long name
# is truncated silently and nothing in this repo would ever notice -- only
# something that reads the bundle the way a host does.
#
# oxbow lives in the fleet, not here, so this is a skip rather than a failure
# when it is not to hand.
OXBOW="${OXBOW:-$OXBOW_REPO/build/oxbow}"
if [[ -x "$OXBOW" ]]; then
	probe="$( "$OXBOW" probe "$bundle" 2>&1 || true )"
	printf '%s\n' "$probe" | sed 's/^/   /'
	grep -q 'MP01' <<<"$probe" || fail "oxbow did not read the id MP01"
	grep -q 'SW Millpond' <<<"$probe" || fail "oxbow did not read the name 'SW Millpond'"
	grep -qi 'effect' <<<"$probe" || fail "oxbow did not read the type as an effect"
	echo "ok   a host reads MP01 / SW Millpond / effect"
else
	echo "   skipped: no oxbow at $OXBOW (set OXBOW=...)"
fi

#---------------------------------------------------------------------------
step "Checks"
#---------------------------------------------------------------------------
# Every claim the README makes, in the order the README makes them.
"$BUILD/mptest" --fft
"$BUILD/mptest" --modes
"$BUILD/mptest" --gravity
"$BUILD/mptest" --quiet
"$BUILD/mptest" --shallow
"$BUILD/mptest" --banks
"$BUILD/mptest" --refraction
"$BUILD/mptest" --fresnel
"$BUILD/mptest" --caustics
"$BUILD/mptest" --still
"$BUILD/mptest" --skim-check
"$BUILD/mptest" --rain
"$BUILD/mptest" --audio

"$BUILD/mptest" --state

#---------------------------------------------------------------------------
step "Faraday"
#---------------------------------------------------------------------------
# The shaken tank, in the order the README makes its claims, at two rasters:
# the water is sized in metres, but the frame-dependent parts (the strobe's
# clock, a resize, the GL state, Shake 0 against far ends) are not.
FARADAY_CHECKS="mathieu onset half wavelength seed saturation pattern strobe clock resize faraday-off rate state-shaken"
for size in 320x180 1280x720; do
	for check in $FARADAY_CHECKS; do
		"$BUILD/mptest" --"$check" --size "$size"
	done
done

#---------------------------------------------------------------------------
step "Negative controls"
#---------------------------------------------------------------------------
# The checks above, run against a model that is deliberately wrong, and
# required to fail. A check that cannot fail is not a check.
"$BUILD/mptest" --negative
"$BUILD/mptest" --faraday-negative --size 320x180
"$BUILD/mptest" --faraday-negative --size 1280x720

#---------------------------------------------------------------------------
step "Faraday off is v0.1.0"
#---------------------------------------------------------------------------
# --faraday-off compares this build with itself. This compares it, bit for
# bit, with the build before Faraday existed: ten scenes, two rasters, the
# output, the surface and the caustics, and v0.1.0's parameter table.
bash tools/offcompare.sh "$BUILD"
bash tools/offcompare.sh --negative "$BUILD"

#---------------------------------------------------------------------------
step "Pipes"
#---------------------------------------------------------------------------
# --pipe and --film are how takes are filmed. A reader that stops early (head,
# an ffmpeg that has had enough) must end the harness with an error, exit 1,
# not kill it with SIGPIPE, 141. Under bash, read from PIPESTATUS -- and set
# +e / +o pipefail for the one pipeline that is meant to fail. The control:
# `yes`, which does not ignore SIGPIPE, must read 141 the same way.
frame=$(( 64 * 36 * 4 ))
bytes="$( "$BUILD/mptest" --film 3 --size 64x36 --set Shake=1 | wc -c | tr -d ' ' )"
[[ "$bytes" == "$(( 3 * frame ))" ]] || fail "--film 3 wrote $bytes bytes, not $(( 3 * frame ))"
bytes="$( /usr/bin/head -c $(( 2 * frame )) /dev/zero | "$BUILD/mptest" --pipe --size 64x36 --set Shake=1 | wc -c | tr -d ' ' )"
[[ "$bytes" == "$(( 2 * frame ))" ]] || fail "--pipe returned $bytes bytes for $(( 2 * frame ))"
set +e +o pipefail
"$BUILD/mptest" --film 200 --size 320x180 --set Shake=1 \
	| "$BUILD/mptest" --pipe --size 320x180 --set Shake=1 | /usr/bin/head -c 1 >/dev/null
statuses=( "${PIPESTATUS[@]}" )
yes | /usr/bin/head -c 1 >/dev/null
control=( "${PIPESTATUS[@]}" )
set -e -o pipefail
[[ "${control[0]}" == "141" ]] || fail "the control: yes | head read ${control[0]}, not 141 -- PIPESTATUS is not being read"
[[ "${statuses[0]}" == "1" && "${statuses[1]}" == "1" ]] \
	|| fail "mptest --film | mptest --pipe | head -c 1 exited ${statuses[*]}, not 1 1 0"
echo "ok   --film and --pipe carry whole frames shaken; | head -c 1 ends both with 1 (the control reads 141)"

#---------------------------------------------------------------------------
step "Dead controls"
#---------------------------------------------------------------------------
# The only thing that catches a uniform whose name does not match the C++.
python3 tools/sweep.py --build "$(basename "$BUILD")"

#---------------------------------------------------------------------------
step "Faraday on Apple's software renderer"
#---------------------------------------------------------------------------
# Would it hold on another rasteriser? The software renderer is the other one
# on every Mac, and its transcendentals are poor enough to have broken three
# of these checks before the passes stopped leaning on them (AGENTS.md).
#
# The software renderer runs each process on one core, and the longest check
# takes many minutes on it, so the checks run side by side and are reported
# in order once all have finished.
if [[ "${SKIP_SOFTWARE:-0}" == "1" ]]; then
	echo "   skipped: SKIP_SOFTWARE=1"
else
	swlogs="$(mktemp -d "${TMPDIR:-/tmp}/millpond-software.XXXXXX")"
	pids=()
	for check in $FARADAY_CHECKS; do
		MPTEST_RENDERER=software "$BUILD/mptest" --"$check" --size 320x180 >"$swlogs/$check.txt" 2>&1 &
		pids+=( "$!" )
	done
	swbad=""
	i=0
	for check in $FARADAY_CHECKS; do
		status=0
		wait "${pids[$i]}" || status=$?
		cat "$swlogs/$check.txt"
		[[ "$status" -eq 0 ]] || swbad="$swbad $check"
		i=$(( i + 1 ))
	done
	rm -rf "$swlogs"
	[[ -z "$swbad" ]] || fail "on Apple's software renderer:$swbad"
fi

#---------------------------------------------------------------------------
step "Faraday shader mutants"
#---------------------------------------------------------------------------
if [[ "${MUTANTS:-0}" == "1" ]]; then
	python3 tools/mutate.py
else
	echo "   skipped: set MUTANTS=1 (one-character mutants of the four Faraday shaders, about eight minutes)"
fi

#---------------------------------------------------------------------------
step "Cost"
#---------------------------------------------------------------------------
"$BUILD/mptest" --bench

printf '\n\033[32mall green\033[0m\n'
