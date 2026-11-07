cmake_minimum_required(VERSION 3.20)

# The decoders use exceptions for end of data and corrupt streams
set(PICO_CXX_ENABLE_EXCEPTIONS 1)
set(PICO_CXX_ENABLE_RTTI 1)
# Also runs on a Pico W, leaving its wireless chip unused
set(PICO_BOARD pico)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(md_mpeg_pico LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

set(MD_MPEG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(MD_MPEG_CLIP_DIR "" CACHE PATH "Directory holding clip.mpg, the clip to embed")

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
		MD_MPEG_DEMUX_RING_SIZE=512
		MD_MPEG_VIDEO_RING_SIZE=9216
		MD_MPEG_AUDIO_RING_SIZE=384
		MD_MPEG_SOUND_RING_SIZE=4096
		PICO_STACK_SIZE=0x1000
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
add_decoder_app(video_player video_player.cpp)
add_decoder_app(video_player_scaled video_player.cpp)
target_compile_definitions(video_player_scaled PRIVATE VIDEO_SCALED=1)
add_decoder_app(video_player_sd video_player.cpp)
target_compile_definitions(video_player_sd PRIVATE VIDEO_SCALED=1 VIDEO_FROM_SD=1)

add_executable(display_test display_test.cpp)
target_link_libraries(display_test PRIVATE pico_stdlib hardware_spi)
pico_enable_stdio_usb(display_test 1)
pico_enable_stdio_uart(display_test 0)
pico_add_extra_outputs(display_test)
