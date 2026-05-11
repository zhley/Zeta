cmake_minimum_required(VERSION 3.15)
project(Zeta VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})
set(LEXER_OUTPUT ${GEN_DIR}/lex.yy.cpp)
set(PARSER_OUTPUT ${GEN_DIR}/syntax.tab.cpp)
set(PARSER_HEADER ${GEN_DIR}/syntax.tab.h)

FLEX_TARGET(Scanner src/compiler/zeta.l ${LEXER_OUTPUT})
BISON_TARGET(Parser src/compiler/zeta.y ${PARSER_OUTPUT}
    COMPILE_FLAGS "-d --defines=${PARSER_HEADER}")

add_flex_bison_dependency(Scanner Parser)

file(GLOB_RECURSE ZETA_SRC
    src/*.cpp
    src/*.h
)

add_executable(zeta ${ZETA_SRC} ${LEXER_OUTPUT} ${PARSER_OUTPUT})
target_include_directories(zeta PRIVATE ${GEN_DIR} src)

