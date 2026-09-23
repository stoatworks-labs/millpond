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
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$REPO/build-verify}"

cd "$REPO"

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
OXBOW="${OXBOW:-$HOME/Projects/resolume/oxbow/build/oxbow}"
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
step "Negative controls"
#---------------------------------------------------------------------------
# The checks above, run against a model that is deliberately wrong, and
# required to fail. A check that cannot fail is not a check.
"$BUILD/mptest" --negative

#---------------------------------------------------------------------------
step "Dead controls"
#---------------------------------------------------------------------------
# The only thing that catches a uniform whose name does not match the C++.
python3 tools/sweep.py --build "$(basename "$BUILD")"

#---------------------------------------------------------------------------
step "Cost"
#---------------------------------------------------------------------------
"$BUILD/mptest" --bench

printf '\n\033[32mall green\033[0m\n'
