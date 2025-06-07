#include "InstructionParser.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <set>

namespace AsmEngine {

    std::vector<std::string> InstructionParser::TokenizeInstruction(const std::string& instruction) {
        std::vector<std::string> tokens;
        std::string current;
        bool inBrackets = false;
        bool inQuotes = false;

        for (size_t i = 0; i < instruction.length(); ++i) {
            char c = instruction[i];

            if (c == '"' && (i == 0 || instruction[i - 1] != '\\')) {
                inQuotes = !inQuotes;
                current += c;
            }
            else if (!inQuotes) {
                if (c == '[') {
                    inBrackets = true;
                    current += c;
                }
                else if (c == ']') {
                    inBrackets = false;
                    current += c;
                }
                else if ((c == ' ' || c == '\t' || c == ',') && !inBrackets) {
                    if (!current.empty()) {
                        tokens.push_back(current);
                        current.clear();
                    }
                }
                else {
                    current += c;
                }
            }
            else {
                current += c;
            }
        }

        if (!current.empty()) {
            tokens.push_back(current);
        }

        return tokens;
    }

    std::pair<std::string, std::vector<std::string>> InstructionParser::ParseBasic(const std::string& instruction) {
        auto tokens = TokenizeInstruction(instruction);

        if (tokens.empty()) {
            return { "", {} };
        }

        std::string mnemonic = tokens[0];
        std::vector<std::string> operands(tokens.begin() + 1, tokens.end());

        return { mnemonic, operands };
    }

    bool InstructionParser::IsJumpInstruction(const std::string& mnemonic) {
        std::string lower = mnemonic;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        static const std::set<std::string> jumpInstructions = {
            "jmp", "je", "jne", "jz", "jnz", "ja", "jae", "jb", "jbe",
            "jg", "jge", "jl", "jle", "jo", "jno", "js", "jns",
            "jp", "jnp", "jcxz", "jecxz", "jrcxz"
        };

        return jumpInstructions.find(lower) != jumpInstructions.end();
    }

    bool InstructionParser::IsMemoryInstruction(const std::string& mnemonic) {
        std::string lower = mnemonic;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        static const std::set<std::string> memoryInstructions = {
            "mov", "movzx", "movsx", "lea", "push", "pop",
            "add", "sub", "and", "or", "xor", "cmp", "test",
            "inc", "dec", "neg", "not", "mul", "div", "imul", "idiv"
        };

        return memoryInstructions.find(lower) != memoryInstructions.end();
    }

    bool InstructionParser::IsFloatInstruction(const std::string& mnemonic) {
        std::string lower = mnemonic;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        // SSE/SSE2 instructions
        if (lower.find("movss") == 0 || lower.find("movsd") == 0 ||
            lower.find("addss") == 0 || lower.find("subss") == 0 ||
            lower.find("mulss") == 0 || lower.find("divss") == 0 ||
            lower.find("comiss") == 0 || lower.find("ucomiss") == 0) {
            return true;
        }

        // x87 FPU instructions
        if (lower.find("fld") == 0 || lower.find("fst") == 0 ||
            lower.find("fadd") == 0 || lower.find("fsub") == 0 ||
            lower.find("fmul") == 0 || lower.find("fdiv") == 0) {
            return true;
        }

        return false;
    }

    size_t InstructionParser::EstimateInstructionSize(const std::string& mnemonic,
        const std::vector<std::string>& operands) {
        std::string lower = mnemonic;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        // NOP
        if (lower == "nop") {
            if (!operands.empty()) {
                // Multiple NOPs
                try {
                    return std::stoul(operands[0]);
                }
                catch (...) {
                    return 1;
                }
            }
            return 1;
        }

        // Single byte instructions
        if (lower == "ret" && operands.empty()) return 1;
        if (lower == "int3") return 1;
        if (lower == "pushf" || lower == "popf") return 1;

        // Push/pop register
        if ((lower == "push" || lower == "pop") && operands.size() == 1) {
            if (IsRegister(operands[0])) {
                return 1; // 64-bit push/pop
            }
            return 5; // push/pop immediate
        }

        // Jump instructions
        if (IsJumpInstruction(lower)) {
            if (operands.size() == 1 && IsRegister(operands[0])) {
                return 2; // jmp reg
            }
            return 5; // near jump
        }

        // Call instruction
        if (lower == "call") {
            if (operands.size() == 1 && IsRegister(operands[0])) {
                return 2; // call reg
            }
            return 5; // near call
        }

        // MOV instruction
        if (lower == "mov") {
            if (operands.size() == 2) {
                bool hasMemory = IsMemoryOperand(operands[0]) || IsMemoryOperand(operands[1]);
                bool hasImmediate = IsImmediate(operands[1]);

                if (hasMemory && hasImmediate) {
                    return 10; // mov [mem], imm64
                }
                else if (hasMemory) {
                    return 7; // mov reg, [mem] or mov [mem], reg
                }
                else if (hasImmediate) {
                    return 10; // mov reg, imm64
                }
                else {
                    return 3; // mov reg, reg
                }
            }
        }

        // Default estimate
        return 7;
    }

    bool InstructionParser::IsRegister(const std::string& str) {
        std::string lower = str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        static const std::set<std::string> registers = {
            // 64-bit
            "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
            // 32-bit
            "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
            "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
            // 16-bit
            "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
            "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w",
            // 8-bit
            "al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl",
            "ah", "bh", "ch", "dh",
            "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
            // XMM
            "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
            "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
            // MM
            "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7"
        };

        return registers.find(lower) != registers.end();
    }

    bool InstructionParser::IsMemoryOperand(const std::string& str) {
        return !str.empty() && str.front() == '[' && str.back() == ']';
    }

    bool InstructionParser::IsImmediate(const std::string& str) {
        if (str.empty()) return false;

        // Check if it's a register or memory operand first
        if (IsRegister(str) || IsMemoryOperand(str)) {
            return false;
        }

        // Check for hex prefix
        if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            return true;
        }

        // Check for CE style hex
        if (str[0] == '$') {
            return true;
        }

        // Check for float cast
        if (str.find("(float)") == 0) {
            return true;
        }

        // Check if all digits
        return std::all_of(str.begin(), str.end(), [](char c) {
            return std::isdigit(c) || c == '-' || c == '+';
            });
    }

} // namespace AsmEngine