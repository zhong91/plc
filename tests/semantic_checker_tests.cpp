#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "semantic/semantic_checker.h"

namespace {

using namespace toycc;

std::shared_ptr<Block> block(std::vector<StmtPtr> statements) {
    auto result = std::make_shared<Block>();
    result->statements = std::move(statements);
    return result;
}

std::shared_ptr<FunctionDef> function(
    ValueType returnType,
    std::string name,
    std::vector<Param> params,
    std::vector<StmtPtr> statements
) {
    return std::make_shared<FunctionDef>(
        returnType,
        std::move(name),
        std::move(params),
        block(std::move(statements))
    );
}

std::shared_ptr<CompUnit> program(std::vector<ASTNodePtr> units) {
    auto result = std::make_shared<CompUnit>();
    result->units = std::move(units);
    return result;
}

std::shared_ptr<FunctionDef> validMain(std::vector<StmtPtr> statements) {
    return function(ValueType::Int, "main", {}, std::move(statements));
}

ExprPtr number(int value) {
    return std::make_shared<NumberLiteral>(value);
}

ExprPtr variable(std::string name) {
    return std::make_shared<Variable>(std::move(name));
}

ExprPtr binary(std::string op, ExprPtr left, ExprPtr right) {
    return std::make_shared<BinaryExpr>(
        std::move(op),
        std::move(left),
        std::move(right)
    );
}

ExprPtr call(std::string name, std::vector<ExprPtr> args = {}) {
    return std::make_shared<FunctionCall>(std::move(name), std::move(args));
}

StmtPtr ret(ExprPtr value = nullptr) {
    return std::make_shared<ReturnStmt>(std::move(value));
}

StmtPtr decl(bool isConst, std::string name, ExprPtr init) {
    return std::make_shared<VarDeclStmt>(
        isConst,
        std::move(name),
        std::move(init)
    );
}

StmtPtr assign(std::string name, ExprPtr value) {
    return std::make_shared<AssignStmt>(std::move(name), std::move(value));
}

StmtPtr exprStmt(ExprPtr value) {
    return std::make_shared<ExprStmt>(std::move(value));
}

struct TestRunner {
    int passed = 0;
    int failed = 0;

    void expectPass(const std::string& name, const ASTNodePtr& root) {
        try {
            SemanticChecker checker;
            checker.check(root);
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": unexpected error: "
                      << error.what() << '\n';
        }
    }

    void expectFail(
        const std::string& name,
        const ASTNodePtr& root,
        const std::string& expectedText
    ) {
        try {
            SemanticChecker checker;
            checker.check(root);
            ++failed;
            std::cerr << "[FAIL] " << name << ": expected an error containing '"
                      << expectedText << "'.\n";
        } catch (const SemanticError& error) {
            const std::string message = error.what();
            if (message.find(expectedText) == std::string::npos) {
                ++failed;
                std::cerr << "[FAIL] " << name << ": error was '" << message
                          << "', expected text '" << expectedText << "'.\n";
                return;
            }
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": wrong exception type: "
                      << error.what() << '\n';
        }
    }
};

} // namespace

int main() {
    TestRunner tests;

    tests.expectPass(
        "valid globals, calls, loop control, and returns",
        program({
            decl(true, "A", number(1)),
            decl(false, "g", number(2)),
            function(
                ValueType::Int,
                "add",
                {Param(ValueType::Int, "a"), Param(ValueType::Int, "b")},
                {ret(binary("+", variable("a"), variable("b")))}
            ),
            validMain({
                decl(false, "x", number(0)),
                std::make_shared<WhileStmt>(
                    binary("<", variable("x"), number(10)),
                    block({
                        assign("x", binary("+", variable("x"), number(1))),
                        std::make_shared<IfStmt>(
                            binary("==", variable("x"), number(3)),
                            std::make_shared<ContinueStmt>(),
                            nullptr
                        ),
                        std::make_shared<IfStmt>(
                            binary("==", variable("x"), number(5)),
                            std::make_shared<BreakStmt>(),
                            nullptr
                        )
                    })
                ),
                ret(binary(
                    "+",
                    variable("A"),
                    call("add", {variable("x"), variable("g")})
                ))
            })
        })
    );

    tests.expectFail(
        "missing main",
        program({function(ValueType::Void, "helper", {}, {})}),
        "must define an entry function"
    );

    tests.expectFail(
        "main must return int",
        program({function(ValueType::Void, "main", {}, {})}),
        "main function must return int"
    );

    tests.expectFail(
        "main must have no parameters",
        program({function(
            ValueType::Int,
            "main",
            {Param(ValueType::Int, "argc")},
            {ret(number(0))}
        )}),
        "empty parameter list"
    );

    tests.expectFail(
        "duplicate local declaration",
        program({validMain({
            decl(false, "x", number(1)),
            decl(false, "x", number(2)),
            ret(number(0))
        })}),
        "duplicate declaration of 'x'"
    );

    tests.expectPass(
        "nested scope may shadow outer variable",
        program({validMain({
            decl(false, "x", number(1)),
            block({decl(false, "x", number(2))}),
            ret(variable("x"))
        })})
    );

    tests.expectFail(
        "use before declaration",
        program({validMain({
            decl(false, "x", variable("y")),
            decl(false, "y", number(1)),
            ret(variable("x"))
        })}),
        "undefined identifier 'y'"
    );

    tests.expectFail(
        "constant cannot be assigned",
        program({validMain({
            decl(true, "x", number(1)),
            assign("x", number(2)),
            ret(number(0))
        })}),
        "cannot assign to constant 'x'"
    );

    tests.expectFail(
        "constant initializer cannot use variable",
        program({
            decl(false, "g", number(1)),
            decl(true, "A", variable("g")),
            validMain({ret(number(0))})
        }),
        "may only reference previously declared constants"
    );

    tests.expectPass(
        "constant expression honors logical short circuit",
        program({
            decl(
                true,
                "A",
                binary("&&", number(0), binary("/", number(1), number(0)))
            ),
            validMain({ret(variable("A"))})
        })
    );

    tests.expectFail(
        "evaluated division by zero",
        program({validMain({ret(binary("/", number(1), number(0)))})}),
        "division or remainder by zero"
    );

    tests.expectFail(
        "function call before definition",
        program({
            function(ValueType::Int, "first", {}, {ret(call("later"))}),
            function(ValueType::Int, "later", {}, {ret(number(1))}),
            validMain({ret(call("first"))})
        }),
        "call to undefined function 'later'"
    );

    tests.expectPass(
        "direct recursion is allowed",
        program({
            function(
                ValueType::Int,
                "countdown",
                {Param(ValueType::Int, "n")},
                {
                    std::make_shared<IfStmt>(
                        binary("==", variable("n"), number(0)),
                        ret(number(0)),
                        ret(call("countdown", {
                            binary("-", variable("n"), number(1))
                        }))
                    )
                }
            ),
            validMain({ret(call("countdown", {number(3)}))})
        })
    );

    tests.expectFail(
        "function argument count mismatch",
        program({
            function(
                ValueType::Int,
                "id",
                {Param(ValueType::Int, "x")},
                {ret(variable("x"))}
            ),
            validMain({ret(call("id"))})
        }),
        "expects 1 argument"
    );

    tests.expectFail(
        "void call cannot be used as int value",
        program({
            function(ValueType::Void, "log", {}, {}),
            validMain({ret(call("log"))})
        }),
        "requires an int value"
    );

    tests.expectPass(
        "void call is valid as expression statement",
        program({
            function(ValueType::Void, "log", {}, {}),
            validMain({exprStmt(call("log")), ret(number(0))})
        })
    );

    tests.expectFail(
        "void function cannot return a value",
        program({
            function(ValueType::Void, "bad", {}, {ret(number(1))}),
            validMain({ret(number(0))})
        }),
        "cannot return a value"
    );

    tests.expectFail(
        "int function needs a value on every path",
        program({
            function(
                ValueType::Int,
                "maybe",
                {Param(ValueType::Int, "x")},
                {
                    std::make_shared<IfStmt>(
                        variable("x"),
                        ret(number(1)),
                        nullptr
                    )
                }
            ),
            validMain({ret(call("maybe", {number(1)}))})
        }),
        "does not return an int value on every possible execution path"
    );

    tests.expectPass(
        "both if branches return",
        program({validMain({
            std::make_shared<IfStmt>(
                number(1),
                ret(number(1)),
                ret(number(2))
            )
        })})
    );

    tests.expectPass(
        "constant true loop whose body always returns",
        program({validMain({
            std::make_shared<WhileStmt>(number(1), ret(number(7)))
        })})
    );

    tests.expectFail(
        "break outside loop",
        program({validMain({std::make_shared<BreakStmt>(), ret(number(0))})}),
        "only allowed inside a while loop"
    );

    tests.expectFail(
        "continue outside loop",
        program({validMain({std::make_shared<ContinueStmt>(), ret(number(0))})}),
        "only allowed inside a while loop"
    );

    tests.expectFail(
        "duplicate parameter name",
        program({
            function(
                ValueType::Int,
                "f",
                {Param(ValueType::Int, "x"), Param(ValueType::Int, "x")},
                {ret(number(0))}
            ),
            validMain({ret(number(0))})
        }),
        "duplicate declaration of 'x'"
    );

    tests.expectFail(
        "global function and variable share ordinary identifier namespace",
        program({
            decl(false, "f", number(1)),
            function(ValueType::Int, "f", {}, {ret(number(0))}),
            validMain({ret(number(0))})
        }),
        "duplicate declaration of 'f'"
    );

    std::cout << "\n" << tests.passed << " passed, " << tests.failed
              << " failed.\n";
    return tests.failed == 0 ? 0 : 1;
}
