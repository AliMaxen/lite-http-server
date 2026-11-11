cmake_minimum_required(VERSION 3.16)
project(litehttpserver LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(Threads REQUIRED)
file(GLOB SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_executable(litehttpserver ${SOURCES})
target_include_directories(litehttpserver PRIVATE include)
target_compile_options(litehttpserver PRIVATE -Wall -Wextra -Wpedantic -O2)
target_link_libraries(litehttpserver PRIVATE Threads::Threads)

enable_testing()
add_executable(parser_test tests/parser_test.c src/http_parser.c)
target_include_directories(parser_test PRIVATE include)
target_compile_options(parser_test PRIVATE -Wall -Wextra -Wpedantic -O2)
add_test(NAME parser_test COMMAND parser_test)
