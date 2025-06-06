#include "AssemblyEngine.h"
#include "InstructionParser.h"
#include <regex>
#include <sstream>
#include <asmjit/x86.h>

namespace AsmEngine {

    using namespace asmjit;

    AssemblyEngine::AssemblyEngine(SymbolManager* symbolManager,
        CaptureStorage* captureStorage)
        : symbolManager_(symbolManager), captureStorage_(captureStorage) {

        // Initialize AsmJit runtime
        runtime_ = std::make_unique<JitRuntime>();
    }

    AssemblyEngine::~AssemblyEngine() {
        // AsmJit cleanup is automatic
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

        // Get all capture names
        auto captureNames = captureStorage_->GetAllNames();

        // Sort by length (descending) to replace longer names first
        std::sort(captureNames.begin(), captureNames.end(),
            [](const std::string& a, const std::string& b) {
                return a.length() > b.length();
            });

        // Replace each capture reference
        for (const auto& captureName : captureNames) {
            // Create regex that matches the capture name as a whole word
            // This handles cases like s1, s2 in assembly instructions
            std::regex captureRegex(R"(\b)" + captureName + R"(\b)");

            auto capture = captureStorage_->Get(captureName);
            if (!capture) continue;

            // Convert captured value to appropriate representation
            std::string replacement;

            switch (capture->size) {
            case 1:
                replacement = std::to_string(capture->AsUInt8());
                break;
            case 2:
                replacement = std::to_string(capture->AsUInt16());
                break;
            case 4:
                replacement = std::to_string(capture->AsUInt32());
                break;
            case 8:
                replacement = std::to_string(capture->AsUInt64());
                break;
            default:
                // For other sizes, use hex representation
                replacement = "0x" + BytesToString(capture->data);
                break;
            }

            result = std::regex_replace(result, captureRegex, replacement);
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
            // Skip if it's a label that might be used in jumps
            if (symbol.type == SymbolType::Label) {
                // Only replace in certain contexts (not in jump instructions)
                if (line.find("jmp") != std::string::npos ||
                    line.find("call") != std::string::npos ||
                    line.find("je") != std::string::npos ||
                    line.find("jne") != std::string::npos ||
                    line.find("jg") != std::string::npos ||
                    line.find("jl") != std::string::npos ||
                    line.find("ja") != std::string::npos ||
                    line.find("jb") != std::string::npos) {
                    continue;
                }
            }

            std::regex symbolRegex(R"(\b)" + symbol.name + R"(\b)");
            std::stringstream replacement;
            replacement << "0x" << std::hex << symbol.address;
            result = std::regex_replace(result, symbolRegex, replacement.str());
        }

        return result;
    }

    std::optional<ByteVector> AssemblyEngine::AssembleWithAsmJit(
        const std::string& instruction, AddressType address) {

        CodeHolder code;
        code.init(runtime_->environment());

        x86::Assembler assembler(&code);

        // Parse instruction using the new parser
        auto parsed = InstructionParser::Parse(instruction);

        try {
            // Use the extended assembler for instruction handling
            if (!ExtendedAssembler::AssembleInstruction(assembler, parsed)) {
                return std::nullopt;
            }

        }
        catch (...) {
            return std::nullopt;
        }

        // Get the assembled code
        CodeBuffer& buffer = code.sectionById(0)->buffer();
        if (buffer.size() == 0) {
            return std::nullopt;
        }

        ByteVector result(buffer.data(), buffer.data() + buffer.size());
        return result;
    }

    std::optional<AssembledCode> AssemblyEngine::Assemble(const std::string& assembly,
        AddressType address) {

        // Preprocess the assembly
        std::string processed = PreprocessAssembly(assembly, address);

        AssembledCode result;

        // Split into lines and assemble each
        std::istringstream stream(processed);
        std::string line;

        while (std::getline(stream, line)) {
            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t") + 1);

            // Skip empty lines and comments
            if (line.empty() || line[0] == ';') {
                continue;
            }

            // Check for labels
            if (line.back() == ':') {
                std::string labelName = line.substr(0, line.length() - 1);
                result.labels[labelName] = address + result.machineCode.size();

                if (symbolManager_) {
                    symbolManager_->RegisterLabel(labelName, address + result.machineCode.size());
                }
                continue;
            }

            // Assemble instruction
            auto instructionBytes = AssembleWithAsmJit(line, address + result.machineCode.size());
            if (instructionBytes) {
                result.machineCode.insert(result.machineCode.end(),
                    instructionBytes->begin(), instructionBytes->end());
            }
        }

        result.codeSize = result.machineCode.size();
        return result;
    }

    std::optional<ByteVector> AssemblyEngine::AssembleInstruction(
        const std::string& instruction, AddressType address) {

        std::string processed = PreprocessAssembly(instruction, address);
        return AssembleWithAsmJit(processed, address);
    }

    std::vector<AssemblyInstruction> AssemblyEngine::Disassemble(
        const ByteVector& machineCode, AddressType address) {

        // Note: For disassembly, you'd need to add a disassembler library like Zydis
        std::vector<AssemblyInstruction> instructions;

        // TODO: Implement with Zydis disassembler

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

        ByteVector detour;
        trampolineSize = 5;

        // Generate jump to hook
        detour = GenerateJump(from, to);

        // Pad with NOPs if needed
        while (detour.size() < trampolineSize) {
            detour.push_back(0x90);
        }

        return detour;
    }

    std::optional<AssemblyEngine::HookInfo> AssemblyEngine::CreateHook(
        AddressType targetAddress, const std::string& hookCode) {

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

        // Not implemented for safety reasons
        return std::nullopt;
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
                offset += 5; // Average x64 instruction size
            }
        }

        return labels;
    }

} // namespace AsmEngine