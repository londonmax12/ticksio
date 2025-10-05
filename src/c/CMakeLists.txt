cmake_minimum_required(VERSION 3.25)
project(ticksio VERSION 1.0 LANGUAGES C)

# Enable C99
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Public include folder
include_directories(include)

# Library
add_library(ticksio STATIC
    src/ticksio.c
)

target_include_directories(ticksio PUBLIC include)

# Tests executable
add_executable(sandbox tests/sandbox.c)
target_link_libraries(sandbox PRIVATE ticksio)
