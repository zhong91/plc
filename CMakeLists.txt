cmake_minimum_required(VERSION 3.20)

project(plc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(toycc_frontend
    src/parser/parser.cpp
    src/ast/ast_printer.cpp
    src/semantic/semantic_checker.cpp
)

target_include_directories(toycc_frontend PUBLIC
    include
)

add_executable(compiler
    src/main.cpp
)

target_link_libraries(compiler PRIVATE
    toycc_frontend
)

include(CTest)

if(BUILD_TESTING)
    add_executable(semantic_checker_tests
        tests/semantic_checker_tests.cpp
    )

    target_link_libraries(semantic_checker_tests PRIVATE
        toycc_frontend
    )

    add_test(
        NAME semantic_checker_tests
        COMMAND semantic_checker_tests
    )
endif()
