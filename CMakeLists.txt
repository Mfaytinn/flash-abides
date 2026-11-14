cmake_minimum_required(VERSION 3.20)

project(t3_lean_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(T3_STATIC "Link the simulate binary statically" OFF)

# The RNG transforms and latency model must round like numpy / CPython.
set(T3_FLAGS -ffp-contract=off -fno-math-errno -fno-plt -march=x86-64-v3)

add_library(t3core STATIC
  src/json.cpp
  src/scenario.cpp
  src/sim.cpp
  src/sim_wheel.cpp
  src/sim_linewheel.cpp
  src/sim_linewheel_lean.cpp
  src/sim_nswheel.cpp
  src/sim_nswheel_lean.cpp
  src/parquet.cpp
  src/snappy.cpp
  src/output.cpp)
target_include_directories(t3core PUBLIC include)
target_compile_options(t3core PRIVATE ${T3_FLAGS} -Wall -Wextra)

add_executable(simulate src/main.cpp)
target_link_libraries(simulate PRIVATE t3core)
target_compile_options(simulate PRIVATE ${T3_FLAGS} -Wall -Wextra)
if(T3_STATIC)
  target_link_options(simulate PRIVATE -static)
endif()
