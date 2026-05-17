cmake_minimum_required(VERSION 3.20)

project(rpg2000_ogg_hooker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "rpg2000_ogg_hooker targets Win32 only.")
endif()

if(MSVC)
  add_compile_options(/utf-8)
endif()

add_library(roh_common STATIC
  src/hooker/log.cpp
  src/hooker/paths.cpp
)
target_include_directories(roh_common PUBLIC src)
target_compile_definitions(roh_common PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(RPG2000_ogg_hooker
  src/hooker/launcher.cpp
)
target_link_libraries(RPG2000_ogg_hooker PRIVATE roh_common)
target_compile_definitions(RPG2000_ogg_hooker PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_options(RPG2000_ogg_hooker PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)

add_library(rpg2k_ogg_hook SHARED
  src/hooker/hook_dll.cpp
  src/hooker/mf_ogg_decoder.cpp
)
target_link_libraries(rpg2k_ogg_hook PRIVATE roh_common mfplat mfreadwrite mfuuid)
target_compile_definitions(rpg2k_ogg_hook PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(rpg2k_ogg_hook PROPERTIES OUTPUT_NAME "rpg2k_ogg_hook")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(WARNING "This project must be built as x86 for RPG2000.exe. Select a Win32/x86 generator or architecture.")
endif()
