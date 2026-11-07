#!/usr/bin/env bash
# Raspberry Pi Pico (RP2040) and Pico 2 (RP2350) builds; everything runs in Docker.
#
#   pico/pico.sh [--board pico|pico2] build [clip.mpg]   Build the image and every app, embedding part of the clip.
#   pico/pico.sh [--board pico|pico2] flash [target]     Flash benchmark_flash (default), video_player, video_player_sd or display_test. The board must be connected.
#   pico/pico.sh test                                     Test the FAT32 code on the desktop, against generated disk images
#   pico/pico.sh monitor [seconds]                        Print the board's USB serial output, default 180 s
#
# --board defaults to pico, which also runs on a Pico W. Each board builds into pico/build/<board>.
set -euo pipefail

IMAGE=md_mpeg_pico
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BOARD=pico
if [ "${1:-}" = "--board" ]; then
	BOARD=${2:?--board needs pico or pico2}
	shift 2
fi
case "$BOARD" in
	pico | pico2) ;;
	*)
		echo "Unknown board $BOARD, expected pico or pico2" >&2
		exit 1
		;;
esac
BUILD=pico/build/$BOARD
# Fits the Pico W's 2 MB of flash with the code and wireless firmware. Starts at a pack holding a sequence
# header, and includes termaudio.mpg's largest pictures.
CLIP_START=471040
CLIP_BYTES=1250000

in_docker() {
	docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT:/src" -w /src "$IMAGE" "$@"
}

case "${1:-}" in
	build)
		clip=${2:-$ROOT/testfiles/termaudio.mpg}
		docker build -t "$IMAGE" "$ROOT/pico"
		mkdir -p "$ROOT/$BUILD/clip"
		head -c "$((CLIP_START + CLIP_BYTES))" "$clip" | tail -c "$CLIP_BYTES" > "$ROOT/$BUILD/clip/clip.mpg"
		in_docker cmake -S pico -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD="$BOARD" \
			-DMD_MPEG_CLIP_DIR="/src/$BUILD/clip"
		in_docker cmake --build "$BUILD"
		in_docker arm-none-eabi-size "$BUILD/benchmark_flash.elf" "$BUILD/video_player_sd.elf"
		;;
	test)
		docker build -q -t "$IMAGE" "$ROOT/pico" > /dev/null
		dir=pico/build/fat32_test
		rm -rf "${ROOT:?}/$dir"
		mkdir -p "$ROOT/$dir"
		in_docker pico/make_fat32_images.sh "$dir"
		in_docker g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -Ipico pico/fat32_test.cpp -o "$dir/fat32_test"
		in_docker "$dir/fat32_test" "$dir"
		;;
	flash)
		docker run --rm --privileged -v /dev/bus/usb:/dev/bus/usb -v "$ROOT:/src" -w /src "$IMAGE" \
			picotool load -f -v -x "$BUILD/${2:-benchmark_flash}.uf2"
		;;
	monitor)
		seconds=${2:-180}
		device=$(ls /dev/ttyACM* | head -n1)
		docker run --rm --device "$device" "$IMAGE" \
			sh -c "stty -F $device raw -echo; timeout $seconds cat $device" || true
		;;
	*)
		sed -n '2,9p' "$0"
		exit 1
		;;
esac
