#!/bin/bash
# Checks that the core is deterministic however it is run: for every game in a list, the recompiler
# against the exact CPU every frame (gamecmp PAIR=ref-armgba), with the renderer's threads as well as
# without; every frame's picture and sound the same with the renderer's two threads and the mixer's
# thread as without them (bench BENCH_FRAMES); and, given a second build, the same as that build's.
#
# usage: determinism.sh build bios.bin games.txt romdir frames [other-build] [jobs]
#   build        a build folder with gamecmp and bench (cmake -DMGBA_DIR=... -DMGBA_LIB=..., see README)
#   games.txt    one game a line, a ROM's name in romdir without .gba
#   other-build  e.g. main's: each frame's picture and sound hashes must be the same as this one's
# Prints one line a game and "ALL SAME" when nothing differs.

build=$1
bios=$2
list=$3
romdir=$4
frames=$5
other=$6
jobs=${7:-$(grep -c ^processor /proc/cpuinfo)}

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

check() {
	local game=$1 rom="$romdir/$1.gba" n=$2 r=""
	# The exact CPU and the recompiler, frame by frame: first with skipped BIOS and scripted input,
	# then booting through the BIOS, then with the picture drawn by two threads and the emulation
	# thread.
	for run in "1 1" "0 2" "1 3 threaded"; do
		set -- $run
		local env=()
		[ "$3" = threaded ] && env=(ARMGBA_THREADED=1 ARMGBA_WORKERS2=1)
		local res
		res=$(env PAIR=ref-armgba "${env[@]}" "$build/gamecmp" "$bios" "$rom" "$frames" "$1" "$2" 2>&1 | grep -v '^GBA\|^Status' | head -1)
		case "$res" in
		*"frames match"*) r="$r ok" ;;
		*) r="$r DIFF(skip $1 seed $2${3:+ $3}: $res)" ;;
		esac
	done
	# Every frame's picture and sound: alone, with the threads, and in the other build.
	BENCH_HASH=1 BENCH_FRAMES="$out/$n.a" "$build/bench" armgba "$bios" "$rom" "$frames" > /dev/null 2>&1
	BENCH_HASH=1 BENCH_FRAMES="$out/$n.t" ARMGBA_THREADED=1 ARMGBA_WORKERS2=1 ARMGBA_AUDIO_THREAD=1 \
		"$build/bench" armgba "$bios" "$rom" "$frames" > /dev/null 2>&1
	same() {
		if [ ! -s "$out/$n.a" ]; then
			r="$r NOFRAMES"
		elif cmp -s "$out/$n.a" "$2"; then
			r="$r $1-same"
		else
			r="$r DIFF($1 parts at frame $(diff "$out/$n.a" "$2" | grep -m1 '^<' | cut -d' ' -f2))"
		fi
	}
	same threads "$out/$n.t"
	if [ -n "$other" ]; then
		BENCH_HASH=1 BENCH_FRAMES="$out/$n.b" "$other/bench" armgba "$bios" "$rom" "$frames" > /dev/null 2>&1
		same other-build "$out/$n.b"
	fi
	echo "$game:$r" > "$out/$n.sum"
}

i=0
while IFS= read -r game; do
	[ -z "$game" ] && continue
	while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do
		wait -n
	done
	check "$game" "$(printf %03d $i)" &
	i=$((i + 1))
done < "$list"
wait

cat "$out"/*.sum
if grep -q 'DIFF\|NOFRAMES' "$out"/*.sum; then
	echo "$(grep -l 'DIFF\|NOFRAMES' "$out"/*.sum | wc -l) of $i games differ"
	exit 1
fi
echo "ALL SAME ($i games)"
