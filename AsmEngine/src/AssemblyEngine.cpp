#include "AssemblyEngine.h"
#include "InstructionParser.h"
#include <regex>
#include <sstream>
#include <asmjit/x86.h>
#include <iostream>
#include <set>

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

    std::string AssemblyEngine::FixMemoryExpression(const std::string& expr) const {
        std::string fixed = expr;

        // Pattern to match register+offset or register-offset
        // This handles cases like rax+30, rdx-0C, etc.
        std::regex offsetRegex(R"(([a-zA-Z]+[0-9]*)([+-])([0-9A-Fa-f]+)\b)");
        std::smatch match;

        std::string result;
        std::string temp = fixed;

        while (std::regex_search(temp, match, offsetRegex)) {
            result += temp.substr(0, match.position());

            std::string reg = match[1].str();
            std::string sign = match[2].str();
            std::string offset = match[3].str();

            // Check if offset already has 0x prefix
            if (offset.substr(0, 2) != "0x" && offset.substr(0, 2) != "0X") {
                // Check if it looks like a hex number
                bool hasHexDigit = false;
                for (char c : offset) {
                    if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
                        hasHexDigit = true;
                        break;
                    }
                }

                // Add 0x prefix for hex numbers
                if (hasHexDigit || offset.length() > 2) {
                    offset = "0x" + offset;
                }
            }

            result += reg + sign + offset;
            temp = match.suffix();
        }
        result += temp;

        return result;
    }

    std::string AssemblyEngine::PreprocessAssembly(const std::string& assembly,
        AddressType baseAddress) const {

        std::string processed = assembly;

        // Step 1: Replace capture references (s1, s2, etc.)
        processed = ReplaceCaptureReferences(processed);

        // Step 2: Process memory expressions and resolve symbols
        // This handles patterns like [player], [rax+s1], [rbx+symbol], etc.
        std::regex memExprRegex(R"(\[([^\]]+)\])");
        std::smatch match;
        std::string result;

        auto tempStr = processed;
        while (std::regex_search(tempStr, match, memExprRegex)) {
            // Add everything before the match
            result += tempStr.substr(0, match.position());

            std::string memExpr = match[1].str();
            std::string processedExpr;

            std::cout << "[DEBUG] Processing memory expression: [" << memExpr << "]" << std::endl;

            // Parse the memory expression
            std::string currentToken;

            for (size_t i = 0; i < memExpr.length(); ++i) {
                char c = memExpr[i];

                if (c == '+' || c == '-' || c == '*') {
                    // Handle accumulated token
                    if (!currentToken.empty()) {
                        std::string processed = ProcessMemoryToken(currentToken);
                        std::cout << "[DEBUG]   Token '" << currentToken << "' -> '" << processed << "'" << std::endl;
                        processedExpr += processed;
                        currentToken.clear();
                    }
                    processedExpr += c;
                }
                else if (c == ' ' || c == '\t') {
                    // Handle accumulated token
                    if (!currentToken.empty()) {
                        std::string processed = ProcessMemoryToken(currentToken);
                        std::cout << "[DEBUG]   Token '" << currentToken << "' -> '" << processed << "'" << std::endl;
                        processedExpr += processed;
                        currentToken.clear();
                    }
                    if (!processedExpr.empty() && processedExpr.back() != ' ') {
                        processedExpr += ' ';
                    }
                }
                else {
                    currentToken += c;
                }
            }

            // Handle final token
            if (!currentToken.empty()) {
                std::string processed = ProcessMemoryToken(currentToken);
                std::cout << "[DEBUG]   Token '" << currentToken << "' -> '" << processed << "'" << std::endl;
                processedExpr += processed;
            }

            std::cout << "[DEBUG] Memory expression result: [" << processedExpr << "]" << std::endl;

            result += "[" + processedExpr + "]";
            tempStr = match.suffix();
        }
        result += tempStr;

        // Step 3: Process immediate values and symbol references outside of memory expressions
        processed = result;
        result.clear();

        // Split by whitespace and commas to process each token
        std::istringstream iss(processed);
        std::string token;
        bool firstToken = true;

        while (iss >> token) {
            if (!firstToken) {
                result += " ";
            }
            firstToken = false;

            // Skip if token contains brackets (already processed)
            if (token.find('[') != std::string::npos || token.find(']') != std::string::npos) {
                result += token;
            }
            // Skip register names
            else if (IsRegisterName(token)) {
                result += token;
            }
            // Skip instruction mnemonics (first token on line typically)
            else if (result.empty() || result.back() == '\n') {
                result += token;
            }
            // Try to resolve as symbol
            else if (symbolManager_) {
                // Remove comma if it's at the end
                std::string cleanToken = token;
                bool hasComma = false;
                if (!cleanToken.empty() && cleanToken.back() == ',') {
                    cleanToken.pop_back();
                    hasComma = true;
                }

                auto addr = symbolManager_->ResolveAddress(cleanToken);
                if (addr) {
                    std::stringstream ss;
                    ss << "0x" << std::hex << *addr;
                    result += ss.str();
                    if (hasComma) result += ",";

                    std::cout << "[DEBUG] Resolved symbol '" << cleanToken << "' to " << ss.str() << std::endl;
                }
                else {
                    result += token;
                }
            }
            else {
                result += token;
            }
        }

        std::cout << "[DEBUG] PreprocessAssembly result: '" << result << "'" << std::endl;

        return result;
    }

    // Helper method to process tokens inside memory expressions
    std::string AssemblyEngine::ProcessMemoryToken(const std::string& token) const {
        // Check if it's a register
        if (IsRegisterName(token)) {
            return token;
        }

        // Check if it's already a number (hex or decimal)
        if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
            return token;  // Already hex
        }

        // Try to resolve as symbol FIRST before checking if it's a number
        if (symbolManager_) {
            auto addr = symbolManager_->ResolveAddress(token);
            if (addr) {
                std::stringstream ss;
                ss << "0x" << std::hex << *addr;
                std::cout << "[DEBUG]     Symbol '" << token << "' resolved to " << ss.str() << std::endl;
                return ss.str();
            }
        }

        // Check if it's a pure number
        bool isNumber = true;
        bool hasHexDigit = false;
        for (char c : token) {
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
                isNumber = false;
                break;
            }
            if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
                hasHexDigit = true;
            }
        }

        if (isNumber && !token.empty()) {
            // If it has hex digits or is longer than 2 chars, treat as hex
            if (hasHexDigit || token.length() > 2) {
                return "0x" + token;  // Add 0x prefix
            }
            else {
                // For small numbers like "30", check context
                // In x64, offsets are often hex even without letters
                try {
                    int val = std::stoi(token, nullptr, 10);
                    if (val > 9) {
                        // Likely hex
                        return "0x" + token;
                    }
                }
                catch (...) {
                    // Not a valid decimal, treat as hex
                    return "0x" + token;
                }
                return token;  // Keep as is
            }
        }

        // Return as is if can't resolve
        std::cout << "[WARNING]     Could not resolve token '" << token << "'" << std::endl;
        return token;
    }

    // Helper method to check if a token is a register name
    bool AssemblyEngine::IsRegisterName(const std::string& token) const {
        static const std::set<std::string> registers = {
            // 64-bit registers
            "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
            // 32-bit registers
            "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
            "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
            // 16-bit registers
            "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
            "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w",
            // 8-bit registers
            "al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl",
            "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
            "ah", "bh", "ch", "dh",
            // Segment registers
            "cs", "ds", "es", "fs", "gs", "ss",
            // Special registers
            "rip", "eip", "ip"
        };

        return registers.find(token) != registers.end();
    }

    // Also add these method declarations to AssemblyEngine.h under private:
    // std::string ProcessMemoryToken(const std::string& token) const;
    // bool IsRegisterName(const std::string& token) const;

    std::string AssemblyEngine::ReplaceCaptureReferences(const std::string& line) const {
        if (!captureStorage_) {
            return line;
        }

        std::string result = line;
        auto captureNames = captureStorage_->GetAllNames();

        // Debug output
        if (!captureNames.empty()) {
            std::cout << "[DEBUG] ReplaceCaptureReferences: input = '" << line << "'" << std::endl;
            std::cout << "[DEBUG] Available captures: ";
            for (const auto& name : captureNames) {
                std::cout << name << " ";
            }
            std::cout << std::endl;
        }

        // Sort by length (descending) to replace longer names first
        std::sort(captureNames.begin(), captureNames.end(),
            [](const std::string& a, const std::string& b) {
                return a.length() > b.length();
            });

        // Replace each capture reference
        for (const auto& captureName : captureNames) {
            // Create regex that matches the capture name as a whole word
            // This handles cases like s1, s2 in expressions like [rax+s1]
            std::regex captureRegex(R"(\b)" + captureName + R"(\b)");

            auto capture = captureStorage_->Get(captureName);
            if (!capture) continue;

            // Convert captured value to appropriate representation
            std::string replacement;

            switch (capture->size) {
            case 1:
            {
                std::stringstream ss;
                ss << "0x" << std::hex << static_cast<unsigned>(capture->AsUInt8());
                replacement = ss.str();
            }
            break;
            case 2:
            {
                std::stringstream ss;
                ss << "0x" << std::hex << capture->AsUInt16();
                replacement = ss.str();
            }
            break;
            case 4:
            {
                std::stringstream ss;
                ss << "0x" << std::hex << capture->AsUInt32();
                replacement = ss.str();
            }
            break;
            case 8:
            {
                std::stringstream ss;
                ss << "0x" << std::hex << capture->AsUInt64();
                replacement = ss.str();
            }
            break;
            default:
                // For other sizes, use hex representation
                replacement = "0x" + BytesToString(capture->data);
                break;
            }

            // Replace all occurrences
            try {
                std::string before = result;
                result = std::regex_replace(result, captureRegex, replacement);
                if (before != result) {
                    std::cout << "[DEBUG] Replaced '" << captureName << "' with " << replacement << std::endl;
                }
            }
            catch (const std::regex_error& e) {
                std::cout << "[WARNING] Failed to replace capture '" << captureName
                    << "': " << e.what() << std::endl;
            }
        }

        if (!captureNames.empty() && result != line) {
            std::cout << "[DEBUG] ReplaceCaptureReferences: output = '" << result << "'" << std::endl;
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

            // Create a more specific regex that won't match inside brackets if the symbol isn't resolved
            // This prevents regex errors when we have [unresolved_symbol]
            std::regex symbolRegex(R"(\b)" + symbol.name + R"(\b(?![^\[]*\]))");
            std::stringstream replacement;
            replacement << "0x" << std::hex << symbol.address;

            try {
                result = std::regex_replace(result, symbolRegex, replacement.str());
            }
            catch (const std::regex_error& e) {
                // If regex fails, skip this symbol
                std::cout << "[WARNING] Failed to replace symbol '" << symbol.name
                    << "': " << e.what() << std::endl;
            }
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

        // 计算相对偏移
        int64_t offset = static_cast<int64_t>(to) - static_cast<int64_t>(from) - 5;

        std::cout << "[DEBUG] Generating jump from 0x" << std::hex << from
            << " to 0x" << to << std::dec << std::endl;
        std::cout << "[DEBUG] Jump offset: 0x" << std::hex << offset << std::dec
            << " (" << offset << " decimal)" << std::endl;

        // 检查是否可以使用短跳转（32位偏移）
        if (offset >= INT32_MIN && offset <= INT32_MAX) {
            // E9 rel32
            jump.push_back(0xE9);

            // 添加偏移（小端序）
            int32_t offset32 = static_cast<int32_t>(offset);
            jump.push_back(offset32 & 0xFF);
            jump.push_back((offset32 >> 8) & 0xFF);
            jump.push_back((offset32 >> 16) & 0xFF);
            jump.push_back((offset32 >> 24) & 0xFF);

            std::cout << "[DEBUG] Generated E9 relative jump: ";
            for (uint8_t b : jump) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)b << " ";
            }
            std::cout << std::dec << std::endl;
        }
        else {
            std::cout << "[WARNING] Offset 0x" << std::hex << offset
                << " too large for relative jump, using absolute jump" << std::dec << std::endl;

            // 对于超出范围的跳转，使用 14 字节的绝对跳转
            // jmp [rip+0]
            jump.push_back(0xFF);
            jump.push_back(0x25);
            jump.push_back(0x00);
            jump.push_back(0x00);
            jump.push_back(0x00);
            jump.push_back(0x00);

            // 后面跟着8字节的绝对地址
            for (int i = 0; i < 8; ++i) {
                jump.push_back((to >> (i * 8)) & 0xFF);
            }

            std::cout << "[DEBUG] Generated FF 25 absolute jump" << std::endl;
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

    std::optional<AssemblyEngine::CodeCave> AssemblyEngine::FindCodeCave(
        AddressType nearAddress, size_t minSize) const {

        // This is a basic implementation that looks for a sequence of NOPs or CC (int3)
        // In a real implementation, you'd want more sophisticated cave detection

        if (!symbolManager_) {
            return std::nullopt;
        }

        // Search in a 2GB range around the target address
        const size_t searchRange = 0x7FFFFFFF; // 2GB - 1
        const size_t pageSize = 0x1000; // 4KB

        // TODO: Implement actual code cave searching logic
        // For now, return nullopt
        return std::nullopt;
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