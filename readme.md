# MPEG1 Video and Audio Player

This is a C++ application for playing MPEG1 video and MP2 audio files.

## Features

- Plays MPEG1 video and MP2 audio from `.mpg` files.
- Supports MPEG-PS (`.mpg`) files containing MPEG1 video and MP2 audio streams.
- Audio/video sync using the audio as the master clock; falls back to the wall clock for files with no audio, or once the audio ends.
- Late video frames are dropped to keep up with the audio.

## License

This project is licensed under the MIT License, see [LICENSE](LICENSE).

Parts of the decoder are adapted from the MIT-licensed [PL_MPEG library](https://github.com/phoboslab/pl_mpeg), see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Dependencies

- [SDL3](https://www.libsdl.org/) for rendering and audio playback.

## Building

To build the player, ensure you have SDL3 installed on your system.

Arch linux:
```bash
sudo pacman -S sdl3
```

Debian:
```bash
sudo apt install libsdl3-dev
```

Then run one of:
```bash
./build.sh           # default build
./build_release.sh   # optimised release build
```

The binary is written to `build/md_mpeg`.

### Prerequisites

- cmake, make, C++20 compiler
- SDL3 libraries
- ffmpeg (optional, for the tests)

## Usage

Run the player with an MPEG-PS (`.mpg`) file as an argument:

```bash
./build/md_mpeg <path-to-video.mpg>
```

### Controls

| Key | Action |
|---|---|
| Space or P | Pause / resume |
| Left / Right | Seek back / forward 10 seconds |
| Down / Up | Seek back / forward 60 seconds |
| Escape or Q | Quit |

### Install and "Open with"

Install the player and a desktop entry, so MPEG files can be opened with md_mpeg from a file manager
(right click, Open with):

```bash
cmake --install build --prefix ~/.local
update-desktop-database ~/.local/share/applications
```

`~/.local/bin` must be on your `PATH`. To make it the default player for `.mpg` files:

```bash
xdg-mime default md_mpeg.desktop video/mpeg
```

## Tests

The tests generate small test clips with ffmpeg (no media is stored in the repository) and decode them
headless: clean streams, B-frames, odd frame sizes, audio-only and video-only streams, unsupported audio,
truncated, corrupt and concatenated files, and seeking. They are skipped if ffmpeg isn't installed.

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

## Code

- `src/md_mpeg.cpp`: the player (SDL window, audio, input, A/V sync)
- `src/mpeg1/`: program stream demuxer, MPEG-1 video and MP2 audio decoders
- `tests/md_mpeg_tests.cpp`: decoder tests

## Encoding Compatible Files

To create compatible `.mpg` files, use `ffmpeg` with the following command:

```bash
ffmpeg -i sourceFile.avi -c:v mpeg1video -c:a mp2 -b:v 2000k -b:a 192k dest.mpg
```

Optionally add a denoise filter, which helps MPEG1 quality at lower bitrates:

```bash
ffmpeg -i sourceFile.avi -c:v mpeg1video -c:a mp2 -b:v 2000k -b:a 192k -vf "hqdn3d=4:3:6:4.5" dest.mpg
```

Test media is not included in the repository; put your own files in `testfiles/` (ignored by git).

- Refer to the [ffmpeg documentation](https://ffmpeg.org/documentation.html) for more information.

## Limitations

- Corrupt data is skipped with a console message. Pictures predicted from a corrupt picture show errors until the next I-frame.
- Seeking estimates the file position from the average bit rate, so it lands within a few seconds of the target in variable bit rate files.
- Only supports MPEG1 video and MP2 (MPEG-1 Layer II) audio. MPEG2 video and MP3 audio are not supported.
- Raw `.mp2` audio files (without a program stream) are not supported.

## Acknowledgments

This project uses portions of the [PL_MPEG library](https://github.com/phoboslab/pl_mpeg) by phoboslab, a single-file C library for decoding MPEG1 video and MP2 audio.

## Contributing

Contributions are welcome! Please submit a pull request or open an issue for bug reports or feature requests.

## Contact

For questions or support, please open an issue on this repository.