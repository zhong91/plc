#pragma once
#include <iostream>
#include "ir/ir.h"

namespace toycc {

class RiscvGenerator {
private:
    std::ostream& out;
    void emitLine(const std::string& s) { out << s << "\n"; }
    void emitInstr(const IRInstr& instr);

public:
    explicit RiscvGenerator(std::ostream& os) : out(os) {}
    void generate(const IRProgram& program);
};

} // namespace toycc