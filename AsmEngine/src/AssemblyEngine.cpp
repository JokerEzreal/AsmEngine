#include "AssemblyEngine.h"
#include <regex>
#include <sstream>

namespace AsmEngine {

    AssemblyEngine::AssemblyEngine(SymbolManager* symbolManager,
        CaptureStorage* captureStorage)
        : ksEngine_(nullptr), symbolManager_(symbolManager),
        captureStorage_(captureStorage) {

        // Initialize Keystone for x64
        ks_err err = ks_open(KS_ARCH_X86, KS_MODE_64, &ksEngine_);
        if (err != KS_ERR_OK) {
            throw EngineException(ErrorCode::AssemblyError,
                "Failed to initialize Keystone engine: " +
                std::string(ks_strerror(err)));
        }

        // Set syntax to NASM
        ks_option(ksEngine_, KS_OPT_SYNTAX, KS_OPT_SYNTAX_NASM);
    }

    AssemblyEngine::~AssemblyEngine() {
        if (ksEngine_) {
            ks_close(ksEngine_);
        }
    }

    std::string AssemblyEngine::PreprocessAssembly(const std::string& assembly,
        AddressType baseAddress) const {
        std::string processed = assembly;

        // Replace capture references
        processed = ReplaceCaptureReferences(processed);

        // Replace symbol references
        processed = ReplaceSymbolReferences(processed);

        return processed;
    }

    std::string AssemblyEngine::ReplaceCaptureReferences(const std::string& line) const {
        if (!captureStorage_) {
            return line;
        }

        std::string result = line;

        // Regex to match capture references like s1, sr1, etc.
        // Matches word boundaries to avoid replacing parts of other identifiers
        std::regex captureRegex(R"(\b([a-zA-Z]\w*)\b)");

        // Get all capture names
        auto captureNames = captureStorage_->GetAllNames();

        // Replace each capture reference
        for (const auto& captureName : captureNames) {
            std::regex specificCapture(R"(\b)" + captureName + R"(\b)");
            std::string replacement = captureStorage_->ResolveReference(captureName);
            result = std::regex_replace(result, specificCapture, replacement);
        }

        return result;
    }

    std::string AssemblyEngine::ReplaceSymbolReferences(const std::string& line) const {
        if (!symbolManager_) {
            return line;
        }

        std::string result = line;

        // Get all symbols
        auto symbols = symbolManager_->GetAllSymbols();

        // Sort by name length (descending) to replace longer names first
        std::sort(symbols.begin(), symbols.end(),
            [](const Symbol& a, const Symbol& b) {
                return a.name.length() > b.name.length();
            });

        // Replace each symbol reference with its address
        for (const auto& symbol : symbols) {
            // Skip if it's a label (will be handled by assembler)
            if (symbol.type == SymbolType::Label) {
                continue;
            }

            std::regex symbolRegex(R"(\b)" + symbol.name + R"(\b)");
            std::stringstream replacement;
            replacement << "0x" << std::hex << symbol.address;
            result = std::regex_replace(result, symbolRegex, replacement.str());
        }

        return result;
    }

    std::vector<std::pair<std::string, size_t>> AssemblyEngine::ExtractLabels(
        const std::string& assembly) const {

        std::vector<std::pair<std::string, size_t>> labels;
        std::istringstream stream(assembly);
        std::string line;
        size_t offset = 0;

        while (std::getline(stream, line)) {
            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t") + 1);

            // Check if line is a label (ends with :)
            if (!line.empty() && line.back() == ':') {
                std::string labelName = line.substr(0, line.length() - 1);
                labels.emplace_back(labelName, offset);
            }
            else if (!line.empty() && line[0] != ';') {
                // Estimate instruction size (rough approximation)
                // In real implementation, we'd assemble each instruction
                offset += 5; // Average x64 instruction size
            }
        }

        return labels;
    }

    std::optional<AssembledCode> AssemblyEngine::Assemble(const std::string& assembly,
        AddressType address) {
        // Preprocess the assembly
        std::string processed = PreprocessAssembly(assembly, address);

        // Extract labels before assembly
        auto labels = ExtractLabels(processed);

        // Assemble with Keystone
        unsigned char* machineCode = nullptr;
        size_t machineCodeSize = 0;
        size_t statementCount = 0;

        if (ks_asm(ksEngine_, processed.c_str(), address,
            &machineCode, &machineCodeSize, &statementCount) != KS_ERR_OK) {
            return std::nullopt;
        }

        // Create result
        AssembledCode result;
        result.machineCode.assign(machineCode, machineCode + machineCodeSize);
        result.codeSize = machineCodeSize;

        // Register labels with their addresses
        for (const auto& [labelName, offset] : labels) {
            AddressType labelAddress = address + offset;
            result.labels[labelName] = labelAddress;

            // Register in symbol manager if available
            if (symbolManager_) {
                symbolManager_->RegisterLabel(labelName, labelAddress);
            }
        }

        // Free Keystone allocated memory
        ks_free(machineCode);

        return result;
    }

    std::optional<ByteVector> AssemblyEngine::AssembleInstruction(
        const std::string& instruction, AddressType address) {

        std::string processed = PreprocessAssembly(instruction, address);

        unsigned char* machineCode = nullptr;
        size_t machineCodeSize = 0;
        size_t statementCount = 0;

        if (ks_asm(ksEngine_, processed.c_str(), address,
            &machineCode, &machineCodeSize, &statementCount) != KS_ERR_OK) {
            return std::nullopt;
        }

        ByteVector result(machineCode, machineCode + machineCodeSize);
        ks_free(machineCode);

        return result;
    }

    std::vector<AssemblyInstruction> AssemblyEngine::Disassemble(
        const ByteVector& machineCode, AddressType address) {

        // Note: This would require a disassembler like Capstone
        // For now, returning empty vector
        std::vector<AssemblyInstruction> instructions;

        // TODO: Implement with Capstone disassembler

        return instructions;
    }

    ByteVector AssemblyEngine::GenerateNop(size_t count) {
        ByteVector nops;
        nops.reserve(count);

        for (size_t i = 0; i < count; ++i) {
            nops.push_back(0x90); // NOP
        }

        return nops;
    }

    ByteVector AssemblyEngine::GenerateJump(AddressType from, AddressType to) {
        ByteVector jump;

        // Calculate relative offset
        int64_t offset = static_cast<int64_t>(to) - static_cast<int64_t>(from) - 5;

        // Check if we can use a short jump (32-bit offset)
        if (offset >= INT32_MIN && offset <= INT32_MAX) {
            // JMP rel32
            jump.push_back(0xE9);

            // Add offset (little endian)
            int32_t offset32 = static_cast<int32_t>(offset);
            jump.push_back(offset32 & 0xFF);
            jump.push_back((offset32 >> 8) & 0xFF);
            jump.push_back((offset32 >> 16) & 0xFF);
            jump.push_back((offset32 >> 24) & 0xFF);
        }
        else {
            // Need absolute jump (14 bytes)
            // MOV RAX, address
            jump.push_back(0x48);
            jump.push_back(0xB8);

            // Add address (little endian)
            for (int i = 0; i < 8; ++i) {
                jump.push_back((to >> (i * 8)) & 0xFF);
            }

            // JMP RAX
            jump.push_back(0xFF);
            jump.push_back(0xE0);
        }

        return jump;
    }

    ByteVector AssemblyEngine::GenerateCall(AddressType from, AddressType to) {
        ByteVector call;

        // Calculate relative offset
        int64_t offset = static_cast<int64_t>(to) - static_cast<int64_t>(from) - 5;

        // Check if we can use a short call (32-bit offset)
        if (offset >= INT32_MIN && offset <= INT32_MAX) {
            // CALL rel32
            call.push_back(0xE8);

            // Add offset (little endian)
            int32_t offset32 = static_cast<int32_t>(offset);
            call.push_back(offset32 & 0xFF);
            call.push_back((offset32 >> 8) & 0xFF);
            call.push_back((offset32 >> 16) & 0xFF);
            call.push_back((offset32 >> 24) & 0xFF);
        }
        else {
            // Need absolute call (14 bytes)
            // MOV RAX, address
            call.push_back(0x48);
            call.push_back(0xB8);

            // Add address (little endian)
            for (int i = 0; i < 8; ++i) {
                call.push_back((to >> (i * 8)) & 0xFF);
            }

            // CALL RAX
            call.push_back(0xFF);
            call.push_back(0xD0);
        }

        return call;
    }

    ByteVector AssemblyEngine::GenerateDetour(AddressType from, AddressType to,
        AddressType& trampolineSize) {
        // This is a simplified detour generator
        // In production, you'd need to:
        // 1. Disassemble instructions at 'from' to find safe hook point
        // 2. Copy original instructions to trampoline
        // 3. Add jump back to original code

        ByteVector detour;

        // For now, assume we need at least 5 bytes for jump
        trampolineSize = 5;

        // Generate jump to hook
        detour = GenerateJump(from, to);

        // Pad with NOPs if needed
        while (detour.size() < trampolineSize) {
            detour.push_back(0x90);
        }

        return detour;
    }

    // 在 AssemblyEngine.cpp 中修复 CreateHook 函数
    std::optional<AssemblyEngine::HookInfo> AssemblyEngine::CreateHook(AddressType targetAddress,
        const std::string& hookCode) {
        // This is a simplified hook creator
        // Full implementation would need memory allocation,
        // proper trampoline generation, etc.

        HookInfo hook;
        hook.targetAddress = targetAddress;

        // Assemble hook code
        auto assembled = Assemble(hookCode, 0);
        if (!assembled) {
            return std::nullopt;
        }

        hook.hookBytes = assembled->machineCode;

        // TODO: Implement full hook creation with trampoline

        return hook;
    }

    std::optional<std::vector<uint64_t>> AssemblyEngine::ExecuteAssembly(
        const std::string& assembly, const std::vector<uint64_t>& parameters) {

        // This would require:
        // 1. Allocating executable memory
        // 2. Writing assembled code
        // 3. Setting up parameters
        // 4. Executing code
        // 5. Retrieving results

        // Not implemented for safety reasons
        return std::nullopt;
    }

} // namespace AsmEngine