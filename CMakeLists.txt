cmake_minimum_required(VERSION 3.15)
project(Zeta VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC)
    add_compile_options(/utf-8)
endif()

if(WIN32)
    set(WINFLEXBISON_DIR ${CMAKE_SOURCE_DIR}/tools/win_flex_bison)
    find_program(FLEX_EXECUTABLE win_flex.exe PATHS ${WINFLEXBISON_DIR} NO_DEFAULT_PATH REQUIRED)
    find_program(BISON_EXECUTABLE win_bison.exe PATHS ${WINFLEXBISON_DIR} NO_DEFAULT_PATH REQUIRED)
else()
    find_program(FLEX_EXECUTABLE flex REQUIRED)
    find_program(BISON_EXECUTABLE bison REQUIRED)
endif()

set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})
set(LEXER_OUTPUT ${GEN_DIR}/lex.yy.cpp)
set(PARSER_OUTPUT ${GEN_DIR}/syntax.tab.cpp)
set(PARSER_HEADER ${GEN_DIR}/syntax.tab.hpp)

add_custom_command(
    OUTPUT ${LEXER_OUTPUT}
    COMMAND ${FLEX_EXECUTABLE} -o ${LEXER_OUTPUT} ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/zeta.l
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/zeta.l
    COMMENT "Running Flex to generate lexer"
    VERBATIM
)

add_custom_command(
    OUTPUT ${PARSER_OUTPUT} ${PARSER_HEADER}
    COMMAND ${BISON_EXECUTABLE} -d --defines=${PARSER_HEADER} -o ${PARSER_OUTPUT} ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/zeta.y
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/zeta.y
    COMMENT "Running Bison to generate parser"
    VERBATIM
)

file(GLOB CORE_SRC
    src/compiler/*.cpp
    src/utils/*.cpp
    src/vm/*.cpp
)

set(LIB_SRC src/compiler/compiler.cpp ${CORE_SRC} ${LEXER_OUTPUT} ${PARSER_OUTPUT})
set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config.h.in
    ${GEN_DIR}/config.h
    @ONLY
)

add_library(zeta_core STATIC ${LIB_SRC})
target_include_directories(zeta_core PRIVATE ${GEN_DIR} src)
target_compile_options(zeta_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>)

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
    set(DEST_FILE ${PUBLIC_INCLUDE_DIR}/zeta/${REL_PATH})
    get_filename_component(DEST_DIR ${DEST_FILE} DIRECTORY)
    add_custom_command(
        TARGET zeta_core PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${HEADER} ${DEST_FILE}
        # DEPENDS ${HEADER}
        COMMENT "Copying public header: ${REL_PATH}"
    )
endforeach()
add_custom_command(
    TARGET zeta_core PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GEN_DIR}/config.h ${PUBLIC_INCLUDE_DIR}/zeta/config.h
    # DEPENDS ${GEN_DIR}/config.h
)

target_include_directories(zeta_core PUBLIC ${PUBLIC_INCLUDE_DIR})

add_executable(zeta ${APP_SRC})
target_link_libraries(zeta zeta_core)
