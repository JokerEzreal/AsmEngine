#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <asmjit/x86.h>

namespace AsmEngine {

    class InstructionParser {
    public:
        struct ParsedInstruction {
            std::string mnemonic;
            std::vector<std::string> operands;
        };

        // Parse instruction string
        static ParsedInstruction Parse(const std::string& instruction);

        // Register mapping
        static asmjit::x86::Gp GetGpRegister(const std::string& name);
        static asmjit::x86::Xmm GetXmmRegister(const std::string& name);

        // Parse operand types
        static bool IsRegister(const std::string& operand);
        static bool IsImmediate(const std::string& operand);
        static bool IsMemory(const std::string& operand);

        // Parse immediate values
        static uint64_t ParseImmediate(const std::string& operand);

        // Parse memory operands like [rax+rbx*2+0x10]
        struct MemoryOperand {
            asmjit::x86::Gp base;
            asmjit::x86::Gp index;
            uint32_t scale;
            int32_t displacement;
            bool hasBase;
            bool hasIndex;
        };

        static MemoryOperand ParseMemory(const std::string& operand);

    private:
        static std::unordered_map<std::string, asmjit::x86::Gp> gpRegMap_;
        static std::unordered_map<std::string, asmjit::x86::Xmm> xmmRegMap_;
    };

    // Extended assembler with more instructions
    class ExtendedAssembler {
    public:
        static bool AssembleInstruction(
            asmjit::x86::Assembler& assembler,
            const InstructionParser::ParsedInstruction& parsed);

    private:
        // Instruction handlers
        static bool HandleMov(asmjit::x86::Assembler& assembler,
            const std::vector<std::string>& operands);
        static bool HandleAdd(asmjit::x86::Assembler& assembler,
            const std::vector<std::string>& operands);
        static bool HandleSub(asmjit::x86::Assembler& assembler,
            const std::vector<std::string>& operands);
        static bool HandlePush(asmjit::x86::Assembler& assembler,
            const std::vector<std::string>& operands);
        static bool HandlePop(asmjit::x86::Assembler& assembler,
            const std::vector<std::string>& operands);
        static bool HandleJmp(asmjit::x86::Assembler& assembler,
            const std::vector<std::string>& operands);
        static bool HandleCall(asmjit::x86::Assembler& assembler,
            const std::vector<std::string>& operands);
        static bool HandleLea(asmjit::x86::Assembler& assembler,
            const std::vector<std::string>& operands);
        static bool HandleTest(asmjit::x86::Assembler& assembler,
            const std::vector<std::string>& operands);
        static bool HandleCmp(asmjit::x86::Assembler& assembler,
            const std::vector<std::string>& operands);
        static bool HandleJcc(asmjit::x86::Assembler& assembler,
            const std::string& mnemonic, const std::vector<std::string>& operands);
    };

} // namespace AsmEngine