#include "ScriptParser.h"
#include "AsmEngine.h"
#include <fstream>
#include <regex>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace AsmEngine {

    // ScriptContext implementation
    void ScriptContext::SetVariable(const std::string& name, const std::string& value) {
        variables_[name] = value;
    }

    std::optional<std::string> ScriptContext::GetVariable(const std::string& name) const {
        auto it = variables_.find(name);
        if (it != variables_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void ScriptContext::AddAllocation(AddressType address) {
        allocations_.push_back(address);
    }

    void ScriptContext::AddPatch(AddressType address, const ByteVector& originalBytes) {
        patches_.emplace_back(address, originalBytes);
    }

    void ScriptContext::Cleanup() {
        // Cleanup is handled by the engine
        allocations_.clear();
        patches_.clear();
        variables_.clear();
    }

    // ScriptParser implementation
    ScriptParser::ScriptParser(AsmEngine* engine)
        : engine_(engine), currentSection_(nullptr), anonymousLabelCounter_(0) {
    }

    void ScriptParser::Parse(const std::string& script) {
        Clear();

        std::istringstream stream(script);
        std::string line;
        size_t lineNumber = 0;

        while (std::getline(stream, line)) {
            lineNumber++;

            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);

            // Skip empty lines and comments
            if (line.empty() || line[0] == ';' ||
                (line.size() >= 2 && line[0] == '/' && line[1] == '/')) {
                continue;
            }

            // Check for section markers
            if (line == "[ENABLE]") {
                BeginSection("ENABLE");
                continue;
            }
            else if (line == "[DISABLE]") {
                BeginSection("DISABLE");
                continue;
            }

            try {
                // Preprocess line for CE syntax
                line = PreprocessLine(line);

                // Check if it's a label (including label+offset)
                if (IsLabel(line)) {
                    auto [labelName, offset] = ParseLabelWithOffset(line);

                    ParsedCommand cmd;
                    cmd.type = CommandType::Label;
                    cmd.name = "label";
                    cmd.arguments.push_back(labelName);
                    if (offset > 0) {
                        cmd.arguments.push_back(std::to_string(offset));
                    }
                    cmd.lineNumber = lineNumber;

                    if (currentSection_) {
                        currentSection_->commands.push_back(cmd);
                    }
                    continue;
                }

                // Process anonymous labels
                line = ProcessAnonymousLabels(line);

                // Parse and add command
                ParsedCommand cmd = ParseLine(line, lineNumber);

                if (currentSection_) {
                    currentSection_->commands.push_back(cmd);
                }
                else {
                    // Create default section if none exists
                    BeginSection("DEFAULT");
                    currentSection_->commands.push_back(cmd);
                }
            }
            catch (const std::exception& e) {
                ReportError(e.what(), lineNumber);
            }
        }

        EndSection();
    }

    void ScriptParser::ParseFile(const std::string& filename) {
        std::ifstream file(filename);
        if (!file) {
            throw EngineException(ErrorCode::InvalidParameter,
                "Failed to open script file: " + filename);
        }

        std::string script((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());

        Parse(script);
    }

    std::string ScriptParser::PreprocessLine(const std::string& line) const {
        std::string result = line;

        // Convert (float)X to hex representation
        std::regex floatRegex(R"(\(float\)(\d+(?:\.\d+)?))");
        std::smatch match;

        while (std::regex_search(result, match, floatRegex)) {
            float value = std::stof(match[1].str());
            uint32_t hexValue = *reinterpret_cast<uint32_t*>(&value);

            std::stringstream ss;
            ss << "0x" << std::hex << hexValue;

            result = result.substr(0, match.position()) + ss.str() +
                result.substr(match.position() + match.length());
        }

        return result;
    }

    bool ScriptParser::IsLabel(const std::string& line) const {
        // Check if line ends with colon
        if (line.empty() || line.back() != ':') {
            return false;
        }

        // Make sure it's not a drive letter (C:)
        if (line.length() == 2 && std::isalpha(line[0])) {
            return false;
        }

        return true;
    }

    std::pair<std::string, size_t> ScriptParser::ParseLabelWithOffset(const std::string& line) const {
        // Remove the trailing colon
        std::string labelPart = line.substr(0, line.length() - 1);

        // Check for offset syntax (label+offset)
        size_t plusPos = labelPart.find('+');
        if (plusPos != std::string::npos) {
            std::string baseName = labelPart.substr(0, plusPos);
            std::string offsetStr = labelPart.substr(plusPos + 1);

            // Parse offset - default to hex unless # prefix for decimal
            size_t offset = 0;
            try {
                if (!offsetStr.empty() && offsetStr[0] == '#') {
                    // Decimal with # prefix
                    offset = std::stoull(offsetStr.substr(1), nullptr, 10);
                }
                else if (offsetStr.size() > 2 && offsetStr[0] == '0' &&
                    (offsetStr[1] == 'x' || offsetStr[1] == 'X')) {
                    // Hex with 0x prefix
                    offset = std::stoull(offsetStr, nullptr, 16);
                }
                else {
                    // Default to hex
                    offset = std::stoull(offsetStr, nullptr, 16);
                }
            }
            catch (...) {
                offset = 0;
            }

            return { baseName, offset };
        }

        return { labelPart, 0 };
    }

    std::string ScriptParser::ProcessAnonymousLabels(const std::string& line) {
        std::string result = line;

        // Check if this is an anonymous label definition
        if (line == "@@:") {
            anonymousLabelCounter_++;
            return "__anon_label_" + std::to_string(anonymousLabelCounter_) + ":";
        }

        // Replace @f with forward reference to next anonymous label
        size_t pos = 0;
        while ((pos = result.find("@f", pos)) != std::string::npos) {
            // Make sure it's not part of a larger word
            if ((pos == 0 || !std::isalnum(result[pos - 1])) &&
                (pos + 2 >= result.length() || !std::isalnum(result[pos + 2]))) {
                result.replace(pos, 2, "__anon_label_" + std::to_string(anonymousLabelCounter_ + 1));
            }
            else {
                pos += 2;
            }
        }

        // Replace @b with backward reference to previous anonymous label
        pos = 0;
        while ((pos = result.find("@b", pos)) != std::string::npos) {
            // Make sure it's not part of a larger word
            if ((pos == 0 || !std::isalnum(result[pos - 1])) &&
                (pos + 2 >= result.length() || !std::isalnum(result[pos + 2]))) {
                result.replace(pos, 2, "__anon_label_" + std::to_string(anonymousLabelCounter_));
            }
            else {
                pos += 2;
            }
        }

        return result;
    }

    ParsedCommand ScriptParser::ParseLine(const std::string& line, size_t lineNumber) const {
        ParsedCommand cmd;
        cmd.lineNumber = lineNumber;

        // Expand defines
        std::string expandedLine = ExpandDefines(line);

        // Tokenize
        auto tokens = TokenizeLine(expandedLine);
        if (tokens.empty()) {
            throw EngineException(ErrorCode::InvalidParameter, "Empty command");
        }

        // 特殊处理 "nop X" 语法
        if (tokens.size() == 2 && tokens[0] == "nop") {
            // 转换 "nop 3" 为多个nop指令
            try {
                int count = std::stoi(tokens[1]);
                cmd.type = CommandType::Asm;
                cmd.name = "nop_multiple";
                cmd.arguments.push_back(std::to_string(count));
                return cmd;
            }
            catch (...) {
                // 如果不是数字，按普通汇编处理
            }
        }

        // First token is the command
        cmd.type = IdentifyCommand(tokens[0]);
        cmd.name = tokens[0];

        // Rest are arguments
        cmd.arguments.assign(tokens.begin() + 1, tokens.end());

        return cmd;
    }

    CommandType ScriptParser::IdentifyCommand(const std::string& command) const {
        std::string lowerCmd = command;
        std::transform(lowerCmd.begin(), lowerCmd.end(), lowerCmd.begin(), ::tolower);

        static const std::unordered_map<std::string, CommandType> commandMap = {
            {"aobscan", CommandType::Aobscan},
            {"aobscanmodule", CommandType::Aobscanmodule},
            {"alloc", CommandType::Alloc},
            {"dealloc", CommandType::Dealloc},
            {"label", CommandType::Label},
            {"registersymbol", CommandType::RegisterSymbol},
            {"unregistersymbol", CommandType::UnregisterSymbol},
            {"define", CommandType::Define},
            {"include", CommandType::Include},
            {"db", CommandType::Db},
            {"dw", CommandType::Dw},
            {"dd", CommandType::Dd},
            {"dq", CommandType::Dq}
        };

        auto it = commandMap.find(lowerCmd);
        if (it != commandMap.end()) {
            return it->second;
        }

        // If not a command, assume it's assembly code
        return CommandType::Asm;
    }

    std::vector<std::string> ScriptParser::TokenizeLine(const std::string& line) const {
        std::vector<std::string> tokens;
        std::string processedLine = line;

        // Handle parentheses syntax for commands
        std::regex cmdWithParens(R"(^(\w+)\s*\((.*)\)\s*$)");
        std::smatch match;

        if (std::regex_match(processedLine, match, cmdWithParens)) {
            // Command with parentheses
            tokens.push_back(match[1].str()); // Command name

            // Parse arguments inside parentheses
            std::string args = match[2].str();
            std::string current;
            bool inQuotes = false;
            int parenDepth = 0;

            for (char c : args) {
                if (c == '"' && (current.empty() || current.back() != '\\')) {
                    inQuotes = !inQuotes;
                    current += c;
                }
                else if (!inQuotes) {
                    if (c == '(') {
                        parenDepth++;
                        current += c;
                    }
                    else if (c == ')') {
                        parenDepth--;
                        current += c;
                    }
                    else if (c == ',' && parenDepth == 0) {
                        // Trim whitespace from current token
                        current.erase(0, current.find_first_not_of(" \t"));
                        current.erase(current.find_last_not_of(" \t") + 1);
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

            // Add last token
            current.erase(0, current.find_first_not_of(" \t"));
            current.erase(current.find_last_not_of(" \t") + 1);
            if (!current.empty()) {
                tokens.push_back(current);
            }

            return tokens;
        }

        // Handle registersymbol/unregistersymbol with multiple arguments
        if (processedLine.find("registersymbol") == 0 || processedLine.find("unregistersymbol") == 0) {
            std::istringstream iss(processedLine);
            std::string token;

            while (iss >> token) {
                // Remove parentheses if present
                if (!token.empty() && token.front() == '(') {
                    token = token.substr(1);
                }
                if (!token.empty() && token.back() == ')') {
                    token = token.substr(0, token.length() - 1);
                }
                tokens.push_back(token);
            }

            return tokens;
        }

        // Standard tokenization
        std::string current;
        bool inQuotes = false;
        bool inBrackets = false;

        for (size_t i = 0; i < processedLine.length(); ++i) {
            char c = processedLine[i];

            if (c == '"' && (i == 0 || processedLine[i - 1] != '\\')) {
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

    std::string ScriptParser::ExpandDefines(const std::string& line) const {
        std::string result = line;

        for (const auto& [name, value] : defines_) {
            std::regex defineRegex(R"(\b)" + name + R"(\b)");
            result = std::regex_replace(result, defineRegex, value);
        }

        return result;
    }

    std::vector<uint8_t> ScriptParser::ParseDataBytes(const std::vector<std::string>& args) const {
        std::vector<uint8_t> data;

        for (const auto& arg : args) {
            // Check if it's a captured value
            if (engine_->Captures()->Exists(arg)) {
                auto capture = engine_->Captures()->Get(arg);
                if (capture && capture->size == 1) {
                    data.push_back(capture->AsUInt8());
                }
                else if (capture) {
                    // For multi-byte captures in db, push each byte
                    for (uint8_t byte : capture->data) {
                        data.push_back(byte);
                    }
                }
            }
            else {
                // Parse as hex or decimal byte
                try {
                    uint32_t value = 0;
                    if (arg.size() > 2 && arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
                        value = std::stoul(arg, nullptr, 16);
                    }
                    else {
                        value = std::stoul(arg, nullptr, 16); // Default to hex for db
                    }
                    data.push_back(value & 0xFF);
                }
                catch (...) {
                    // Skip invalid values
                }
            }
        }

        return data;
    }

    uint64_t ScriptParser::ParseNumber(const std::string& str) const {
        if (str.empty()) {
            return 0;
        }

        std::string numStr = str;
        bool negative = false;

        // Handle negative numbers
        if (str[0] == '-') {
            negative = true;
            numStr = str.substr(1);
        }

        uint64_t value = 0;

        try {
            if (!numStr.empty() && numStr[0] == '#') {
                // Decimal with # prefix
                value = std::stoull(numStr.substr(1), nullptr, 10);
            }
            else if (numStr.size() > 1 && numStr[0] == '$') {
                // Hex with $ prefix (CE style)
                value = std::stoull(numStr.substr(1), nullptr, 16);
            }
            else if (numStr.size() > 2 && numStr[0] == '0' &&
                (numStr[1] == 'x' || numStr[1] == 'X')) {
                // Hex with 0x prefix
                value = std::stoull(numStr, nullptr, 16);
            }
            else {
                // Default to hex (including cases like 0C, 30, etc.)
                value = std::stoull(numStr, nullptr, 16);
            }
        }
        catch (...) {
            value = 0;
        }

        return negative ? static_cast<uint64_t>(-static_cast<int64_t>(value)) : value;
    }

    void ScriptParser::Execute() {
        // Execute ENABLE section by default
        for (auto& section : sections_) {
            if (section.name == "ENABLE") {
                ExecuteSection(section.name);
                break;
            }
        }
    }

    void ScriptParser::ExecuteSection(const std::string& sectionName) {
        auto sectionIt = std::find_if(sections_.begin(), sections_.end(),
            [&](const ScriptSection& s) {
                return s.name == sectionName;
            });

        if (sectionIt == sections_.end()) {
            throw EngineException(ErrorCode::InvalidParameter,
                "Section not found: " + sectionName);
        }

        currentSection_ = &(*sectionIt);

        // First pass: process all allocations and symbols
        for (const auto& cmd : sectionIt->commands) {
            try {
                switch (cmd.type) {
                case CommandType::Aobscan:
                    HandleAobscan(cmd);
                    break;

                case CommandType::Aobscanmodule:
                    HandleAobscanmodule(cmd);
                    break;

                case CommandType::Alloc:
                    HandleAlloc(cmd);
                    break;

                case CommandType::RegisterSymbol:
                    HandleRegisterSymbol(cmd);
                    break;

                case CommandType::Define:
                    HandleDefine(cmd);
                    break;

                default:
                    break;
                }
            }
            catch (const std::exception& e) {
                ReportError(e.what(), cmd.lineNumber);
            }
        }

        // Second pass: handle labels and assembly
        std::map<std::string, std::vector<uint8_t>> codeBuffers;
        std::string currentLabel;
        AddressType currentWriteAddress = 0;
        bool hasValidLabel = false;

        for (const auto& cmd : sectionIt->commands) {
            try {
                switch (cmd.type) {
                case CommandType::Label:
                {
                    // 如果上一个标签没有任何代码，清理它
                    if (!currentLabel.empty() &&
                        currentSection_->codeChunks.count(currentLabel) > 0 &&
                        currentSection_->codeChunks[currentLabel].empty()) {
                        std::cout << "[DEBUG] Removing empty code chunk for label: " << currentLabel << std::endl;
                        currentSection_->codeChunks.erase(currentLabel);
                    }

                    std::string baseLabelName = cmd.arguments[0];
                    size_t offset = 0;

                    if (cmd.arguments.size() > 1) {
                        offset = std::stoull(cmd.arguments[1]);
                    }

                    // 调试输出
                    std::cout << "[DEBUG] Processing label: " << baseLabelName;
                    if (offset > 0) {
                        std::cout << " + 0x" << std::hex << offset;
                    }
                    std::cout << std::dec << std::endl;

                    // 解析基址 - 按优先级查找
                    AddressType baseAddr = 0;

                    // 1. 首先从符号管理器查找
                    auto resolvedAddr = engine_->Symbols()->ResolveAddress(baseLabelName);
                    if (resolvedAddr) {
                        baseAddr = *resolvedAddr;
                        std::cout << "[DEBUG] Resolved from symbols: 0x" << std::hex << baseAddr << std::dec << std::endl;
                    }
                    // 2. 然后从当前section的labels查找
                    else if (currentSection_->labels.count(baseLabelName)) {
                        baseAddr = currentSection_->labels[baseLabelName];
                        std::cout << "[DEBUG] Found in section labels: 0x" << std::hex << baseAddr << std::dec << std::endl;
                    }
                    // 3. 最后从allocations查找（只对alloc的内存有效）
                    else if (currentSection_->allocations.count(baseLabelName)) {
                        baseAddr = currentSection_->allocations[baseLabelName];
                        std::cout << "[DEBUG] Found in allocations: 0x" << std::hex << baseAddr << std::dec << std::endl;
                    }

                    if (baseAddr > 0) {
                        currentWriteAddress = baseAddr + offset;

                        // 创建当前标签名（带偏移的使用唯一名称）
                        if (offset > 0) {
                            currentLabel = baseLabelName + "_offset_" + std::to_string(offset);
                        }
                        else {
                            currentLabel = baseLabelName;
                        }

                        // 注册标签
                        currentSection_->labels[currentLabel] = currentWriteAddress;
                        engine_->Symbols()->RegisterLabel(currentLabel, currentWriteAddress);

                        hasValidLabel = true;

                        std::cout << "[DEBUG] Current label: " << currentLabel
                            << " at 0x" << std::hex << currentWriteAddress << std::dec << std::endl;
                    }
                    else {
                        std::cout << "[ERROR] Could not resolve base address for label: "
                            << baseLabelName << std::endl;
                        hasValidLabel = false;
                        currentLabel.clear();
                    }
                }
                break;

                case CommandType::Asm:
                    if (hasValidLabel && currentWriteAddress > 0) {
                        // 处理多个nop
                        if (cmd.name == "nop_multiple" && !cmd.arguments.empty()) {
                            int count = std::stoi(cmd.arguments[0]);
                            std::vector<uint8_t> nops(count, 0x90);

                            std::cout << "[DEBUG] Adding " << count << " NOPs to " << currentLabel << std::endl;

                            currentSection_->codeChunks[currentLabel].insert(
                                currentSection_->codeChunks[currentLabel].end(),
                                nops.begin(), nops.end()
                            );
                            currentWriteAddress += count;
                        }
                        else {
                            // 正常的汇编指令
                            std::string asmLine = cmd.name;
                            for (const auto& arg : cmd.arguments) {
                                asmLine += " " + arg;
                            }

                            std::cout << "[DEBUG] Assembling for " << currentLabel
                                << " at 0x" << std::hex << currentWriteAddress
                                << ": " << asmLine << std::dec << std::endl;

                            auto result = engine_->Assembly()->AssembleInstruction(asmLine, currentWriteAddress);
                            if (result && !result->empty()) {
                                std::cout << "[DEBUG] Assembled " << result->size() << " bytes: ";
                                for (size_t i = 0; i < std::min<size_t>(result->size(), 16); ++i) {
                                    std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)(*result)[i] << " ";
                                }
                                if (result->size() > 16) {
                                    std::cout << "...";
                                }
                                std::cout << std::dec << std::endl;

                                currentSection_->codeChunks[currentLabel].insert(
                                    currentSection_->codeChunks[currentLabel].end(),
                                    result->begin(), result->end()
                                );

                                // 更新写入地址以供下一条指令使用
                                currentWriteAddress += result->size();
                            }
                            else {
                                std::cout << "[ERROR] Failed to assemble: " << asmLine << std::endl;
                            }
                        }
                    }
                    else {
                        std::cout << "[WARNING] No valid label for assembly: " << cmd.name
                            << " (hasValidLabel=" << hasValidLabel
                            << ", currentWriteAddress=0x" << std::hex << currentWriteAddress << std::dec << ")" << std::endl;
                    }
                    break;

                case CommandType::Db:
                case CommandType::Dd:
                case CommandType::Dq:
                    if (hasValidLabel && currentWriteAddress > 0) {
                        HandleDataDefinition(cmd);

                        // Update write address based on data size
                        size_t dataSize = 0;
                        if (cmd.type == CommandType::Db) {
                            // 对于 db，需要考虑捕获值可能是多字节的
                            for (const auto& arg : cmd.arguments) {
                                if (engine_->Captures()->Exists(arg)) {
                                    auto capture = engine_->Captures()->Get(arg);
                                    if (capture) {
                                        dataSize += capture->data.size();
                                    }
                                }
                                else {
                                    dataSize += 1;  // 普通字节
                                }
                            }
                        }
                        else if (cmd.type == CommandType::Dd) {
                            dataSize = cmd.arguments.size() * 4;
                        }
                        else if (cmd.type == CommandType::Dq) {
                            dataSize = cmd.arguments.size() * 8;
                        }

                        currentWriteAddress += dataSize;
                        std::cout << "[DEBUG] Data definition added " << dataSize << " bytes, new address: 0x"
                            << std::hex << currentWriteAddress << std::dec << std::endl;
                    }
                    break;

                default:
                    break;
                }
            }
            catch (const std::exception& e) {
                ReportError(e.what(), cmd.lineNumber);
            }
        }

        // 清理最后一个标签（如果为空）
        if (!currentLabel.empty() &&
            currentSection_->codeChunks.count(currentLabel) > 0 &&
            currentSection_->codeChunks[currentLabel].empty()) {
            std::cout << "[DEBUG] Removing empty code chunk for last label: " << currentLabel << std::endl;
            currentSection_->codeChunks.erase(currentLabel);
        }

        // 清理所有空的 code chunks
        std::cout << "[DEBUG] Cleaning up empty code chunks..." << std::endl;
        auto it = currentSection_->codeChunks.begin();
        while (it != currentSection_->codeChunks.end()) {
            if (it->second.empty()) {
                std::cout << "[DEBUG] Removing empty code chunk: " << it->first << std::endl;
                it = currentSection_->codeChunks.erase(it);
            }
            else {
                ++it;
            }
        }

        // Third pass: write code to memory
        WriteCodeToMemory();

        // Fourth pass: cleanup commands
        for (const auto& cmd : sectionIt->commands) {
            try {
                if (cmd.type == CommandType::Dealloc) {
                    HandleDealloc(cmd);
                }
                else if (cmd.type == CommandType::UnregisterSymbol) {
                    HandleUnregisterSymbol(cmd);
                }
            }
            catch (const std::exception& e) {
                ReportError(e.what(), cmd.lineNumber);
            }
        }

        currentSection_ = nullptr;
    }

    void ScriptParser::HandleAobscan(const ParsedCommand& cmd) {
        if (cmd.arguments.size() < 2) {
            throw EngineException(ErrorCode::InvalidParameter,
                "aobscan requires name and pattern");
        }

        std::string name = cmd.arguments[0];
        std::string pattern;

        // Combine all remaining arguments as pattern
        for (size_t i = 1; i < cmd.arguments.size(); ++i) {
            if (i > 1) pattern += " ";
            pattern += cmd.arguments[i];
        }

        // Scan all modules
        auto results = engine_->Scanner()->ScanAll(pattern);
        if (!results.empty()) {
            // Register the first result as a symbol
            engine_->Symbols()->RegisterSymbol(name, results[0].address);
            if (currentSection_) {
                currentSection_->allocations[name] = results[0].address;
            }
        }
        else {
            throw EngineException(ErrorCode::PatternNotFound,
                "Pattern not found: " + pattern);
        }
    }

    void ScriptParser::HandleAobscanmodule(const ParsedCommand& cmd) {
        if (cmd.arguments.size() < 3) {
            throw EngineException(ErrorCode::InvalidParameter,
                "aobscanmodule requires name, module, and pattern");
        }

        std::string name = cmd.arguments[0];
        std::string module = cmd.arguments[1];

        // Combine remaining arguments as pattern
        std::string pattern;
        for (size_t i = 2; i < cmd.arguments.size(); ++i) {
            if (i > 2) pattern += " ";
            pattern += cmd.arguments[i];
        }

        // Scan specific module
        auto result = engine_->Scanner()->ScanModule(module, pattern);
        if (result) {
            // Register as a symbol
            engine_->Symbols()->RegisterSymbol(name, result->address);
            if (currentSection_) {
                currentSection_->allocations[name] = result->address;
                currentSection_->labels[name] = result->address;
            }
        }
        else {
            throw EngineException(ErrorCode::PatternNotFound,
                "Pattern not found in module " + module + ": " + pattern);
        }
    }

    void ScriptParser::HandleAlloc(const ParsedCommand& cmd) {
        if (cmd.arguments.size() < 2) {
            throw EngineException(ErrorCode::InvalidParameter,
                "alloc requires name and size");
        }

        std::string name = cmd.arguments[0];

        // Parse size - default to hex unless # prefix for decimal
        size_t size = 0;
        std::string sizeStr = cmd.arguments[1];

        if (sizeStr.size() > 1 && sizeStr[0] == '$') {
            // Hex with $ prefix (CE style)
            size = std::stoull(sizeStr.substr(1), nullptr, 16);
        }
        else if (sizeStr.size() > 1 && sizeStr[0] == '#') {
            // Decimal with # prefix
            size = std::stoull(sizeStr.substr(1), nullptr, 10);
        }
        else if (sizeStr.size() > 2 && sizeStr[0] == '0' &&
            (sizeStr[1] == 'x' || sizeStr[1] == 'X')) {
            // Hex with 0x prefix
            size = std::stoull(sizeStr, nullptr, 16);
        }
        else {
            // Default to hex
            size = std::stoull(sizeStr, nullptr, 16);
        }

        // Allocate near address if specified
        AddressType nearAddress = 0;
        if (cmd.arguments.size() > 2) {
            auto resolved = engine_->Symbols()->ResolveAddress(cmd.arguments[2]);
            if (resolved) {
                nearAddress = *resolved;
            }
        }

        // Allocate memory
        AddressType allocated = 0;
        if (nearAddress) {
            // Try to allocate near
            allocated = engine_->Memory()->AllocateNear(nearAddress, size);
            if (!allocated) {
                // Fallback to regular allocation
                allocated = engine_->Memory()->AllocateMemory(size);
            }
        }
        else {
            allocated = engine_->Memory()->AllocateMemory(size);
        }

        if (allocated) {
            // Register as symbol
            engine_->Symbols()->RegisterAllocation(name, allocated, size);
            if (currentSection_) {
                currentSection_->allocations[name] = allocated;
                currentSection_->labels[name] = allocated;
            }
        }
        else {
            throw EngineException(ErrorCode::AllocationError,
                "Failed to allocate memory");
        }

    }

    void ScriptParser::HandleDealloc(const ParsedCommand& cmd) {
        if (cmd.arguments.empty()) {
            throw EngineException(ErrorCode::InvalidParameter,
                "dealloc requires name or address");
        }

        // Resolve address
        auto address = engine_->Symbols()->ResolveAddress(cmd.arguments[0]);
        if (address) {
            engine_->Memory()->FreeMemory(*address);
            engine_->Symbols()->UnregisterSymbol(cmd.arguments[0]);
        }
    }

    void ScriptParser::HandleLabel(const ParsedCommand& cmd) {
        // Label handling is done in ExecuteSection
    }

    void ScriptParser::HandleRegisterSymbol(const ParsedCommand& cmd) {
        if (cmd.arguments.empty()) {
            throw EngineException(ErrorCode::InvalidParameter,
                "registersymbol requires name");
        }

        // Can register multiple symbols at once
        for (const auto& name : cmd.arguments) {
            // Check if it already exists in allocations
            if (currentSection_ && currentSection_->allocations.count(name)) {
                // Already registered during allocation
                continue;
            }

            // Check if it exists as a label
            if (currentSection_ && currentSection_->labels.count(name)) {
                engine_->Symbols()->RegisterSymbol(name, currentSection_->labels[name]);
            }
        }
    }

    void ScriptParser::HandleUnregisterSymbol(const ParsedCommand& cmd) {
        if (cmd.arguments.empty()) {
            throw EngineException(ErrorCode::InvalidParameter,
                "unregistersymbol requires name");
        }

        for (const auto& name : cmd.arguments) {
            engine_->Symbols()->UnregisterSymbol(name);
        }
    }

    void ScriptParser::HandleDefine(const ParsedCommand& cmd) {
        if (cmd.arguments.size() < 2) {
            throw EngineException(ErrorCode::InvalidParameter,
                "define requires name and value");
        }

        defines_[cmd.arguments[0]] = cmd.arguments[1];
    }

    void ScriptParser::HandleInclude(const ParsedCommand& cmd) {
        if (cmd.arguments.empty()) {
            throw EngineException(ErrorCode::InvalidParameter,
                "include requires filename");
        }

        // Parse included file
        ParseFile(cmd.arguments[0]);
    }

    void ScriptParser::HandleAsm(const ParsedCommand& cmd) {
        // Assembly handling is done in ExecuteSection
    }

    void ScriptParser::HandleDataDefinition(const ParsedCommand& cmd) {
        std::vector<uint8_t> data;

        if (cmd.type == CommandType::Db) {
            // Define bytes - handle captures and hex values
            for (const auto& arg : cmd.arguments) {
                if (engine_->Captures()->Exists(arg)) {
                    auto capture = engine_->Captures()->Get(arg);
                    if (capture) {
                        for (uint8_t byte : capture->data) {
                            data.push_back(byte);
                        }
                    }
                }
                else {
                    // Use the common number parser
                    uint64_t value = ParseNumber(arg);
                    data.push_back(value & 0xFF);
                }
            }
        }
        else if (cmd.type == CommandType::Dd) {
            // Define dwords
            for (const auto& arg : cmd.arguments) {
                uint32_t value = static_cast<uint32_t>(ParseNumber(arg));
                data.push_back(value & 0xFF);
                data.push_back((value >> 8) & 0xFF);
                data.push_back((value >> 16) & 0xFF);
                data.push_back((value >> 24) & 0xFF);
            }
        }
        else if (cmd.type == CommandType::Dq) {
            // Define qwords
            for (const auto& arg : cmd.arguments) {
                uint64_t value = ParseNumber(arg);
                for (int i = 0; i < 8; i++) {
                    data.push_back((value >> (i * 8)) & 0xFF);
                }
            }
        }

        // Store data
        if (currentSection_ && !data.empty()) {
            // Find current label
            std::string targetLabel;
            AddressType targetAddr = 0;

            // Find the most recent label
            for (const auto& [label, addr] : currentSection_->labels) {
                if (addr <= GetCurrentAddress() && addr > targetAddr) {
                    targetAddr = addr;
                    targetLabel = label;
                }
            }

            if (!targetLabel.empty()) {
                currentSection_->codeChunks[targetLabel].insert(
                    currentSection_->codeChunks[targetLabel].end(),
                    data.begin(), data.end()
                );
            }
        }
    }

    void ScriptParser::BeginSection(const std::string& name) {
        EndSection(); // End current section if any

        ScriptSection section;
        section.name = name;
        section.isEnabled = true;
        sections_.push_back(section);

        currentSection_ = &sections_.back();
    }

    void ScriptParser::EndSection() {
        currentSection_ = nullptr;
    }

    void ScriptParser::EnableSection(const std::string& sectionName) {
        auto it = std::find_if(sections_.begin(), sections_.end(),
            [&](ScriptSection& s) { return s.name == sectionName; });
        if (it != sections_.end()) {
            it->isEnabled = true;
        }
    }

    void ScriptParser::DisableSection(const std::string& sectionName) {
        auto it = std::find_if(sections_.begin(), sections_.end(),
            [&](ScriptSection& s) { return s.name == sectionName; });
        if (it != sections_.end()) {
            it->isEnabled = false;
        }
    }

    std::vector<std::string> ScriptParser::GetSectionNames() const {
        std::vector<std::string> names;
        for (const auto& section : sections_) {
            names.push_back(section.name);
        }
        return names;
    }

    std::optional<ScriptSection> ScriptParser::GetSection(const std::string& name) const {
        auto it = std::find_if(sections_.begin(), sections_.end(),
            [&](const ScriptSection& s) { return s.name == name; });
        if (it != sections_.end()) {
            return *it;
        }
        return std::nullopt;
    }

    void ScriptParser::Clear() {
        sections_.clear();
        defines_.clear();
        currentSection_ = nullptr;
        forwardJumps_.clear();
        labelAddresses_.clear();
        anonymousLabelCounter_ = 0;
    }

    void ScriptParser::SetErrorCallback(ErrorCallback callback) {
        errorCallback_ = callback;
    }

    void ScriptParser::ReportError(const std::string& message, size_t lineNumber) {
        if (errorCallback_) {
            errorCallback_(message, lineNumber);
        }
        else {
            throw EngineException(ErrorCode::InvalidParameter,
                "Line " + std::to_string(lineNumber) + ": " + message);
        }
    }

    AddressType ScriptParser::GetCurrentAddress() const {
        if (!currentSection_) {
            return 0;
        }

        // For now, return the first allocation address
        // In a real implementation, you'd track the current write position more carefully
        if (!currentSection_->allocations.empty()) {
            return currentSection_->allocations.begin()->second;
        }

        return 0;
    }

    void ScriptParser::WriteCodeToMemory() {
        if (!currentSection_) {
            std::cout << "[ERROR] No current section in WriteCodeToMemory" << std::endl;
            return;
        }

        std::cout << "[DEBUG] WriteCodeToMemory: " << currentSection_->codeChunks.size()
            << " code chunks to write" << std::endl;

        // Write each code chunk to its corresponding address
        for (const auto& [label, code] : currentSection_->codeChunks) {
            if (code.empty()) {
                std::cout << "[DEBUG] Skipping empty code chunk for label: " << label << std::endl;
                continue;
            }

            AddressType baseAddr = 0;

            // Find base address for this label
            if (currentSection_->labels.count(label)) {
                baseAddr = currentSection_->labels[label];
                std::cout << "[DEBUG] Found label '" << label << "' in section labels: 0x"
                    << std::hex << baseAddr << std::dec << std::endl;
            }
            else if (currentSection_->allocations.count(label)) {
                baseAddr = currentSection_->allocations[label];
                std::cout << "[DEBUG] Found label '" << label << "' in allocations: 0x"
                    << std::hex << baseAddr << std::dec << std::endl;
            }
            else {
                // 尝试从符号管理器解析
                auto resolved = engine_->Symbols()->ResolveAddress(label);
                if (resolved) {
                    baseAddr = *resolved;
                    std::cout << "[DEBUG] Resolved label '" << label << "' from symbols: 0x"
                        << std::hex << baseAddr << std::dec << std::endl;
                }
            }

            if (baseAddr) {
                std::cout << "[DEBUG] Writing " << code.size() << " bytes to 0x"
                    << std::hex << baseAddr << " for label '" << label << "': ";

                // 显示前几个字节
                for (size_t i = 0; i < std::min<size_t>(code.size(), 16); ++i) {
                    std::cout << std::hex << std::setw(2) << std::setfill('0')
                        << (int)code[i] << " ";
                }
                if (code.size() > 16) {
                    std::cout << "...";
                }
                std::cout << std::dec << std::endl;

                // Write the code
                if (!engine_->Memory()->WriteMemory(baseAddr, code.data(), code.size())) {
                    DWORD error = GetLastError();
                    throw EngineException(ErrorCode::MemoryAccessError,
                        "Failed to write code for label: " + label +
                        " at 0x" + std::to_string(baseAddr) +
                        ", Windows error: " + std::to_string(error));
                }
                else {
                    std::cout << "[DEBUG] Successfully wrote code for label: " << label << std::endl;

                    // 验证写入
                    std::vector<uint8_t> verify(code.size());
                    if (engine_->Memory()->ReadMemory(baseAddr, verify.data(), verify.size())) {
                        bool match = true;
                        for (size_t i = 0; i < code.size(); ++i) {
                            if (verify[i] != code[i]) {
                                match = false;
                                break;
                            }
                        }

                        if (match) {
                            std::cout << "[DEBUG] Write verification successful" << std::endl;
                        }
                        else {
                            std::cout << "[ERROR] Write verification failed - data mismatch!" << std::endl;
                        }
                    }
                }
            }
            else {
                std::cout << "[ERROR] No base address found for label: " << label << std::endl;
            }
        }
    }

} // namespace AsmEngine