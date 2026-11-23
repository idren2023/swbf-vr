cmake_minimum_required(VERSION 3.20)
project(swbf_vr LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Build for x86 (Win32): Battlefront.exe is 32-bit. Use the 'x86' preset.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so users need no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB SWBF_SRC src/*.cpp)
add_library(swbf_vr SHARED ${SWBF_SRC} src/dinput8.def)
set_target_properties(swbf_vr PROPERTIES OUTPUT_NAME dinput8 PREFIX "")

set(SWBF_GAME_DIR "C:/Program Files (x86)/Steam/steamapps/common/Star Wars Battlefront (Classic 2004)/GameData"
    CACHE PATH "Folder containing Battlefront.exe")
add_custom_target(deploy
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:swbf_vr> "${SWBF_GAME_DIR}/dinput8.dll"
  DEPENDS swbf_vr)
