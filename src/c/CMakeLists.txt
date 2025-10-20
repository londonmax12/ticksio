cmake_minimum_required(VERSION 3.25)
project(ticksio VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(ticksio STATIC
    src/ticksio.c
    src/ticksio_csv.c
    src/ticksio_helpers.c
    src/ticksio_chunks.c
    src/ticksio_index.c
)

target_include_directories(ticksio PUBLIC include)
target_include_directories(ticksio PRIVATE include/ticksio)

add_executable(sandbox tests/sandbox.c)

target_include_directories(sandbox PRIVATE
    include
)
target_link_libraries(sandbox PRIVATE ticksio)