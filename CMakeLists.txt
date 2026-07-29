cmake_minimum_required(VERSION 3.15)
project(Zeta VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
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
set(PARSER_HEADER ${GEN_DIR}/syntax.tab.hpp)

FLEX_TARGET(Scanner src/compiler/zeta.l ${LEXER_OUTPUT})
BISON_TARGET(Parser src/compiler/zeta.y ${PARSER_OUTPUT}
    COMPILE_FLAGS "-d --defines=${PARSER_HEADER}")

add_flex_bison_dependency(Scanner Parser)

file(GLOB CORE_SRC
    src/compiler/*.cpp
    src/compiler/*.h
    src/utils/*.cpp
    src/utils/*.h
    src/vm/*.cpp
    src/vm/*.h
)

set(LIB_SRC ${CORE_SRC} ${LEXER_OUTPUT} ${PARSER_OUTPUT})
set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(zeta_core STATIC ${LIB_SRC})
target_include_directories(zeta_core PRIVATE ${GEN_DIR} src)

set(PUBLIC_INCLUDE_DIR ${CMAKE_BINARY_DIR}/include)
file(MAKE_DIRECTORY ${PUBLIC_INCLUDE_DIR})
set(PUBLIC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/ast.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/bytecode.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/compiler.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/error.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/translate.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/visitor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/utils/utils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/vm.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/value.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/gc.h
)
set(PUBLIC_HEADER_BASE ${CMAKE_CURRENT_SOURCE_DIR}/src)
foreach(HEADER ${PUBLIC_HEADERS})
    file(RELATIVE_PATH REL_PATH ${PUBLIC_HEADER_BASE} ${HEADER})
    set(DEST_FILE ${PUBLIC_INCLUDE_DIR}/${REL_PATH})
    get_filename_component(DEST_DIR ${DEST_FILE} DIRECTORY)
    add_custom_command(
        OUTPUT ${DEST_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${HEADER} ${DEST_FILE}
        DEPENDS ${HEADER}
        COMMENT "Copying public header: ${REL_PATH}"
    )
    target_sources(zeta_core PRIVATE ${DEST_FILE})
endforeach()

target_include_directories(zeta_core PUBLIC ${PUBLIC_INCLUDE_DIR})

add_executable(zeta ${APP_SRC})
target_link_libraries(zeta zeta_core)
