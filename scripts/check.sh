#!/usr/bin/env bash
# Pre-merge checks: builds and tests every target in parallel, and fails if any of them fails.
#
#   scripts/check.sh            Run every check
#   scripts/check.sh <name>...  Run only the named checks
#   scripts/check.sh --list     List the checks
#
# Checks:
#   desktop          Release build of the player and tests (GCC), then the decoder tests
#   desktop-clang    The same with Clang
#   desktop-asan     Decoder tests built with AddressSanitizer and UndefinedBehaviorSanitizer
#   desktop-pico-options  Decoder tests with the Pico's decoder options: mono audio, MPEG-1 only, no diagnostic messages
#   desktop-float    Decoder tests with floating point audio and the full IDCT
#   pico-host        FAT32 and display driver tests on the desktop (pico/pico.sh test)
#   pico-asm         The Pico's Cortex-M0+ assembler run in an emulator against the decoder's C (tests/asm_check.py)
#   pico             Pico (RP2040) build of every app
#   pico2            Pico 2 (RP2350) build of every app
#   pico-displays    Pico build of the players and display test for every other display module and a rotation
#   fuzz             Fuzz the demuxer, decoders and .IFO parser with libFuzzer for FUZZ_SECONDS (default 60). The
#                    corpus is kept in build/check/fuzz/corpus between runs; crashing inputs go in
#                    build/check/fuzz/crashes. Add the smallest input for each new bug to tests/fuzz_regressions,
#                    which the desktop checks replay.
#
# Needs cmake, ninja, g++, clang++, ffmpeg, SDL3, Docker and Python's unicorn module. Builds go into build/check and pico/build/check-*;
# each check's output is in build/check/logs. Hardware-only tests (pico asm_test, the benchmark) aren't run.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/check
LOGS=$OUT/logs
IMAGE=md_mpeg_pico
ALL=(desktop desktop-clang desktop-asan desktop-pico-options desktop-float pico-host pico-asm pico pico2 pico-displays fuzz)

if [ "${1:-}" = --list ]; then
	printf '%s\n' "${ALL[@]}"
	exit 0
fi
CHECKS=("$@")
if [ ${#CHECKS[@]} -eq 0 ]; then
	CHECKS=("${ALL[@]}")
fi
for check in "${CHECKS[@]}"; do
	if ! printf '%s\n' "${ALL[@]}" | grep -qx -- "$check"; then
		echo "Unknown check $check, expected one of: ${ALL[*]}" >&2
		exit 2
	fi
done

needs_docker=0
for check in "${CHECKS[@]}"; do
	case "$check" in pico*) needs_docker=1 ;; esac
done
missing=()
for tool in cmake ninja g++ clang++ ffmpeg; do
	command -v "$tool" > /dev/null || missing+=("$tool")
done
if [ $needs_docker = 1 ]; then
	command -v docker > /dev/null || missing+=(docker)
fi
for check in "${CHECKS[@]}"; do
	if [ "$check" = pico-asm ] && ! python3 -c 'import unicorn' 2> /dev/null; then
		missing+=("Python unicorn module (python-unicorn)")
	fi
done
# Without ffmpeg the decoder tests skip themselves, which would pass silently
if [ ${#missing[@]} -gt 0 ]; then
	echo "Missing: ${missing[*]}" >&2
	exit 2
fi

rm -rf "$LOGS"
mkdir -p "$LOGS"

# Desktop build and decoder tests: desktop_check <build dir> <targets> <cmake options>...
desktop_check() {
	local dir=$OUT/$1 targets=$2
	shift 2
	cmake -S "$ROOT" -B "$dir" -G Ninja "$@" &&
		cmake --build "$dir" --target $targets &&
		ctest --test-dir "$dir" --output-on-failure --no-tests=error
}

in_docker() {
	docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT:/src" -w /src "$IMAGE" "$@"
}

# The size of clip pico/pico.sh embeds, so the build checks the flash still fits
CLIP_BYTES=1250000
# Any MPEG-1 clip of the right size; the build only embeds it
make_clip() {
	local clip=$OUT/clip.mpg
	if [ ! -f "$clip" ]; then
		ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc2=size=240x136:rate=24 -f lavfi -i sine=frequency=440 \
			-t 30 -c:v mpeg1video -b:v 2M -c:a mp2 -ar 32000 -f mpeg "$clip.tmp" &&
			head -c "$CLIP_BYTES" "$clip.tmp" > "$clip" && rm "$clip.tmp"
		[ "$(stat -c %s "$clip")" = "$CLIP_BYTES" ] || { echo "Generated clip is too small" >&2; return 1; }
	fi
}

# Pico build: pico_check <name> <targets or ""> <cmake options>...
pico_check() {
	local dir=pico/build/check-$1 targets=$2
	shift 2
	rm -rf "${ROOT:?}/$dir"
	mkdir -p "$ROOT/$dir/clip"
	cp "$OUT/clip.mpg" "$ROOT/$dir/clip/clip.mpg"
	in_docker cmake -S pico -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMD_MPEG_CLIP_DIR="/src/$dir/clip" "$@" &&
		in_docker cmake --build "$dir" ${targets:+--target $targets}
}

pico_displays() {
	local displays default=st7789_240x280 name option failed=()
	displays=$(sed -n '/^set(MD_MPEG_PICO_DISPLAYS/,/)/p' "$ROOT/pico/CMakeLists.txt" | tr '()' '  ' | tr -s ' \t\n' '\n' |
		grep -v -e '^set$' -e '^MD_MPEG_PICO_DISPLAYS$' -e '^$' -e "^$default\$")
	[ "$(echo "$displays" | wc -l)" -ge 10 ] || { echo "Couldn't read the display modules from pico/CMakeLists.txt" >&2; return 1; }
	local -a builds=()
	for display in $displays; do
		builds+=("$display -DMD_MPEG_PICO_DISPLAY=$display")
	done
	builds+=("rotation90 -DMD_MPEG_PICO_DISPLAY_ROTATION=90")
	# Several at once; each build uses every core while compiling but waits on Docker and CMake for a while
	for build in "${builds[@]}"; do
		read -r name option <<< "$build"
		while [ "$(jobs -rp | wc -l)" -ge 6 ]; do wait -n; done
		(pico_check "display-$name" "video_player video_player_sd display_test" "$option" > "$LOGS/pico-displays-$name.log" 2>&1
			echo $? > "$LOGS/pico-displays-$name.status") &
	done
	wait
	for build in "${builds[@]}"; do
		read -r name option <<< "$build"
		if [ "$(cat "$LOGS/pico-displays-$name.status" 2>/dev/null)" = 0 ]; then
			echo "pass $name"
		else
			echo "FAILED $name, see $LOGS/pico-displays-$name.log"
			failed+=("$name")
		fi
	done
	[ ${#failed[@]} -eq 0 ]
}

# Seeds for the fuzzer: a byte choosing the harness's mode (see tests/md_mpeg_fuzz.cpp), then a stream
fuzz_seeds() {
	local seeds=$1 media=$OUT/fuzz/media
	mkdir -p "$seeds" "$media"
	local ff="ffmpeg -nostdin -loglevel error -y"
	local video="-f lavfi -i testsrc=size=96x64:rate=25 -t 1"
	local audio="-f lavfi -i sine=frequency=440 -t 1"
	$ff $video $audio -c:v mpeg1video -bf 2 -g 6 -c:a mp2 -f mpeg "$media/m1.mpg" &&
		$ff $video $audio -c:v mpeg2video -bf 2 -g 6 -flags +ilme+ildct -c:a ac3 -f vob "$media/m2.vob" &&
		$ff $video $audio -c:v mpeg2video -c:a pcm_s16be -f vob "$media/lpcm.vob" &&
		$ff $video -c:v mpeg1video -bf 2 -f mpeg1video "$media/video.m1v" || return 1
	for clip in m1.mpg m2.vob lpcm.vob; do
		{ printf '\x00'; head -c 65536 "$media/$clip"; } > "$seeds/$clip"
	done
	{ printf '\x01'; head -c 65536 "$media/video.m1v"; } > "$seeds/video.m1v"
	# DVD information files, if the desktop check has made its test DVD
	find "$OUT/desktop/test_media" -iname '*.ifo' 2>/dev/null | head -n 4 | while read -r ifo; do
		{ printf '\x02'; cat "$ifo"; } > "$seeds/$(basename "$(dirname "$ifo")")-$(basename "$ifo")"
	done
}

fuzz() {
	local dir=$OUT/fuzz
	cmake -S "$ROOT" -B "$dir/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ \
		-DMD_MPEG_FUZZ=ON "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=all" &&
		cmake --build "$dir/build" --target md_mpeg_fuzz &&
		fuzz_seeds "$dir/seeds" || return 1
	mkdir -p "$dir/corpus" "$dir/crashes"
	local before
	before=$(ls "$dir/crashes" | wc -l)
	# Separate processes sharing the corpus (-fork mode is experimental, and its parent has crashed). Each job
	# logs to $dir/fuzz-<n>.log.
	local jobs=$(( $(nproc) / 4 > 1 ? $(nproc) / 4 : 1 ))
	cp -n "$dir/seeds/"* "$dir/corpus/"
	rm -f "$dir"/fuzz-*.log
	(cd "$dir" && "$dir/build/md_mpeg_fuzz" -max_total_time="${FUZZ_SECONDS:-60}" -max_len=65536 -timeout=10 \
		-rss_limit_mb=2048 -jobs=$jobs -workers=$jobs -artifact_prefix="$dir/crashes/" "$dir/corpus")
	tail -n 3 "$dir"/fuzz-*.log
	local after
	after=$(ls "$dir/crashes" | wc -l)
	if [ "$after" -gt "$before" ]; then
		echo "$((after - before)) new crashing input(s) in $dir/crashes; reproduce with $dir/build/md_mpeg_fuzz <file>"
		return 1
	fi
}

# The assembler, linked on its own in Docker, run against cases with the C's results
pico_asm() {
	local dir=$OUT/pico-asm rel=build/check/pico-asm
	cmake -S "$ROOT" -B "$dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release &&
		cmake --build "$dir/build" --target md_mpeg_asm_vectors &&
		"$dir/build/md_mpeg_asm_vectors" "$dir/vectors" "${ASM_CASES:-20000}" &&
		in_docker sh -c "arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -c pico/idct_m0.S -o $rel/idct_m0.o &&
			arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -c pico/predict_m0.S -o $rel/predict_m0.o &&
			arm-none-eabi-ld -T tests/asm_check.ld -o $rel/asm.elf $rel/idct_m0.o $rel/predict_m0.o &&
			arm-none-eabi-objcopy -O binary $rel/asm.elf $rel/asm.bin &&
			arm-none-eabi-nm $rel/asm.elf > $rel/asm.symbols" &&
		python3 "$ROOT/tests/asm_check.py" "$dir/asm.bin" "$dir/asm.symbols" "$dir/vectors"
}

run_check() {
	case "$1" in
		desktop) desktop_check desktop all -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ ;;
		desktop-clang) desktop_check desktop-clang all -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ ;;
		desktop-asan)
			desktop_check desktop-asan "md_mpeg_tests md_mpeg_fuzz_replay" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
				"-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
			;;
		desktop-pico-options)
			desktop_check desktop-pico-options "md_mpeg_tests md_mpeg_fuzz_replay" -DCMAKE_BUILD_TYPE=Release \
				-DMD_MPEG_AUDIO_MONO=ON -DMD_MPEG_ENABLE_MPEG2=OFF -DMD_MPEG_DIAGNOSTICS=OFF
			;;
		desktop-float)
			desktop_check desktop-float "md_mpeg_tests md_mpeg_fuzz_replay" -DCMAKE_BUILD_TYPE=Release \
				-DMD_MPEG_AUDIO_FIXED_POINT=OFF -DMD_MPEG_SPARSE_IDCT=OFF
			;;
		pico-host) "$ROOT/pico/pico.sh" test ;;
		pico-asm) pico_asm ;;
		pico) pico_check pico "" -DPICO_BOARD=pico ;;
		pico2) pico_check pico2 "" -DPICO_BOARD=pico2 ;;
		pico-displays) pico_displays ;;
		fuzz) fuzz ;;
	esac
}

if [ $needs_docker = 1 ]; then
	# Once, before the checks start their containers
	docker build -q -t "$IMAGE" "$ROOT/pico" > /dev/null || { echo "Docker image build failed" >&2; exit 1; }
	make_clip || exit 1
fi

start=$SECONDS
declare -A pids
for check in "${CHECKS[@]}"; do
	(
		s=$SECONDS
		run_check "$check" > "$LOGS/$check.log" 2>&1
		status=$?
		echo "$status $((SECONDS - s))" > "$LOGS/$check.status"
		exit $status
	) &
	pids[$check]=$!
done

failed=()
for check in "${CHECKS[@]}"; do
	wait "${pids[$check]}"
	read -r status seconds < "$LOGS/$check.status"
	if [ "$status" = 0 ]; then
		printf 'pass  %-22s %4d s\n' "$check" "$seconds"
	else
		printf 'FAIL  %-22s %4d s  %s\n' "$check" "$seconds" "$LOGS/$check.log"
		failed+=("$check")
	fi
done
echo "Total $((SECONDS - start)) s"

if [ ${#failed[@]} -gt 0 ]; then
	echo "Failed: ${failed[*]}" >&2
	for check in "${failed[@]}"; do
		echo "--- $check (last 20 lines) ---" >&2
		tail -n 20 "$LOGS/$check.log" >&2
	done
	exit 1
fi
