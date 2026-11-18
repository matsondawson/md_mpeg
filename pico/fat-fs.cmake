# The fat-fs library (https://github.com/matsondawson/fat-fs): the SD card driver and FAT volume, used by
# video_player_sd and sd_benchmark on the Pico and by card_volume_test on the desktop.
include(FetchContent)
FetchContent_Declare(fat
	GIT_REPOSITORY https://github.com/matsondawson/fat-fs.git
	GIT_TAG v0.1.1)
# FAT32 only, with no block cache: the card driver streams and reads ahead itself
set(FAT_LONG_FILE_NAMES OFF CACHE BOOL "" FORCE)
set(FAT_BLOCK_CACHE OFF CACHE BOOL "" FORCE)
set(FAT_FAT12_16 OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fat)
