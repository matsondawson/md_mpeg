# Desktop test of CardVolume.h against FAT images, run by pico/pico.sh test
cmake_minimum_required(VERSION 3.20)
project(md_mpeg_pico_host_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(../fat-fs.cmake)

add_executable(card_volume_test card_volume_test.cpp)
target_include_directories(card_volume_test PRIVATE ..)
target_link_libraries(card_volume_test PRIVATE fat::fat)
target_compile_options(card_volume_test PRIVATE -Wall -Wextra -fsanitize=address,undefined)
target_link_options(card_volume_test PRIVATE -fsanitize=address,undefined)
