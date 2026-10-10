cmake_minimum_required(VERSION 3.20)
project(RaidWorkshop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build for x64: Guild Wars 2 is a 64-bit game.")
endif()

add_library(RaidWorkshop SHARED
  src/main.cpp
  thirdparty/imgui/imgui.cpp
  thirdparty/imgui/imgui_draw.cpp
  thirdparty/imgui/imgui_widgets.cpp
  thirdparty/imgui/imgui_tables.cpp
)
target_include_directories(RaidWorkshop PRIVATE include thirdparty/imgui)
target_compile_definitions(RaidWorkshop PRIVATE IMGUI_DISABLE_DEMO_WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  # static runtime: players don't need extra Visual C++ redistributables
  set_property(TARGET RaidWorkshop PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
target_link_libraries(RaidWorkshop PRIVATE imm32 winhttp)
