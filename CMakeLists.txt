cmake_minimum_required(VERSION 3.25)
project(nr_mac_scheduler VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NRS_BUILD_TESTS "Build unit tests" ON)
set(NRS_SANITIZER "" CACHE STRING "Sanitizer build: empty, asan-ubsan or tsan")

# Sanitizers must instrument every translation unit (including dependencies),
# otherwise TSan reports false positives from uninstrumented code.
if(NRS_SANITIZER STREQUAL "asan-ubsan")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
elseif(NRS_SANITIZER STREQUAL "tsan")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT NRS_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown NRS_SANITIZER '${NRS_SANITIZER}'")
endif()

include(FetchContent)
# Some pinned deps still declare very old cmake_minimum_required, which CMake >= 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
# Pinned release tarballs: reproducible and faster than git clones.
# SYSTEM keeps third-party warnings out of our -Werror build.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
  SYSTEM)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
  URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.9.0.tar.gz
  SYSTEM)

FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  SYSTEM)

FetchContent_MakeAvailable(httplib yaml-cpp json)

find_package(Threads REQUIRED)

add_library(nrs_warnings INTERFACE)
target_compile_options(nrs_warnings INTERFACE -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

# Pure domain logic: no threads, no sockets. Everything here is unit-testable
# and is what offline mode and CI benchmarks exercise.
add_library(nrs_core STATIC
  src/phy/cqi_table.cpp
  src/core/cell.cpp
  src/sched/scheduler.cpp
  src/sched/round_robin.cpp
  src/sched/proportional_fair.cpp
  src/sched/max_throughput.cpp
  src/sched/greedy_fill.cpp
  src/proto/wire.cpp
  src/kpi/kpi.cpp
  src/kpi/kpi_json.cpp
  src/config/config.cpp
  src/offline/trace.cpp
  src/offline/replay.cpp
)
target_include_directories(nrs_core PUBLIC src)
target_link_libraries(nrs_core PUBLIC yaml-cpp::yaml-cpp nlohmann_json::nlohmann_json PRIVATE nrs_warnings)

# Linux runtime: epoll receiver, real-time slot loop, HTTP endpoints.
add_library(nrs_runtime STATIC
  src/runtime/udp_receiver.cpp
  src/runtime/slot_loop.cpp
  src/runtime/prometheus.cpp
  src/runtime/http_server.cpp
)
target_link_libraries(nrs_runtime PUBLIC nrs_core httplib::httplib Threads::Threads PRIVATE nrs_warnings)

add_executable(nr_mac_scheduler src/main.cpp)
target_link_libraries(nr_mac_scheduler PRIVATE nrs_runtime nrs_warnings)

if(NRS_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    SYSTEM)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_subdirectory(tests/unit)
endif()
