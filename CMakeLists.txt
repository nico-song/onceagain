cmake_minimum_required(VERSION 3.20)
project(onceagain LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# grab raylib from github at configure time
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(raylib)

find_package(Threads REQUIRED)

add_executable(onceagain
  src/main.cpp
  src/audio_engine.cpp
  src/library.cpp
)
target_link_libraries(onceagain PRIVATE raylib Threads::Threads)

# ring buffer stress test (run with: ctest --test-dir build)
enable_testing()
add_executable(ring_test tests/ring_test.cpp)
target_include_directories(ring_test PRIVATE src)
target_link_libraries(ring_test PRIVATE Threads::Threads)
add_test(NAME ring_test COMMAND ring_test)
