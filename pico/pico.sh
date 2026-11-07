#!/usr/bin/env bash
# RP2040 benchmark, everything runs in Docker.
#
#   pico/pico.sh build [clip.mpg]   Build the image and benchmark, embedding part of the clip.
#   pico/pico.sh flash [target]     Flash benchmark_flash (default), video_player, video_player_scaled or display_test. The board must be connected.
#   pico/pico.sh monitor [seconds]  Print the benchmark's USB serial output, default 180 s
set -euo pipefail

IMAGE=md_mpeg_pico
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=pico/build
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
		in_docker cmake -S pico -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMD_MPEG_CLIP_DIR=/src/$BUILD/clip
		in_docker cmake --build "$BUILD" 
		in_docker arm-none-eabi-size "$BUILD/benchmark_flash.elf"
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
		sed -n '2,6p' "$0"
		exit 1
		;;
esac
