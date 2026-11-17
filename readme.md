# MPEG1 Video and Audio Player

This is a C++ application for playing MPEG1 and MPEG2 video with MP2 or A/52 audio.

## Features

- Plays MPEG1 and MPEG2 video and MP2 audio from `.mpg` and `.vob` files.
- Decodes A/52 audio (the usual audio of DVD `.vob` files), downmixing surround to stereo.
- Plays DVD-Video folders (`VIDEO_TS`) like a DVD player: from the disc's start through its menus, with menu buttons
  chosen by the keyboard or mouse.
- Supports MPEG-1 and MPEG-2 program streams (`.mpg`, `.vob`), detected per file.
- MPEG2 main profile video: progressive and interlaced, frame and field pictures, 3:2 pulldown timing.
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

To play a DVD with its menus, pass its `VIDEO_TS` folder, the folder holding it, or its `VIDEO_TS.IFO`:

```bash
./build/md_mpeg <path-to-dvd-folder>
```

A single `.vob` file plays as a program stream, without menus.

### Controls

| Key | Action |
|---|---|
| Space or P | Pause / resume |
| Left / Right | Seek back / forward 10 seconds |
| Down / Up | Seek back / forward 60 seconds |
| Escape or Q | Quit |

Playing a DVD folder:

| Key | Action |
|---|---|
| Arrow keys | In a menu, move between buttons; in a title, seek as above |
| Enter, or click | Choose the menu button (moving the mouse over a button selects it) |
| M / T | Root menu / title menu |
| Page Down / Page Up | Next / previous chapter |

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

## Fixed point audio

The MP2 decoder uses integers by default, with no value wider than 32 bits, and outputs 16-bit samples. Turn
`MD_MPEG_AUDIO_FIXED_POINT` off to use floating point and output 32-bit float samples instead:

```bash
cmake -S . -B build -DMD_MPEG_AUDIO_FIXED_POINT=OFF
```

Without CMake, define it as 0 or 1 on the compiler command line. `src/mpeg1/AudioSample.h` has the default.

## MPEG2

MPEG2 video and program streams are decoded by default. Turn `MD_MPEG_ENABLE_MPEG2` off for an MPEG1-only
build with a smaller binary (the Pico build has it off by default):

```bash
cmake -S . -B build -DMD_MPEG_ENABLE_MPEG2=OFF
```

Without CMake, define it as 0 or 1 on the compiler command line; it must be the same for every file that
includes the decoder headers. Only 4:2:0 video is supported (not the 4:2:2 profile). Interlaced pictures are
shown as whole frames, without deinterlacing.

The same option enables the A/52 audio decoder, written from the ATSC A/52 standard (published as ETSI TS 102 366),
for A/52 audio in private stream 1 of a program stream. All channel layouts are decoded: 3 to 5 channels are
downmixed to stereo using the stream's centre and surround mix levels, and the low frequency effects channel is
left out. Dynamic range compression is applied as the stream asks. It decodes with floating point, so it isn't
meant for the Pico build.

When a file has several audio streams, the lowest numbered is played, which on a DVD is usually the main
soundtrack. A .vob file can hold several programs one after another (an intro, then the film) with different
audio streams; when the playing stream stops, the lowest numbered of those still present takes over.

## DVD-Video

DVD folders are played with the MPEG-2 option. `src/dvd/` reads the information files (`.IFO`, falling back to the
`.BUP` copies), runs the discs' navigation commands from First Play through menus and titles, decodes menu button and
subtitle sub-pictures and draws button highlights from the navigation packs. The formats follow the documentation of
the libdvdread and libdvdnav projects and the DVD-Video pages at dvd.sourceforge.net.

Not supported yet: choosing audio and subtitle streams (the disc's defaults are used, with only forced subtitles
shown), camera angles (the first is played), LPCM and DTS audio, encrypted (CSS) discs, the navigation timer, and
sub-picture colour changes within a picture. Resuming a title from a menu restarts the cell it was in.

## Tests

The tests generate small test clips with ffmpeg (no media is stored in the repository) and decode them
headless: clean streams, B-frames, odd frame sizes, audio-only and video-only streams, unsupported audio,
truncated, corrupt and concatenated files, and seeking. MPEG2 clips (progressive, interlaced, the alternate
coding options, and dual prime if mjpegtools' `mpeg2enc` is installed) are compared picture by picture with
ffmpeg's decode. A/52 audio (mono, stereo, coupled, short blocks, 5.1 downmixed, 32 to 48 kHz) is compared
sample by sample with ffmpeg's decode. They are skipped if ffmpeg isn't installed.

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

## Code

- `src/md_mpeg.cpp`: the player (SDL window, audio, input, A/V sync)
- `src/mpeg1/`: program stream demuxer, MPEG-1 video and MP2 audio decoders. `Mpeg2Video.cpp` has the
  MPEG-2 macroblock layer, `A52AudioDecoder.cpp` the A/52 audio decoder.
- `src/dvd/`: DVD-Video information files, navigation, sub-pictures and navigation packs; `src/DvdPlayer.cpp` plays them
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
- Seeking uses a map of the file's timing sampled when it is opened, refined by reading the file around the target, so it
  lands within about a second of the target. Files made of several programs, each starting its clock again (common in
  DVD .vob files), are timed as one timeline.
- Only supports MPEG1/MPEG2 video, and MP2 (MPEG-1 Layer II) and A/52 audio. MP3, LPCM and DTS audio are not
  supported. MPEG-2 transport streams (`.ts`) are not supported.
- Raw `.mp2` audio files (without a program stream) are not supported.

## Acknowledgments

This project uses portions of the [PL_MPEG library](https://github.com/phoboslab/pl_mpeg) by phoboslab, a single-file C library for decoding MPEG1 video and MP2 audio.

## Contributing

Contributions are welcome! Please submit a pull request or open an issue for bug reports or feature requests.

## Contact

For questions or support, please open an issue on this repository.