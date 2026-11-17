cmake_minimum_required(VERSION 3.20)
project("md_mpeg" LANGUAGES CXX)

# Multi-config generators choose the configuration at build time
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# add_compile_options("$<$<CONFIG:Debug>:-DMDEBUG>")

set(MD_MPEG_COMPILE_OPTIONS
	$<$<CONFIG:Release>:-O3 -march=native -ffast-math>
	$<$<CONFIG:Debug>:-O0>
)

option(MD_MPEG_AUDIO_FIXED_POINT "Decode audio with 32-bit integer arithmetic and output 16-bit samples" ON)
option(MD_MPEG_SPARSE_IDCT "Skip the IDCT arithmetic for rows and columns with only a first coefficient" ON)
option(MD_MPEG_AUDIO_MONO "Mix stereo audio to mono before the synthesis filter, halving its work" OFF)
option(MD_MPEG_ENABLE_MPEG2 "Decode MPEG-2 video and program streams, and A/52 and LPCM audio, as well as MPEG-1" ON)
option(MD_MPEG_DIAGNOSTICS "Compile in diagnostic messages and their callback, not just the counters (src/Diagnostics.h)" ON)

# Demuxing and decoding, no SDL dependency. Shared by the player and the tests.
add_library(md_mpeg_core STATIC
	"src/Time.cpp"
	"src/Deinterlace.cpp"
	"src/mpeg1/Frame.cpp"
	"src/mpeg1/Sequence.cpp"
	"src/mpeg1/Mpeg1VideoDecoder.cpp"
	"src/mpeg1/Mpeg2Video.cpp"
	"src/mpeg1/Mpeg1AudioDecoder.cpp"
	"src/mpeg1/A52AudioDecoder.cpp"
	"src/mpeg1/LpcmAudioDecoder.cpp"
	"src/dvd/Ifo.cpp"
	"src/dvd/Disc.cpp"
	"src/dvd/Navigator.cpp"
	"src/dvd/SubPicture.cpp"
	"src/dvd/NavPack.cpp"
	"src/dvd/Overlay.cpp"
)
target_include_directories(md_mpeg_core PUBLIC
	"${CMAKE_SOURCE_DIR}/src"
	"${CMAKE_SOURCE_DIR}/src/stream"
	"${CMAKE_SOURCE_DIR}/src/mpeg1"
)
target_compile_options(md_mpeg_core PRIVATE ${MD_MPEG_COMPILE_OPTIONS})
target_compile_definitions(md_mpeg_core PUBLIC
	MD_MPEG_AUDIO_FIXED_POINT=$<BOOL:${MD_MPEG_AUDIO_FIXED_POINT}>
	MD_MPEG_AUDIO_MONO=$<BOOL:${MD_MPEG_AUDIO_MONO}>
	# Public: it changes the decoder classes' layout
	MD_MPEG_ENABLE_MPEG2=$<BOOL:${MD_MPEG_ENABLE_MPEG2}>
	MD_MPEG_DIAGNOSTICS=$<BOOL:${MD_MPEG_DIAGNOSTICS}>
)
target_compile_definitions(md_mpeg_core PRIVATE MD_MPEG_SPARSE_IDCT=$<BOOL:${MD_MPEG_SPARSE_IDCT}>)

# Player
find_package(SDL3 REQUIRED)

add_executable(md_mpeg
	"src/md_mpeg.cpp"
	"src/sdl2/screenContext.cpp"
	"src/sdl2/ControlBar.cpp"
	"src/DvdPlayer.cpp"
)
target_compile_options(md_mpeg PRIVATE ${MD_MPEG_COMPILE_OPTIONS})
target_link_libraries(md_mpeg PRIVATE md_mpeg_core SDL3::SDL3)

# Tests: decode generated clips (needs ffmpeg at test time, skipped otherwise)
enable_testing()
add_executable(md_mpeg_tests "tests/md_mpeg_tests.cpp")
target_link_libraries(md_mpeg_tests PRIVATE md_mpeg_core)
add_test(NAME md_mpeg_tests COMMAND md_mpeg_tests "${CMAKE_CURRENT_BINARY_DIR}/test_media")
set_tests_properties(md_mpeg_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

# The fuzz harness run on the saved inputs that once crashed or hung it
add_executable(md_mpeg_fuzz_replay "tests/md_mpeg_fuzz.cpp" "tests/md_mpeg_fuzz_replay.cpp")
target_link_libraries(md_mpeg_fuzz_replay PRIVATE md_mpeg_core)
add_test(NAME md_mpeg_fuzz_regressions COMMAND md_mpeg_fuzz_replay "${CMAKE_SOURCE_DIR}/tests/fuzz_regressions")
set_tests_properties(md_mpeg_fuzz_regressions PROPERTIES TIMEOUT 60)

# Cases for tests/asm_check.py, which runs the Pico's assembler in an emulator against the decoder's C
add_executable(md_mpeg_asm_vectors "tests/md_mpeg_asm_vectors.cpp")
target_link_libraries(md_mpeg_asm_vectors PRIVATE md_mpeg_core)

# Fuzzer: needs Clang. scripts/check.sh builds and runs it; build with
# -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined,fuzzer-no-link" so the decoder is instrumented too.
option(MD_MPEG_FUZZ "Build md_mpeg_fuzz, the libFuzzer harness" OFF)
if(MD_MPEG_FUZZ)
	add_executable(md_mpeg_fuzz "tests/md_mpeg_fuzz.cpp")
	target_link_libraries(md_mpeg_fuzz PRIVATE md_mpeg_core)
	target_link_options(md_mpeg_fuzz PRIVATE -fsanitize=fuzzer)
endif()

# Install the player and a desktop entry so it shows in "Open with" for MPEG files
include(GNUInstallDirs)
install(TARGETS md_mpeg RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "packaging/md_mpeg.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
