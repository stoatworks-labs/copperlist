#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# Each check answers a question none of the others can:
#
#   shaders       does the shader compile, through a real GLSL compiler,
#                 before a host has to find out. A shader that will not
#                 compile presents to an operator as "the clip is
#                 black", with the real message buried in the log.
#   build         a fresh universal Release build, which is what ships
#   suites        the plugin's claims: the copper's grid and write order,
#                 the blitter's lines against an independent Bresenham, the
#                 bitplane count, the field arithmetic from Resolume's clock,
#                 replay against a jump, the 12-bit palette, the whole-pixel
#                 scroller, and the output being the chip's picture -- the
#                 GL ones at 1280x720 AND 320x180, each with its negative
#                 control
#   pipe          the fleet's --pipe frame format writes whole frames
#   sweep         does every control change the picture
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop
#                 while still producing a bundle that loads and exports
#                 plugMain
#   lipo          is the macOS build really universal, or did CMake latch
#                 the architecture list before -DCMAKE_OSX_ARCHITECTURES
#                 arrived and report success anyway
#   plist         does CFBundleExecutable name the binary that is actually
#                 on disk -- if it does not, codesign reports "code object
#                 is not signed at all" about a *nested* object and mentions
#                 neither the plist nor the cause
#   codesign      the exact command the release job runs, against a copy
#   oxbow         instantiation and frames in a host, and the name, id
#                 and TYPE a host reads. A source that registered as an
#                 effect would be handed an input it never asked for.
#   bench         the render cost, for the record. Not pass/fail -- there is
#                 no threshold worth asserting on somebody else's GPU -- but
#                 a verify run leaves a timing on the record, which is what
#                 turns "it feels slower" into a comparison.
#
# The last five are release-job work done locally on purpose. A check that
# only runs in CI, after a tag, is a check that will catch you after the tag.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# The shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V,
# which demands an explicit layout( location ) on every uniform and varying.
# Those are Vulkan rules and not GLSL ones, and without the flag every shader
# "fails" for reasons that have nothing to do with the code.
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

# A shader may be several adjacent raw strings (MSVC caps one literal at about
# 16 KB), so everything up to the terminating semicolon is joined. There is
# one short pass here, but the joining stays, because the day a string grows
# past the cap is not the day to discover the extraction only ever read the
# first half of it.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

for name, body in named.items():
	if not ( body.lstrip().startswith( "#version" ) and "void main" in body ):
		continue
	# The vertex shader is the one that writes gl_Position; everything else is
	# a fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )
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

	if [ "$n" -lt 2 ]; then
		# Fewer than the two shaders this repo has is a FAILURE, not a pass.
		# It means the extraction above has lost track of where the GLSL
		# lives, and a check that silently looks at nothing is worse than no
		# check at all.
		printf '   only %d shaders were extracted -- the extraction has gone stale\n' "$n"
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

#---------------------------------------------------------------------------
# A fresh universal Release build -- the one that ships. The dev build in
# build/ is arm64 only and is not what any of the binary checks below should
# be looking at.
#---------------------------------------------------------------------------
step "build"
if [ ! -d "$BUILD" ]; then
	cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
fi
if cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "universal Release build"
else
	fail "build failed -- run: cmake --build $BUILD"
	exit 1
fi

CPTEST="$BUILD/cptest"

step "suites"
for t in names copper line bitplanes fields replay palette scroll scaling; do
	if "$CPTEST" --$t >/tmp/copperlist-$t.txt 2>&1; then
		n=$(grep -c '^  ok' /tmp/copperlist-$t.txt)
		if [ "$n" -gt 0 ]; then pass "cptest --$t ($n assertions)"; else pass "cptest --$t"; fi
	else
		fail "cptest --$t, see /tmp/copperlist-$t.txt"
	fi
done

step "pipe"
bytes=$("$CPTEST" --pipe --size 64x36 --frames 3 2>/dev/null | wc -c | tr -d ' ')
if [ "$bytes" = "27648" ]; then
	pass "--pipe writes three whole 64x36 RGBA frames"
else
	fail "--pipe wrote $bytes bytes, expected 27648"
fi

step "sweep"
for size in 480x270 320x180; do
	if python3 tools/sweep.py --binary "$CPTEST" --size $size --jobs 4 >/tmp/copperlist-sweep.txt 2>&1; then
		pass "$size: $( tail -1 /tmp/copperlist-sweep.txt )"
	else
		echo "   *** dead controls, see /tmp/copperlist-sweep.txt"
		tail -4 /tmp/copperlist-sweep.txt | sed 's/^/   /'
		fail "tools/sweep.py reports a dead control at $size"
	fi
done

BUNDLE="$BUILD/Copperlist.bundle"
BIN="$BUNDLE/Contents/MacOS/Copperlist"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under
	# `set -o pipefail`: grep exits at once, nm takes SIGPIPE, and the
	# pipeline reports that failure. Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ "$ident" = "com.stoatworks.ffgl.copperlist" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Copperlist.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		# A source that lights pixels with no input and no file, so oxbow's
		# verdict is meaningful here.
		out=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$out" in
			*"FF_INSTANTIATE_GL failed"*) fail "instantiation failed -- see: $OXBOW selftest $BUNDLE" ;;
			*PASS*) pass "registers, instantiates and lights pixels" ;;
			*"id:"*) fail "registers but oxbow reports FAIL -- see: $OXBOW selftest $BUNDLE" ;;
			*) fail "oxbow did not recognise the bundle" ;;
		esac
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		case "$probe" in *"name:        SW Copperlist"*) pass "the host sees the name SW Copperlist" ;; *) fail "wrong or missing name" ;; esac
		case "$probe" in *"id:          CP01"*) pass "the host sees the id CP01" ;; *) fail "wrong or missing id" ;; esac
		case "$probe" in *"type:        source"*) pass "the host sees a SOURCE" ;; *) fail "wrong plugin type" ;; esac
		case "$probe" in *"inputs:      0..0"*) pass "and is told it takes no inputs" ;; *) fail "wrong input count" ;; esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

step "bench"
"$CPTEST" --bench 2>&1 | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
