#include <iostream>
#include <sstream>
#include <string>

#include "ast/ast_printer.h"
#include "codegen/riscv_generator.h"
#include "ir/ir_builder.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "semantic/semantic_checker.h"

int main(int argc, char* argv[]) {
    // 支持 -opt 标志：目前未实现优化，仅保证评测时不报错
    bool enableOpt = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-opt") {
            enableOpt = true;
        }
    }
    (void)enableOpt;  // 预留：后续可在此处启用优化通道

    // 1. 从 stdin 读取全部 ToyC 源代码
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    std::string source = buffer.str();

    try {
        // 2. Lexer：源代码 -> Token 序列
        toycc::Lexer lexer(source);
        std::vector<toycc::Token> tokens = lexer.tokenize();

        // 3. Parser：Token 序列 -> AST
        toycc::Parser parser(tokens);
        toycc::ASTNodePtr ast = parser.parse();
        toycc::ASTPrinter::print(ast, std::cerr);  // 调试信息

        // 4. Semantic：语义检查
        toycc::SemanticChecker checker;
        checker.check(ast);
        std::cerr << "[DEBUG] Semantic check passed\n";

        // 5. IR 生成
        toycc::IRBuilder builder;
        auto program = builder.build(ast, &checker);  // 传入 checker
        std::cerr << "[DEBUG] IR generation finished\n";

        // 6. RISC-V 汇编生成（输出到 stdout）
        toycc::RiscvGenerator generator(std::cout);
        generator.generate(program);
        std::cerr << "[DEBUG] Assembly generation finished\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
