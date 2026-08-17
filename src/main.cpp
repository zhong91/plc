#include <iostream>
#include <sstream>
#include <string>

#include "codegen/riscv_generator.h"
#include "ir/ir_builder.h"
#include "lexer/lexer.h"
#include "opt/ir_optimizer.h"
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

        toycc::IRBuilder builder;
        auto program = builder.build(ast, &checker);

        if (enableOpt) {
            toycc::IROptimizer optimizer;
            optimizer.optimize(program);
        }

        toycc::RiscvGenerator generator(std::cout, enableOpt);
        generator.generate(program);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
