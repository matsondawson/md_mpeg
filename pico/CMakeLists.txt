cmake_minimum_required(VERSION 3.20)

# The decoders use exceptions for end of data and corrupt streams
set(PICO_CXX_ENABLE_EXCEPTIONS 1)
set(PICO_CXX_ENABLE_RTTI 1)
# pico (RP2040, also runs on a Pico W, leaving its wireless chip unused) or pico2 (RP2350). Each board needs
# its own build directory, as pico/pico.sh uses.
set(PICO_BOARD pico CACHE STRING "Board to build for: pico or pico2")

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(md_mpeg_pico LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

set(MD_MPEG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(MD_MPEG_DISPLAY_DRIVERS "${CMAKE_CURRENT_SOURCE_DIR}/../display")

# The display module, see DisplayModule.h for each one's wiring
set(MD_MPEG_PICO_DISPLAYS
	st7789_240x280 st7789_240x320 st7789_240x240 st7789_172x320 st7789_135x240
	st7735_128x160 st7735_128x128 st7735_80x160
	ili9341_240x320 st7796_320x480 ili9488_320x480 gc9a01_240x240
	ssd1351_128x128 ssd1351_128x96 ssd1331_96x64
	ssd1306_128x64_i2c ssd1306_128x32_i2c ssd1306_128x64_spi sh1106_128x64_i2c sh1106_128x64_spi)
set(MD_MPEG_PICO_DISPLAY st7789_240x280 CACHE STRING "Display module")
set_property(CACHE MD_MPEG_PICO_DISPLAY PROPERTY STRINGS ${MD_MPEG_PICO_DISPLAYS})
if(NOT MD_MPEG_PICO_DISPLAY IN_LIST MD_MPEG_PICO_DISPLAYS)
	message(FATAL_ERROR "Unknown display module ${MD_MPEG_PICO_DISPLAY}, expected one of: ${MD_MPEG_PICO_DISPLAYS}")
endif()
set(MD_MPEG_PICO_DISPLAY_ROTATION "" CACHE STRING "Display rotation, 0, 90, 180 or 270; empty for the module's landscape default")
string(TOUPPER "MD_MPEG_DISPLAY_${MD_MPEG_PICO_DISPLAY}" MD_MPEG_DISPLAY_DEFINE)

# The chosen display module, on its bus
function(use_display name)
	target_include_directories(${name} PRIVATE "${MD_MPEG_DISPLAY_DRIVERS}")
	target_compile_definitions(${name} PRIVATE ${MD_MPEG_DISPLAY_DEFINE}=1)
	if(NOT MD_MPEG_PICO_DISPLAY_ROTATION STREQUAL "")
		target_compile_definitions(${name} PRIVATE MD_MPEG_PICO_DISPLAY_ROTATION=${MD_MPEG_PICO_DISPLAY_ROTATION})
	endif()
	target_link_libraries(${name} PRIVATE hardware_dma hardware_i2c hardware_spi)
endfunction()
set(MD_MPEG_CLIP_DIR "" CACHE PATH "Directory holding clip.mpg, the clip to embed")
option(MD_MPEG_SPARSE_IDCT "Skip the IDCT arithmetic for rows and columns with only a first coefficient" ON)
# The Pico's audio output is mono
option(MD_MPEG_AUDIO_MONO "Mix stereo audio to mono before the synthesis filter, halving its work" ON)

# The video ring holds the largest picture. Measured peaks: termaudio 9049 bytes, a 140x120 variable bit
# rate file 10378 (its header declared 6 KB, so headers can't be trusted for this). The RP2350's 520 KB of
# RAM leaves room for bigger pictures and a bigger ring.
if(PICO_BOARD STREQUAL "pico2")
	set(MD_MPEG_PICO_VIDEO_RING_SIZE 32768)
else()
	set(MD_MPEG_PICO_VIDEO_RING_SIZE 16384)
endif()

function(add_decoder_app name main_source)
	add_executable(${name}
		${main_source}
		clip.S
		"${MD_MPEG_SRC}/mpeg1/Frame.cpp"
		"${MD_MPEG_SRC}/mpeg1/Sequence.cpp"
		"${MD_MPEG_SRC}/mpeg1/Mpeg1VideoDecoder.cpp"
		"${MD_MPEG_SRC}/mpeg1/Mpeg1AudioDecoder.cpp"
	)
	target_include_directories(${name} PRIVATE "${MD_MPEG_SRC}" "${MD_MPEG_SRC}/stream" "${MD_MPEG_SRC}/mpeg1")
	target_compile_definitions(${name} PRIVATE
		MD_MPEG_AUDIO_FIXED_POINT=1
		MD_MPEG_SPARSE_IDCT=$<BOOL:${MD_MPEG_SPARSE_IDCT}>
		MD_MPEG_AUDIO_MONO=$<BOOL:${MD_MPEG_AUDIO_MONO}>
		MD_MPEG_DEMUX_RING_SIZE=512
		MD_MPEG_VIDEO_RING_SIZE=${MD_MPEG_PICO_VIDEO_RING_SIZE}
		MD_MPEG_AUDIO_RING_SIZE=384
		MD_MPEG_SOUND_RING_SIZE=4096
		PICO_STACK_SIZE=0x1000
		# Out of memory throws std::bad_alloc, so a picture too big for memory can be skipped
		PICO_MALLOC_PANIC=0
	)
	target_compile_options(${name} PRIVATE
		$<$<COMPILE_LANGUAGE:ASM>:-Wa,-I${MD_MPEG_CLIP_DIR}>
		$<$<COMPILE_LANGUAGE:CXX>:-w>
	)
	set_property(SOURCE clip.S APPEND PROPERTY OBJECT_DEPENDS "${MD_MPEG_CLIP_DIR}/clip.mpg")
	target_link_libraries(${name} PRIVATE pico_stdlib pico_multicore hardware_dma hardware_spi hardware_vreg hardware_watchdog)
	pico_enable_stdio_usb(${name} 1)
	pico_enable_stdio_uart(${name} 0)
	pico_add_extra_outputs(${name})
endfunction()

add_decoder_app(benchmark_flash benchmark.cpp)
option(MD_MPEG_PICO_AUDIO_ON_CORE1 "Decode audio on core 1, between frame scans, instead of on core 0" ON)
add_decoder_app(video_player video_player.cpp)
add_decoder_app(video_player_sd video_player.cpp)
target_compile_definitions(video_player_sd PRIVATE VIDEO_FROM_SD=1)
foreach(player video_player video_player_sd)
	use_display(${player})
	target_compile_definitions(${player} PRIVATE
		VIDEO_AUDIO_ON_CORE1=$<BOOL:${MD_MPEG_PICO_AUDIO_ON_CORE1}>
		# Core 1 would otherwise compete with core 0 for the flash cache
		MD_MPEG_AUDIO_IN_RAM=$<BOOL:${MD_MPEG_PICO_AUDIO_ON_CORE1}>
	)
endforeach()

add_executable(display_test display_test.cpp)
target_link_libraries(display_test PRIVATE pico_stdlib hardware_vreg)
use_display(display_test)
pico_enable_stdio_usb(display_test 1)
pico_enable_stdio_uart(display_test 0)
pico_add_extra_outputs(display_test)
