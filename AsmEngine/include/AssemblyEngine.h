#pragma once

#include "Common.h"
#include "SymbolManager.h"
#include "CaptureStorage.h"
#include <asmjit/asmjit.h>
#include <memory>

namespace AsmEngine {

    // Assembly context for managing labels and symbols during assembly
    class AssemblyContext {
    public:
        asmjit::CodeHolder code;
        asmjit::x86::Assembler assembler;
        std::unordered_map<std::string, asmjit::Label> labels;
        std::unordered_map<std::string, AddressType> resolvedAddresses;
        AddressType baseAddress;

        AssemblyContext(asmjit::JitRuntime* runtime, AddressType base = 0);

        // Label management
        asmjit::Label GetOrCreateLabel(const std::string& name);
        void BindLabel(const std::string& name);

        // Symbol resolution
        asmjit::Imm ResolveImmediate(const std::string& value);
    };

    // Assembled code result
    struct AssembledCode {
        ByteVector machineCode;
        std::unordered_map<std::string, AddressType> labels;
        size_t codeSize = 0;
    };

    class AssemblyEngine {
    private:
        std::unique_ptr<asmjit::JitRuntime> runtime_;
        SymbolManager* symbolManager_;
        CaptureStorage* captureStorage_;

        // Enhanced parsing with asmjit
        bool ParseAndAssembleInstruction(
            AssemblyContext& ctx,
            const std::string& instruction);

        // Memory operand parsing using asmjit
        asmjit::x86::Mem ParseMemoryOperand(
            AssemblyContext& ctx,
            const std::string& memExpr);

        // Register parsing using asmjit
        asmjit::x86::Gp ParseGpRegister(const std::string& regName);
        asmjit::x86::Xmm ParseXmmRegister(const std::string& regName);
        asmjit::x86::Mm ParseMmRegister(const std::string& regName);

        // Immediate value resolution
        asmjit::Imm ResolveImmediate(
            AssemblyContext& ctx,
            const std::string& value);

        // Enhanced preprocessing
        std::string PreprocessLine(
            const std::string& line,
            AssemblyContext& ctx) const;

        // Replace capture references
        std::string ReplaceCaptureReferences(const std::string& line) const;

        // Replace symbol references
        std::string ReplaceSymbolReferences(
            const std::string& line,
            AssemblyContext& ctx) const;

        // Parse scale factor in memory expressions
        uint32_t ParseScale(const std::string& scaleStr);

        // Handle data directives
        bool HandleDataDirective(
            AssemblyContext& ctx,
            const std::string& directive,
            const std::vector<std::string>& values);

        // Handle special instructions
        bool HandleSpecialInstruction(
            AssemblyContext& ctx,
            const std::string& mnemonic,
            const std::vector<std::string>& operands);

    public:
        AssemblyEngine(SymbolManager* symbolManager, CaptureStorage* captureStorage);
        ~AssemblyEngine();

        // Get runtime for direct access if needed
        asmjit::JitRuntime* GetRuntime() { return runtime_.get(); }

        // Main assembly functions
        std::optional<AssembledCode> Assemble(
            const std::string& assembly,
            AddressType address = 0);

        std::optional<ByteVector> AssembleInstruction(
            const std::string& instruction,
            AddressType address = 0);

        // Code generation utilities using asmjit
        ByteVector GenerateNop(size_t count);
        ByteVector GenerateJump(AddressType from, AddressType to);
        ByteVector GenerateCall(AddressType from, AddressType to);
        ByteVector GenerateDetour(
            AddressType from,
            AddressType to,
            size_t& trampolineSize);

        // Hook generation with asmjit
        struct HookInfo {
            AddressType targetAddress;
            AddressType hookAddress;
            ByteVector originalBytes;
            ByteVector hookBytes;
            AddressType trampolineAddress;
        };

        std::optional<HookInfo> CreateHook(
            AddressType targetAddress,
            const std::string& hookCode);

        // Generate push/pop for all registers
        ByteVector GeneratePushAll();
        ByteVector GeneratePopAll();

        // Generate function prologue/epilogue
        ByteVector GeneratePrologue(size_t stackSpace = 0);
        ByteVector GenerateEpilogue();
    };

} // namespace AsmEngine