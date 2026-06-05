cmake_minimum_required(VERSION 3.15)
project(midi_clip_player)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LV2 REQUIRED lv2)

add_library(midi_clip_player MODULE src/plugin.cpp)

target_include_directories(midi_clip_player PRIVATE 
    ${LV2_INCLUDE_DIRS}
    /opt/homebrew/Cellar/lv2/1.18.10/include
    /opt/homebrew/include
)
target_compile_options(midi_clip_player PRIVATE -fvisibility=hidden)

set_target_properties(midi_clip_player PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "midi_clip_player"
)

install(TARGETS midi_clip_player
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/midi_clip_player.lv2
)
install(FILES manifest.ttl plugin.ttl
    DESTINATION ${CMAKE_INSTALL_PREFIX}/midi_clip_player.lv2
)