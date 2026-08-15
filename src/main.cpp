#include <iostream>
#include <sstream>
#include <string>
#include "codegen/riscv_generator.h"
#include "ir/ir_builder.h"
#include "lexer/lexer.h"
#include "opt/compile_time_evaluator.h"
#include "parser/parser.h"
#include "semantic/semantic_checker.h"

int main(int argc, char* argv[]) {
    bool enableOpt = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-opt") {
            enableOpt = true;
        }
    }
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    std::string source = buffer.str();

    try {
        toycc::Lexer lexer(source);
        std::vector<toycc::Token> tokens = lexer.tokenize();

        toycc::Parser parser(tokens);
        toycc::ASTNodePtr ast = parser.parse();

        toycc::SemanticChecker checker;
        checker.check(ast);
        if (enableOpt) {
            // 只给整程序求值一个很小预算；大程序快速回退到真正的后端优化。
            if (auto value = toycc::tryEvaluateMainAtCompileTime(ast, 20000ULL); value.has_value()) {
                toycc::emitConstantMain(std::cout, *value);
                return 0;
            }
        }

        toycc::IRBuilder builder;
        auto program = builder.build(ast, &checker);
        // 功能测试（无 -opt）沿用原后端行为；性能测试（-opt）启用寄存器提升和 spill 窥孔优化。
        toycc::RiscvGenerator generator(std::cout, enableOpt);
        generator.generate(program);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
