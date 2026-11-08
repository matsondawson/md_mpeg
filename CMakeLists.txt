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

# Demuxing and decoding, no SDL dependency. Shared by the player and the tests.
add_library(md_mpeg_core STATIC
	"src/Time.cpp"
	"src/mpeg1/Frame.cpp"
	"src/mpeg1/Sequence.cpp"
	"src/mpeg1/Mpeg1VideoDecoder.cpp"
	"src/mpeg1/Mpeg1AudioDecoder.cpp"
)
target_include_directories(md_mpeg_core PUBLIC
	"${CMAKE_SOURCE_DIR}/src"
	"${CMAKE_SOURCE_DIR}/src/stream"
	"${CMAKE_SOURCE_DIR}/src/mpeg1"
)
target_compile_options(md_mpeg_core PRIVATE ${MD_MPEG_COMPILE_OPTIONS})
target_compile_definitions(md_mpeg_core PUBLIC MD_MPEG_AUDIO_FIXED_POINT=$<BOOL:${MD_MPEG_AUDIO_FIXED_POINT}>)
target_compile_definitions(md_mpeg_core PRIVATE MD_MPEG_SPARSE_IDCT=$<BOOL:${MD_MPEG_SPARSE_IDCT}>)

# Player
find_package(SDL3 REQUIRED)

add_executable(md_mpeg
	"src/md_mpeg.cpp"
	"src/sdl2/screenContext.cpp"
	"src/sdl2/ControlBar.cpp"
)
target_compile_options(md_mpeg PRIVATE ${MD_MPEG_COMPILE_OPTIONS})
target_link_libraries(md_mpeg PRIVATE md_mpeg_core SDL3::SDL3)

# Tests: decode generated clips (needs ffmpeg at test time, skipped otherwise)
enable_testing()
add_executable(md_mpeg_tests "tests/md_mpeg_tests.cpp")
target_link_libraries(md_mpeg_tests PRIVATE md_mpeg_core)
add_test(NAME md_mpeg_tests COMMAND md_mpeg_tests "${CMAKE_CURRENT_BINARY_DIR}/test_media")
set_tests_properties(md_mpeg_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

# Install the player and a desktop entry so it shows in "Open with" for MPEG files
include(GNUInstallDirs)
install(TARGETS md_mpeg RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "packaging/md_mpeg.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
