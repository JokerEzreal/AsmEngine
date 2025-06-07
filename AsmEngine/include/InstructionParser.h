#pragma once

#include <string>
#include <vector>
#include <asmjit/x86.h>

namespace AsmEngine {

    // Simplified instruction parser - most functionality moved to AssemblyEngine
    class InstructionParser {
    public:
        // Simple tokenization for preprocessing
        static std::vector<std::string> TokenizeInstruction(const std::string& instruction);

        // Extract mnemonic and operands
        static std::pair<std::string, std::vector<std::string>> ParseBasic(const std::string& instruction);

        // Helper to check instruction type
        static bool IsJumpInstruction(const std::string& mnemonic);
        static bool IsMemoryInstruction(const std::string& mnemonic);
        static bool IsFloatInstruction(const std::string& mnemonic);

        // Size estimation for instruction (rough estimate for pre-allocation)
        static size_t EstimateInstructionSize(const std::string& mnemonic,
            const std::vector<std::string>& operands);

        // Check if string is a valid register name
        static bool IsRegister(const std::string& str);

        // Check if string is a memory operand
        static bool IsMemoryOperand(const std::string& str);

        // Check if string is an immediate value
        static bool IsImmediate(const std::string& str);
    };

} // namespace AsmEngine