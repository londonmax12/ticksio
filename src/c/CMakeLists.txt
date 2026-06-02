cmake_minimum_required(VERSION 3.25)
project(ticksio VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- ZSTD (per-chunk block compression) -------------------------------------
# Fetched and built at configure time. Build only the static library — no CLI
# programs, tests, or shared lib — to keep the dependency lean.
include(FetchContent)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    # zstd's CMake project lives under build/cmake, not the repo root.
    SOURCE_SUBDIR  build/cmake
)
FetchContent_MakeAvailable(zstd)

add_library(ticksio STATIC
    src/ticksio.c
    src/ticksio_csv.c
    src/ticksio_helpers.c
    src/ticksio_chunks.c
    src/ticksio_compress.c
    src/ticksio_index.c
    src/ticksio_schema.c
)

target_include_directories(ticksio PUBLIC include)
target_include_directories(ticksio PRIVATE include/ticksio)
# zstd.h lives in the fetched source's lib/ directory; the static target name is
# libzstd_static. Linking PUBLIC so consumers of the static ticksio lib link it too.
target_include_directories(ticksio PRIVATE ${zstd_SOURCE_DIR}/lib)
target_link_libraries(ticksio PUBLIC libzstd_static)

add_executable(sandbox tests/sandbox.c)

target_include_directories(sandbox PRIVATE
    include
)
target_link_libraries(sandbox PRIVATE ticksio)

# Round-trip test: writes synthetic ticks, then decodes the raw chunk bytes and
# checks every value reconstructs (columnar + delta + zig-zag).
add_executable(roundtrip tests/roundtrip.c)
target_include_directories(roundtrip PRIVATE include)
target_link_libraries(roundtrip PRIVATE ticksio)