#include "InstructionParser.h"
#include <sstream>
#include <algorithm>
#include <regex>

namespace AsmEngine {

    using namespace asmjit;

    // Initialize register maps
    std::unordered_map<std::string, x86::Gp> InstructionParser::gpRegMap_ = {
        // 64-bit registers
        {"rax", x86::rax}, {"rbx", x86::rbx}, {"rcx", x86::rcx}, {"rdx", x86::rdx},
        {"rsi", x86::rsi}, {"rdi", x86::rdi}, {"rbp", x86::rbp}, {"rsp", x86::rsp},
        {"r8", x86::r8}, {"r9", x86::r9}, {"r10", x86::r10}, {"r11", x86::r11},
        {"r12", x86::r12}, {"r13", x86::r13}, {"r14", x86::r14}, {"r15", x86::r15},

        // 32-bit registers
        {"eax", x86::eax}, {"ebx", x86::ebx}, {"ecx", x86::ecx}, {"edx", x86::edx},
        {"esi", x86::esi}, {"edi", x86::edi}, {"ebp", x86::ebp}, {"esp", x86::esp},
        {"r8d", x86::r8d}, {"r9d", x86::r9d}, {"r10d", x86::r10d}, {"r11d", x86::r11d},
        {"r12d", x86::r12d}, {"r13d", x86::r13d}, {"r14d", x86::r14d}, {"r15d", x86::r15d},

        // 16-bit registers
        {"ax", x86::ax}, {"bx", x86::bx}, {"cx", x86::cx}, {"dx", x86::dx},
        {"si", x86::si}, {"di", x86::di}, {"bp", x86::bp}, {"sp", x86::sp},

        // 8-bit registers
        {"al", x86::al}, {"bl", x86::bl}, {"cl", x86::cl}, {"dl", x86::dl},
        {"ah", x86::ah}, {"bh", x86::bh}, {"ch", x86::ch}, {"dh", x86::dh},
    };

    std::unordered_map<std::string, x86::Xmm> InstructionParser::xmmRegMap_ = {
        {"xmm0", x86::xmm0}, {"xmm1", x86::xmm1}, {"xmm2", x86::xmm2}, {"xmm3", x86::xmm3},
        {"xmm4", x86::xmm4}, {"xmm5", x86::xmm5}, {"xmm6", x86::xmm6}, {"xmm7", x86::xmm7},
        {"xmm8", x86::xmm8}, {"xmm9", x86::xmm9}, {"xmm10", x86::xmm10}, {"xmm11", x86::xmm11},
        {"xmm12", x86::xmm12}, {"xmm13", x86::xmm13}, {"xmm14", x86::xmm14}, {"xmm15", x86::xmm15},
    };

    InstructionParser::ParsedInstruction InstructionParser::Parse(const std::string& instruction) {
        ParsedInstruction result;

        std::string cleaned = instruction;
        // Remove commas
        std::replace(cleaned.begin(), cleaned.end(), ',', ' ');

        std::istringstream iss(cleaned);
        iss >> result.mnemonic;

        // Convert mnemonic to lowercase
        std::transform(result.mnemonic.begin(), result.mnemonic.end(),
            result.mnemonic.begin(), ::tolower);

        std::string operand;
        while (iss >> operand) {
            result.operands.push_back(operand);
        }

        return result;
    }

    x86::Gp InstructionParser::GetGpRegister(const std::string& name) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        auto it = gpRegMap_.find(lower);
        if (it != gpRegMap_.end()) {
            return it->second;
        }

        return x86::Gp(); // Invalid register
    }

    x86::Xmm InstructionParser::GetXmmRegister(const std::string& name) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        auto it = xmmRegMap_.find(lower);
        if (it != xmmRegMap_.end()) {
            return it->second;
        }

        return x86::Xmm(); // Invalid register
    }

    bool InstructionParser::IsRegister(const std::string& operand) {
        std::string lower = operand;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        return gpRegMap_.find(lower) != gpRegMap_.end() ||
            xmmRegMap_.find(lower) != xmmRegMap_.end();
    }

    bool InstructionParser::IsImmediate(const std::string& operand) {
        if (operand.empty()) return false;

        // Check for hex (0x prefix)
        if (operand.size() > 2 && operand[0] == '0' &&
            (operand[1] == 'x' || operand[1] == 'X')) {
            return true;
        }

        // Check for decimal
        return std::all_of(operand.begin(), operand.end(), ::isdigit) ||
            (operand[0] == '-' && operand.size() > 1 &&
                std::all_of(operand.begin() + 1, operand.end(), ::isdigit));
    }

    bool InstructionParser::IsMemory(const std::string& operand) {
        return operand.front() == '[' && operand.back() == ']';
    }

    uint64_t InstructionParser::ParseImmediate(const std::string& operand) {
        if (operand.size() > 2 && operand[0] == '0' &&
            (operand[1] == 'x' || operand[1] == 'X')) {
            return std::stoull(operand, nullptr, 16);
        }
        return std::stoull(operand, nullptr, 10);
    }

    InstructionParser::MemoryOperand InstructionParser::ParseMemory(const std::string& operand) {
        MemoryOperand mem = {};
        mem.scale = 1;
        mem.displacement = 0;
        mem.hasBase = false;
        mem.hasIndex = false;

        // Remove brackets
        std::string expr = operand.substr(1, operand.length() - 2);

        // Simple parser for [base+index*scale+disp] format
        // This is a simplified version - real implementation would need better parsing

        // For now, just handle simple cases like [rax], [rax+8], [rax+rbx*2+8]
        std::regex simpleReg(R"(^(\w+)$)");
        std::regex regPlusDisp(R"(^(\w+)\s*([+-])\s*(\d+)$)");

        std::smatch match;

        if (std::regex_match(expr, match, simpleReg)) {
            // [reg]
            mem.base = GetGpRegister(match[1].str());
            mem.hasBase = true;
        }
        else if (std::regex_match(expr, match, regPlusDisp)) {
            // [reg+disp]
            mem.base = GetGpRegister(match[1].str());
            mem.hasBase = true;
            mem.displacement = std::stoi(match[3].str());
            if (match[2].str() == "-") {
                mem.displacement = -mem.displacement;
            }
        }
        // Add more complex parsing as needed

        return mem;
    }

    // ExtendedAssembler implementation
    bool ExtendedAssembler::AssembleInstruction(
        x86::Assembler& assembler,
        const InstructionParser::ParsedInstruction& parsed) {

        const std::string& mnemonic = parsed.mnemonic;
        const std::vector<std::string>& operands = parsed.operands;

        // Basic instructions
        if (mnemonic == "nop") {
            assembler.nop();
            return true;
        }
        else if (mnemonic == "ret") {
            assembler.ret();
            return true;
        }
        else if (mnemonic == "mov") {
            return HandleMov(assembler, operands);
        }
        else if (mnemonic == "add") {
            return HandleAdd(assembler, operands);
        }
        else if (mnemonic == "sub") {
            return HandleSub(assembler, operands);
        }
        else if (mnemonic == "push") {
            return HandlePush(assembler, operands);
        }
        else if (mnemonic == "pop") {
            return HandlePop(assembler, operands);
        }
        else if (mnemonic == "jmp") {
            return HandleJmp(assembler, operands);
        }
        else if (mnemonic == "call") {
            return HandleCall(assembler, operands);
        }
        else if (mnemonic == "lea") {
            return HandleLea(assembler, operands);
        }
        else if (mnemonic == "test") {
            return HandleTest(assembler, operands);
        }
        else if (mnemonic == "cmp") {
            return HandleCmp(assembler, operands);
        }
        // Conditional jumps
        else if (mnemonic == "je" || mnemonic == "jz" ||
            mnemonic == "jne" || mnemonic == "jnz" ||
            mnemonic == "jg" || mnemonic == "jge" ||
            mnemonic == "jl" || mnemonic == "jle" ||
            mnemonic == "ja" || mnemonic == "jae" ||
            mnemonic == "jb" || mnemonic == "jbe") {
            return HandleJcc(assembler, mnemonic, operands);
        }

        return false;
    }

    bool ExtendedAssembler::HandleMov(x86::Assembler& assembler,
        const std::vector<std::string>& operands) {

        if (operands.size() != 2) return false;

        const std::string& dest = operands[0];
        const std::string& src = operands[1];

        // mov reg, reg
        if (InstructionParser::IsRegister(dest) && InstructionParser::IsRegister(src)) {
            auto destReg = InstructionParser::GetGpRegister(dest);
            auto srcReg = InstructionParser::GetGpRegister(src);
            if (destReg.isValid() && srcReg.isValid()) {
                assembler.mov(destReg, srcReg);
                return true;
            }
        }
        // mov reg, imm
        else if (InstructionParser::IsRegister(dest) && InstructionParser::IsImmediate(src)) {
            auto destReg = InstructionParser::GetGpRegister(dest);
            if (destReg.isValid()) {
                uint64_t value = InstructionParser::ParseImmediate(src);
                assembler.mov(destReg, value);
                return true;
            }
        }
        // mov reg, [mem]
        else if (InstructionParser::IsRegister(dest) && InstructionParser::IsMemory(src)) {
            auto destReg = InstructionParser::GetGpRegister(dest);
            auto mem = InstructionParser::ParseMemory(src);
            if (destReg.isValid() && mem.hasBase) {
                if (mem.displacement != 0) {
                    assembler.mov(destReg, x86::ptr(mem.base, mem.displacement));
                }
                else {
                    assembler.mov(destReg, x86::ptr(mem.base));
                }
                return true;
            }
        }
        // mov [mem], reg
        else if (InstructionParser::IsMemory(dest) && InstructionParser::IsRegister(src)) {
            auto srcReg = InstructionParser::GetGpRegister(src);
            auto mem = InstructionParser::ParseMemory(dest);
            if (srcReg.isValid() && mem.hasBase) {
                if (mem.displacement != 0) {
                    assembler.mov(x86::ptr(mem.base, mem.displacement), srcReg);
                }
                else {
                    assembler.mov(x86::ptr(mem.base), srcReg);
                }
                return true;
            }
        }

        return false;
    }

    bool ExtendedAssembler::HandleAdd(x86::Assembler& assembler,
        const std::vector<std::string>& operands) {

        if (operands.size() != 2) return false;

        const std::string& dest = operands[0];
        const std::string& src = operands[1];

        // add reg, reg
        if (InstructionParser::IsRegister(dest) && InstructionParser::IsRegister(src)) {
            auto destReg = InstructionParser::GetGpRegister(dest);
            auto srcReg = InstructionParser::GetGpRegister(src);
            if (destReg.isValid() && srcReg.isValid()) {
                assembler.add(destReg, srcReg);
                return true;
            }
        }
        // add reg, imm
        else if (InstructionParser::IsRegister(dest) && InstructionParser::IsImmediate(src)) {
            auto destReg = InstructionParser::GetGpRegister(dest);
            if (destReg.isValid()) {
                uint64_t value = InstructionParser::ParseImmediate(src);
                assembler.add(destReg, value);
                return true;
            }
        }

        return false;
    }

    bool ExtendedAssembler::HandleSub(x86::Assembler& assembler,
        const std::vector<std::string>& operands) {

        if (operands.size() != 2) return false;

        const std::string& dest = operands[0];
        const std::string& src = operands[1];

        // sub reg, reg
        if (InstructionParser::IsRegister(dest) && InstructionParser::IsRegister(src)) {
            auto destReg = InstructionParser::GetGpRegister(dest);
            auto srcReg = InstructionParser::GetGpRegister(src);
            if (destReg.isValid() && srcReg.isValid()) {
                assembler.sub(destReg, srcReg);
                return true;
            }
        }
        // sub reg, imm
        else if (InstructionParser::IsRegister(dest) && InstructionParser::IsImmediate(src)) {
            auto destReg = InstructionParser::GetGpRegister(dest);
            if (destReg.isValid()) {
                uint64_t value = InstructionParser::ParseImmediate(src);
                assembler.sub(destReg, value);
                return true;
            }
        }

        return false;
    }

    bool ExtendedAssembler::HandlePush(x86::Assembler& assembler,
        const std::vector<std::string>& operands) {

        if (operands.size() != 1) return false;

        const std::string& src = operands[0];

        if (InstructionParser::IsRegister(src)) {
            auto reg = InstructionParser::GetGpRegister(src);
            if (reg.isValid()) {
                assembler.push(reg);
                return true;
            }
        }
        else if (InstructionParser::IsImmediate(src)) {
            uint64_t value = InstructionParser::ParseImmediate(src);
            assembler.push(value);
            return true;
        }

        return false;
    }

    bool ExtendedAssembler::HandlePop(x86::Assembler& assembler,
        const std::vector<std::string>& operands) {

        if (operands.size() != 1) return false;

        const std::string& dest = operands[0];

        if (InstructionParser::IsRegister(dest)) {
            auto reg = InstructionParser::GetGpRegister(dest);
            if (reg.isValid()) {
                assembler.pop(reg);
                return true;
            }
        }

        return false;
    }

    bool ExtendedAssembler::HandleJmp(x86::Assembler& assembler,
        const std::vector<std::string>& operands) {

        if (operands.size() != 1) return false;

        const std::string& target = operands[0];

        if (InstructionParser::IsRegister(target)) {
            auto reg = InstructionParser::GetGpRegister(target);
            if (reg.isValid()) {
                assembler.jmp(reg);
                return true;
            }
        }
        // For absolute addresses, use indirect jump
        else if (InstructionParser::IsImmediate(target)) {
            uint64_t addr = InstructionParser::ParseImmediate(target);
            assembler.mov(x86::rax, addr);
            assembler.jmp(x86::rax);
            return true;
        }

        return false;
    }

    bool ExtendedAssembler::HandleCall(x86::Assembler& assembler,
        const std::vector<std::string>& operands) {

        if (operands.size() != 1) return false;

        const std::string& target = operands[0];

        if (InstructionParser::IsRegister(target)) {
            auto reg = InstructionParser::GetGpRegister(target);
            if (reg.isValid()) {
                assembler.call(reg);
                return true;
            }
        }
        // For absolute addresses, use indirect call
        else if (InstructionParser::IsImmediate(target)) {
            uint64_t addr = InstructionParser::ParseImmediate(target);
            assembler.mov(x86::rax, addr);
            assembler.call(x86::rax);
            return true;
        }

        return false;
    }

    bool ExtendedAssembler::HandleLea(x86::Assembler& assembler,
        const std::vector<std::string>& operands) {

        if (operands.size() != 2) return false;

        const std::string& dest = operands[0];
        const std::string& src = operands[1];

        if (InstructionParser::IsRegister(dest) && InstructionParser::IsMemory(src)) {
            auto destReg = InstructionParser::GetGpRegister(dest);
            auto mem = InstructionParser::ParseMemory(src);
            if (destReg.isValid() && mem.hasBase) {
                if (mem.displacement != 0) {
                    assembler.lea(destReg, x86::ptr(mem.base, mem.displacement));
                }
                else {
                    assembler.lea(destReg, x86::ptr(mem.base));
                }
                return true;
            }
        }

        return false;
    }

    bool ExtendedAssembler::HandleTest(x86::Assembler& assembler,
        const std::vector<std::string>& operands) {

        if (operands.size() != 2) return false;

        const std::string& op1 = operands[0];
        const std::string& op2 = operands[1];

        if (InstructionParser::IsRegister(op1) && InstructionParser::IsRegister(op2)) {
            auto reg1 = InstructionParser::GetGpRegister(op1);
            auto reg2 = InstructionParser::GetGpRegister(op2);
            if (reg1.isValid() && reg2.isValid()) {
                assembler.test(reg1, reg2);
                return true;
            }
        }
        else if (InstructionParser::IsRegister(op1) && InstructionParser::IsImmediate(op2)) {
            auto reg = InstructionParser::GetGpRegister(op1);
            if (reg.isValid()) {
                uint64_t value = InstructionParser::ParseImmediate(op2);
                assembler.test(reg, value);
                return true;
            }
        }

        return false;
    }

    bool ExtendedAssembler::HandleCmp(x86::Assembler& assembler,
        const std::vector<std::string>& operands) {

        if (operands.size() != 2) return false;

        const std::string& op1 = operands[0];
        const std::string& op2 = operands[1];

        if (InstructionParser::IsRegister(op1) && InstructionParser::IsRegister(op2)) {
            auto reg1 = InstructionParser::GetGpRegister(op1);
            auto reg2 = InstructionParser::GetGpRegister(op2);
            if (reg1.isValid() && reg2.isValid()) {
                assembler.cmp(reg1, reg2);
                return true;
            }
        }
        else if (InstructionParser::IsRegister(op1) && InstructionParser::IsImmediate(op2)) {
            auto reg = InstructionParser::GetGpRegister(op1);
            if (reg.isValid()) {
                uint64_t value = InstructionParser::ParseImmediate(op2);
                assembler.cmp(reg, value);
                return true;
            }
        }

        return false;
    }

    bool ExtendedAssembler::HandleJcc(x86::Assembler& assembler,
        const std::string& mnemonic, const std::vector<std::string>& operands) {

        if (operands.size() != 1) return false;

        // For now, conditional jumps to absolute addresses need labels
        // AsmJit doesn't support direct conditional jumps to absolute addresses
        // You'd need to use the label system or implement a workaround

        return false;
    }

} // namespace AsmEngine