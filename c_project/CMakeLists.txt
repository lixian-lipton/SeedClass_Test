cmake_minimum_required(VERSION 4.0)
project(c_project C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(c_project
        main.c
        terminal.c
)

target_compile_options(c_project PRIVATE
        -Wall
        -Wextra
        -pedantic
)