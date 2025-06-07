#pragma once

#include "Common.h"
#include "SymbolManager.h"
#include "CaptureStorage.h"
#include <asmjit/asmjit.h>
#include <memory>
#include <map>
namespace AsmEngine {

    // Assembly instruction info
    struct AssemblyInstruction {
        std::string mnemonic;
        std::vector<std::string> operands;
        AddressType address;
        ByteVector machineCode;
    };

    // Assembled code result
    struct AssembledCode {
        ByteVector machineCode;
        std::unordered_map<std::string, AddressType> labels;
        std::vector<AssemblyInstruction> instructions;
        size_t codeSize = 0;
    };

    class AssemblyEngine {
    private:
        // AsmJit instruction assembly
        bool AssembleInstructionAsmJit(asmjit::x86::Assembler& assembler,
            const std::string& instruction,
            std::map<std::string, asmjit::Label>& labelMap,
            std::vector<std::pair<size_t, std::string>>& jumpFixups);

        // Register parsing
        asmjit::x86::Gp ParseRegister(const std::string& str);
        asmjit::x86::Xmm ParseXmmRegister(const std::string& str);

        // Memory and immediate parsing
        asmjit::x86::Mem ParseMemory(const std::string& str);
        uint64_t ParseImmediate(const std::string& str);

        // Instruction handlers
        bool HandleMov(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands);
        bool HandleMovss(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands);
        bool HandleLea(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands);
        bool HandleAdd(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands);
        bool HandleSub(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands);
        bool HandleTest(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands);
        bool HandleCmp(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands);
        bool HandlePush(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands);
        bool HandlePop(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands);
        bool HandleJmp(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands,
            std::map<std::string, asmjit::Label>& labelMap);
        bool HandleCall(asmjit::x86::Assembler& assembler, const std::vector<std::string>& operands,
            std::map<std::string, asmjit::Label>& labelMap);

    public:
        AssemblyEngine(SymbolManager* symbolManager, CaptureStorage* captureStorage);
        ~AssemblyEngine();

        // Assemble code at specific address
        std::optional<AssembledCode> Assemble(const std::string& assembly,
            AddressType address = 0);

        // Assemble single instruction
        std::optional<ByteVector> AssembleInstruction(const std::string& instruction,
            AddressType address = 0);

        // Disassemble machine code
        std::vector<AssemblyInstruction> Disassemble(const ByteVector& machineCode,
            AddressType address = 0);

        // Generate common code patterns
        ByteVector GenerateNop(size_t count);
        ByteVector GenerateJump(AddressType from, AddressType to);
        ByteVector GenerateCall(AddressType from, AddressType to);
        ByteVector GenerateDetour(AddressType from, AddressType to,
            AddressType& trampolineSize);

        // Code cave utilities
        struct CodeCave {
            AddressType address;
            size_t size;
            ByteVector originalBytes;
        };

        std::optional<CodeCave> FindCodeCave(AddressType nearAddress,
            size_t minSize) const;

        // Hook generation
        struct HookInfo {
            AddressType targetAddress;
            AddressType hookAddress;
            ByteVector originalBytes;
            ByteVector hookBytes;
            AddressType trampolineAddress;
        };

        std::optional<HookInfo> CreateHook(AddressType targetAddress,
            const std::string& hookCode);

        // Inline assembly execution (for testing)
        std::optional<std::vector<uint64_t>> ExecuteAssembly(
            const std::string& assembly,
            const std::vector<uint64_t>& parameters = {});
    };

} // namespace AsmEngine