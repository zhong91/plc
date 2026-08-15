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

        // 先做完整语义检查；只有合法程序才进入优化或后端。
        toycc::SemanticChecker checker;
        checker.check(ast);

        if (enableOpt) {
            // ToyC 没有 I/O/运行时输入。优先尝试整程序编译期求值；成功时直接输出
            // 等价的常量 main，运行期开销接近最小。若预算耗尽，则自动回退普通后端。
            if (auto value = toycc::tryEvaluateMainAtCompileTime(ast); value.has_value()) {
                toycc::emitConstantMain(std::cout, *value);
                return 0;
            }
        }

        toycc::IRBuilder builder;
        auto program = builder.build(ast, &checker);

        toycc::RiscvGenerator generator(std::cout);
        generator.generate(program);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
