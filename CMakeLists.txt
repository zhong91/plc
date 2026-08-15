cmake_minimum_required(VERSION 3.20)

project(plc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(toycc_frontend
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/ast/ast_printer.cpp
    src/semantic/semantic_checker.cpp
    src/ir/ir_builder.cpp
    src/codegen/riscv_generator.cpp
    src/opt/compile_time_evaluator.cpp
    src/opt/ir_compile_time_evaluator.cpp
    src/opt/ir_optimizer.cpp
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

# The compiler itself is outside the runtime performance score.  Building the
# host compiler with optimization makes bounded compile-time execution much
# faster without changing non--opt ToyC semantics or generated fallback code.
if(MSVC)
    target_compile_options(toycc_frontend PRIVATE /O2)
    target_compile_options(compiler PRIVATE /O2)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(toycc_frontend PRIVATE -O3 -DNDEBUG)
    target_compile_options(compiler PRIVATE -O3 -DNDEBUG)
endif()

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
