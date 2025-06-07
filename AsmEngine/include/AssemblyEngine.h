#pragma once

#include "Common.h"
#include "SymbolManager.h"
#include "CaptureStorage.h"
#include <asmjit/asmjit.h>
#include <memory>

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
        std::unique_ptr<asmjit::JitRuntime> runtime_;
        SymbolManager* symbolManager_;
        CaptureStorage* captureStorage_;

        // Preprocess assembly to resolve symbols and captures
        std::string PreprocessAssembly(const std::string& assembly,
            AddressType baseAddress) const;

        // Replace capture references with actual values
        std::string ReplaceCaptureReferences(const std::string& line) const;

        std::string ProcessMemoryToken(const std::string& token) const;
        bool IsRegisterName(const std::string& token) const;
        std::string FixMemoryExpression(const std::string& expr) const;

        // Replace symbol references with addresses
        std::string ReplaceSymbolReferences(const std::string& line) const;

        // Extract labels from assembly
        std::vector<std::pair<std::string, size_t>> ExtractLabels(
            const std::string& assembly) const;

        // Parse and assemble single instruction using AsmJit
        std::optional<ByteVector> AssembleWithAsmJit(const std::string& instruction,
            AddressType address = 0);

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