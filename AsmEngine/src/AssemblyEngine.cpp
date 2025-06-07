#include "AssemblyEngine.h"
#include "InstructionParser.h"
#include <regex>
#include <sstream>
#include <asmjit/x86.h>
#include <iostream>

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

        // 匹配 reg+number 或 reg-number 模式
        std::regex offsetRegex(R"(([a-zA-Z]+[0-9]*)([+-])([0-9A-Fa-f]+))");
        std::smatch match;

        if (std::regex_match(expr, match, offsetRegex)) {
            std::string reg = match[1].str();
            std::string sign = match[2].str();
            std::string offset = match[3].str();

            // 确保偏移量有0x前缀
            if (offset.substr(0, 2) != "0x" && offset.substr(0, 2) != "0X") {
                offset = "0x" + offset;
            }

            fixed = reg + sign + offset;
        }

        return fixed;
    }

    std::string AssemblyEngine::PreprocessAssembly(const std::string& assembly,
        AddressType baseAddress) const {
        std::string processed = assembly;

        // 替换捕获引用
        processed = ReplaceCaptureReferences(processed);

        // 替换符号引用
        processed = ReplaceSymbolReferences(processed);

        // 修复方括号内的语法
        // 将 [reg+hex] 转换为 [reg+0xhex]
        std::regex bracketRegex(R"(\[([^]]+)\])");
        std::smatch match;
        std::string temp = processed;
        std::string result;

        while (std::regex_search(temp, match, bracketRegex)) {
            result += temp.substr(0, match.position());

            std::string memExpr = match[1].str();
            // 处理内存表达式
            std::string fixedExpr = FixMemoryExpression(memExpr);
            result += "[" + fixedExpr + "]";

            temp = match.suffix();
        }
        result += temp;

        return result;
    }

    std::string AssemblyEngine::ReplaceCaptureReferences(const std::string& line) const {
        if (!captureStorage_) {
            return line;
        }

        std::string result = line;
        auto captureNames = captureStorage_->GetAllNames();

        // 调试输出
        if (!captureNames.empty() && (line.find("s1") != std::string::npos ||
            line.find("s2") != std::string::npos)) {
            std::cout << "[DEBUG] ReplaceCaptureReferences: input = '" << line << "'" << std::endl;
        }

        // 先处理内存引用中的十六进制数（如 [rax+30], [rdx-0C]）
        std::regex memOffsetRegex(R"(\[([^\]]+)\])");
        std::smatch memMatch;
        std::string temp = result;

        while (std::regex_search(temp, memMatch, memOffsetRegex)) {
            std::string memExpr = memMatch[1].str();
            std::string processedExpr = memExpr;

            // 处理加减偏移
            std::regex offsetRegex(R"(([+-])([0-9A-Fa-f]+)\b)");
            std::smatch offsetMatch;
            std::string tempExpr = processedExpr;
            std::string newExpr;
            size_t lastPos = 0;

            while (std::regex_search(tempExpr, offsetMatch, offsetRegex)) {
                // 添加匹配前的部分
                newExpr += tempExpr.substr(0, offsetMatch.position());

                std::string sign = offsetMatch[1].str();
                std::string num = offsetMatch[2].str();

                // 添加处理后的偏移（确保是0x格式）
                newExpr += sign + "0x" + num;

                // 移动到下一个搜索位置
                lastPos = offsetMatch.position() + offsetMatch.length();
                tempExpr = tempExpr.substr(lastPos);
            }

            // 添加剩余部分
            newExpr += tempExpr;
            processedExpr = newExpr;

            // 替换原始表达式
            size_t pos = result.find(memMatch[0].str());
            if (pos != std::string::npos) {
                result.replace(pos, memMatch[0].length(), "[" + processedExpr + "]");
            }

            temp = memMatch.suffix();
        }

        // Sort by length (descending) to replace longer names first
        std::sort(captureNames.begin(), captureNames.end(),
            [](const std::string& a, const std::string& b) {
                return a.length() > b.length();
            });

        // Replace each capture reference
        for (const auto& captureName : captureNames) {
            // Create regex that matches the capture name as a whole word
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

        // 计算相对偏移
        int64_t offset = static_cast<int64_t>(to) - static_cast<int64_t>(from) - 5;

        std::cout << "[DEBUG] Generating jump from 0x" << std::hex << from
            << " to 0x" << to << std::dec << std::endl;
        std::cout << "[DEBUG] Jump offset: 0x" << std::hex << offset << std::dec << std::endl;

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
            std::cout << "[WARNING] Offset too large for relative jump, need trampoline" << std::endl;
            // 这里应该使用跳板机制
            return ByteVector(); // 返回空，让调用者处理
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