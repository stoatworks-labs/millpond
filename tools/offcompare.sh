#!/usr/bin/env bash
#
# With Shake at 0, is this build's every float the same as the last build
# before Faraday's?
#
#     tools/offcompare.sh [--negative] [BUILD_DIR] [BASE]
#
# Builds tools/offdump against BASE (default 64f9690: v0.1.0's plugin plus the
# PassBuffer depth-buffer fix, the commit the Faraday work branched from) from
# a git archive, builds it against this tree in BUILD_DIR (default build/), runs
# both at two rasters, and compares with cmp: the output picture, the surface
# and the caustic buffer of ten scenes, and v0.1.0's parameter table. This
# build also renders each scene with the other five Faraday controls at their
# far ends, which must match the base build too.
#
# Bit for bit, so no tolerance needs a source -- and it is a comparison
# between two processes on one GPU, which is deterministic. Not on Apple's
# software renderer, which is not deterministic across builds that schedule
# its threads differently.
#
# --negative is its negative control: the far variant sets Shake to 0.001,
# and the script then passes only if it finds differences.
#
set -euo pipefail

negative=0
if [[ "${1:-}" == "--negative" ]]; then
	negative=1
	shift
	export OFFDUMP_SHAKE=0.001
fi

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$REPO/build}"
BASE="${2:-64f9690}"
ARCH="$(uname -m)"

work="$(mktemp -d "${TMPDIR:-/tmp}/millpond-offcompare.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# The base tree. git archive leaves the submodule out: this tree's FFGL SDK
# stands in, which is the same commit unless the submodule moved.
mkdir -p "$work/src"
git -C "$REPO" archive "$BASE" | tar -x -C "$work/src"
mkdir -p "$work/src/external"
rm -rf "$work/src/external/ffgl"
ln -s "$REPO/external/ffgl" "$work/src/external/ffgl"
base_ffgl="$(git -C "$REPO" ls-tree "$BASE" external/ffgl | awk '{ print $3 }')"
here_ffgl="$(git -C "$REPO/external/ffgl" rev-parse HEAD)"
[[ "$base_ffgl" == "$here_ffgl" ]] || echo "   note: FFGL SDK is $here_ffgl here, $base_ffgl at $BASE"

# offdump, added to the base tree's own build. printf, not a heredoc.
{
	printf '\nadd_executable(offdump "%s/tools/offdump/main.cpp")\n' "$REPO"
	printf 'target_link_libraries(offdump PRIVATE millpond_core "-framework OpenGL" "-framework AppKit")\n'
} >>"$work/src/CMakeLists.txt"

build() {
	if ! "$@" >"$work/build.log" 2>&1; then
		tail -30 "$work/build.log"
		echo "FAIL could not build offdump: $*"
		exit 1
	fi
}
build cmake -S "$work/src" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCH"
build cmake --build "$work/build" -j"$(sysctl -n hw.ncpu)" --target offdump
build cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)" --target offdump

bad=0
files=0
for size in 320x180 1280x720; do
	mkdir -p "$work/old-$size" "$work/new-$size"
	"$work/build/offdump" "$work/old-$size" "$size" >/dev/null
	"$BUILD/offdump" "$work/new-$size" "$size" >/dev/null

	compare() {
		local old="$1" new="$2"
		files=$(( files + 1 ))
		if ! cmp -s "$old" "$new"; then
			bad=$(( bad + 1 ))
			[[ "$negative" -eq 1 ]] && return 0
			python3 -I -c '
import sys, array
a = array.array("f"); a.frombytes(open(sys.argv[1], "rb").read())
b = array.array("f"); b.frombytes(open(sys.argv[2], "rb").read())
n = sum(1 for x, y in zip(a, b) if x != y) + abs(len(a) - len(b))
print("   DIFFERS %s: %d of %d floats" % (sys.argv[3], n, max(len(a), len(b))))
' "$old" "$new" "$size $(basename "$new")" 2>/dev/null || echo "   DIFFERS $size $(basename "$new")"
		fi
	}

	if ! cmp -s "$work/old-$size/params.txt" "$work/new-$size/params.txt"; then
		bad=$(( bad + 1 ))
		echo "   DIFFERS: v0.1.0's parameter table"
		diff "$work/old-$size/params.txt" "$work/new-$size/params.txt" | sed 's/^/      /' || true
	fi
	files=$(( files + 1 ))

	for old in "$work/old-$size"/*.base.*; do
		name="$(basename "$old")"
		compare "$old" "$work/new-$size/$name"
		far="${name/.base./.far.}"
		if [[ -f "$work/new-$size/$far" ]]; then
			compare "$old" "$work/new-$size/$far"
		else
			bad=$(( bad + 1 ))
			echo "   MISSING $size $far"
		fi
	done
done

if [[ "$negative" -eq 1 ]]; then
	if [[ "$bad" -gt 0 ]]; then
		echo "ok   negative control: with Shake at 0.001, $bad of $files files differ from $BASE's build, as they must"
		exit 0
	fi
	echo "FAIL negative control: Shake at 0.001 changed nothing -- the comparison cannot fail"
	exit 1
fi

if [[ "$bad" -eq 0 ]]; then
	echo "ok   $files files at 320x180 and 1280x720 bit-identical to $BASE's build (Shake 0, other Faraday controls at defaults and at far ends)"
	exit 0
fi
echo "FAIL $bad of $files files differ from $BASE's build"
exit 1
