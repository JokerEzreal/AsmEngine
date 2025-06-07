#include "ScriptParser.h"
#include "AsmEngine.h"
#include <fstream>
#include <regex>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <set>

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

        std::cout << "[DEBUG] Starting script parse..." << std::endl;

        while (std::getline(stream, line)) {
            lineNumber++;

            // Store original line for debugging
            std::string originalLine = line;

            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);

            // Skip empty lines and comments
            if (line.empty() || line[0] == ';' ||
                (line.size() >= 2 && line[0] == '/' && line[1] == '/')) {
                continue;
            }

            // Debug output for non-empty lines
            if (lineNumber <= 50) {  // Only show first 50 lines to avoid too much output
                std::cout << "[DEBUG] Line " << lineNumber << ": '" << originalLine << "'" << std::endl;
                std::cout << "[DEBUG]   Trimmed: '" << line << "'" << std::endl;
            }

            // Check for section markers
            if (line == "[ENABLE]") {
                std::cout << "[DEBUG]   -> Begin ENABLE section" << std::endl;
                BeginSection("ENABLE");
                continue;
            }
            else if (line == "[DISABLE]") {
                std::cout << "[DEBUG]   -> Begin DISABLE section" << std::endl;
                BeginSection("DISABLE");
                continue;
            }

            try {
                // Preprocess line for CE syntax
                line = PreprocessLine(line);

                // Check if it's a label (including label+offset)
                if (IsLabel(line)) {
                    auto [labelName, offset] = ParseLabelWithOffset(line);

                    std::cout << "[DEBUG]   -> Label definition: " << labelName;
                    if (offset > 0) {
                        std::cout << " + " << offset;
                    }
                    std::cout << std::endl;

                    ParsedCommand cmd;
                    cmd.type = CommandType::Label;
                    cmd.name = labelName;  // 重要：这里应该是标签名，而不是 "label"
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

                if (lineNumber <= 50) {
                    std::cout << "[DEBUG]   -> Command: " << cmd.name
                        << " (type=" << static_cast<int>(cmd.type)
                        << ", Asm=" << static_cast<int>(CommandType::Asm) << ")" << std::endl;
                }

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

        // Debug: Show total commands parsed
        std::cout << "[DEBUG] Parse complete. Total sections: " << sections_.size() << std::endl;
        for (const auto& section : sections_) {
            std::cout << "[DEBUG] Section '" << section.name << "' has "
                << section.commands.size() << " commands" << std::endl;
        }
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
            std::string anonLabel = "__anon_label_" + std::to_string(anonymousLabelCounter_);

            // 注册这个标签供后续引用
            // 注意：地址将在后续根据上下文确定
            std::cout << "[DEBUG] Created anonymous label: " << anonLabel << std::endl;

            return anonLabel + ":";
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

        if (processedLine.find("label(") == 0) {
            // Extract label command and its argument
            size_t startParen = processedLine.find('(');
            size_t endParen = processedLine.find(')');

            if (startParen != std::string::npos && endParen != std::string::npos &&
                endParen > startParen) {
                tokens.push_back("label");  // Command is "label"
                std::string labelName = processedLine.substr(startParen + 1,
                    endParen - startParen - 1);
                tokens.push_back(labelName);
                return tokens;
            }
        }

        // Handle parentheses syntax for commands
        std::regex cmdWithParens(R"(^(\w+)\s*\((.*)\)\s*$)");
        std::smatch match;

        if (std::regex_match(processedLine, match, cmdWithParens)) {
            // Command with parentheses
            tokens.push_back(match[1].str()); // Command name

            // Parse arguments inside parentheses
            std::string args = match[2].str();
            if (!args.empty()) {
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

        // ============ 第1遍：处理 AOB扫描、内存分配、label()声明 ============
        std::cout << "[DEBUG] Pass 1: Processing allocations, scans and label declarations..." << std::endl;

        // Track declared labels for forward reference resolution
        std::set<std::string> declaredLabels;

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

                case CommandType::Label:
                    // Only handle label() declarations here
                    if (cmd.name == "label" && !cmd.arguments.empty()) {
                        declaredLabels.insert(cmd.arguments[0]);
                        std::cout << "[DEBUG] Declared label: " << cmd.arguments[0] << std::endl;
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

        // ============ 预解析遍：解析所有标签定义和它们的地址 ============
        std::cout << "[DEBUG] Pre-resolve pass: Finding all label definitions..." << std::endl;

        // Map to store pre-resolved label addresses
        std::map<std::string, AddressType> preresolvedLabels;

        for (size_t i = 0; i < sectionIt->commands.size(); i++) {
            const auto& cmd = sectionIt->commands[i];

            // Look for label definitions (not label() declarations)
            if (cmd.type == CommandType::Label && cmd.name != "label") {
                std::string baseLabelName = cmd.arguments[0];
                size_t offset = 0;

                std::cout << "[DEBUG] Found label definition in pre-resolve: " << baseLabelName << std::endl;

                if (cmd.arguments.size() > 1) {
                    offset = std::stoull(cmd.arguments[1]);
                }

                // Try to resolve base address
                AddressType baseAddr = 0;
                bool hasBase = false;

                auto resolvedAddr = engine_->Symbols()->ResolveAddress(baseLabelName);
                if (resolvedAddr) {
                    baseAddr = *resolvedAddr;
                    hasBase = true;
                }
                else if (currentSection_->allocations.count(baseLabelName)) {
                    baseAddr = currentSection_->allocations[baseLabelName];
                    hasBase = true;
                }

                if (hasBase) {
                    AddressType currentAddr = baseAddr + offset;

                    // Look ahead for data labels that follow immediately
                    for (size_t j = i + 1; j < sectionIt->commands.size(); j++) {
                        const auto& nextCmd = sectionIt->commands[j];

                        // Stop if we hit another label with offset
                        if (nextCmd.type == CommandType::Label &&
                            nextCmd.arguments.size() > 1 &&
                            nextCmd.name != "label") {
                            break;
                        }

                        // If it's a simple label definition (labelname:)
                        if (nextCmd.type == CommandType::Label &&
                            nextCmd.arguments.size() == 1 &&
                            nextCmd.name != "label") {
                            std::string sublabel = nextCmd.arguments[0];
                            preresolvedLabels[sublabel] = currentAddr;

                            std::cout << "[DEBUG] Pre-resolving data label '" << sublabel
                                << "' at offset from " << baseLabelName << std::endl;

                            // Look at the next command to determine size
                            if (j + 1 < sectionIt->commands.size()) {
                                const auto& dataCmd = sectionIt->commands[j + 1];
                                if (dataCmd.type == CommandType::Dd) {
                                    currentAddr += 4;  // dd is 4 bytes
                                }
                                else if (dataCmd.type == CommandType::Dq) {
                                    currentAddr += 8;  // dq is 8 bytes
                                }
                                else if (dataCmd.type == CommandType::Db) {
                                    currentAddr += dataCmd.arguments.size();  // db is 1 byte per argument
                                }
                            }
                        }
                        else if (nextCmd.type != CommandType::Dd &&
                            nextCmd.type != CommandType::Dq &&
                            nextCmd.type != CommandType::Db) {
                            // Any other command type, stop scanning
                            break;
                        }
                    }
                }
            }
        }

        // Register all pre-resolved labels
        for (const auto& [label, addr] : preresolvedLabels) {
            engine_->Symbols()->RegisterSymbol(label, addr);
            currentSection_->labels[label] = addr;
            std::cout << "[DEBUG] Pre-resolved label '" << label
                << "' at 0x" << std::hex << addr << std::dec << std::endl;
        }

        // ============ 计算代码标签位置 ============
        std::cout << "[DEBUG] Calculating code label positions..." << std::endl;

        // Map to track anonymous labels and code labels
        std::map<std::string, std::vector<AddressType>> anonLabels; // Track @@ labels per section
        std::map<std::string, AddressType> codeLabelAddresses;
        AddressType estimatedAddress = 0;
        std::string currentCodeSection;
        int globalAnonCounter = 0;

        for (size_t i = 0; i < sectionIt->commands.size(); i++) {
            const auto& cmd = sectionIt->commands[i];

            // Track current position for labels
            if (cmd.type == CommandType::Label && cmd.name != "label") {
                std::string labelName = cmd.arguments[0];

                // Handle base labels with offsets
                if (cmd.arguments.size() > 1) {
                    size_t offset = std::stoull(cmd.arguments[1]);
                    auto baseAddr = engine_->Symbols()->ResolveAddress(labelName);
                    if (baseAddr || currentSection_->allocations.count(labelName)) {
                        AddressType addr = baseAddr ? *baseAddr : currentSection_->allocations[labelName];
                        estimatedAddress = addr + offset;
                        currentCodeSection = labelName;
                        if (offset > 0) {
                            currentCodeSection += "_offset_" + std::to_string(offset);
                        }
                    }
                }
                // Handle anonymous labels
                else if (labelName == "@@") {
                    if (!currentCodeSection.empty() && estimatedAddress != 0) {
                        anonLabels[currentCodeSection].push_back(estimatedAddress);
                        globalAnonCounter++;
                        std::string anonName = "__anon_label_" + std::to_string(globalAnonCounter);
                        engine_->Symbols()->RegisterSymbol(anonName, estimatedAddress);
                        std::cout << "[DEBUG] Registered anonymous label '" << anonName
                            << "' at 0x" << std::hex << estimatedAddress << std::dec
                            << " in section " << currentCodeSection << std::endl;
                    }
                }
                // Handle named labels within code sections
                else if (!currentCodeSection.empty() && estimatedAddress != 0) {
                    // This is a code label like "code:" or "return:"
                    codeLabelAddresses[labelName] = estimatedAddress;
                    engine_->Symbols()->RegisterSymbol(labelName, estimatedAddress);
                    currentSection_->labels[labelName] = estimatedAddress;
                    std::cout << "[DEBUG] Registered code label '" << labelName
                        << "' at 0x" << std::hex << estimatedAddress << std::dec << std::endl;
                }
            }
            // Estimate size for assembly instructions
            else if (cmd.type == CommandType::Asm && !currentCodeSection.empty() && estimatedAddress != 0) {
                // Estimate instruction sizes
                if (cmd.name == "nop_multiple" && !cmd.arguments.empty()) {
                    estimatedAddress += std::stoi(cmd.arguments[0]);
                }
                else if (cmd.name == "push" || cmd.name == "pop") {
                    estimatedAddress += 1;  // push/pop reg is 1 byte
                }
                else if (cmd.name == "mov" || cmd.name == "lea" || cmd.name == "cmp") {
                    estimatedAddress += 7;  // Rough estimate
                }
                else if (cmd.name == "test") {
                    estimatedAddress += 3;  // test reg,reg is 3 bytes
                }
                else if (cmd.name == "jmp" || cmd.name == "je" || cmd.name == "jne") {
                    estimatedAddress += 5;  // Near jump
                }
                else if (cmd.name == "movss") {
                    estimatedAddress += 4;  // SSE instruction
                }
                else {
                    estimatedAddress += 5;  // Default estimate
                }
            }
        }

        // ============ 第2遍：处理标签和代码生成 ============
        std::cout << "[DEBUG] Pass 2: Processing labels and assembly..." << std::endl;
        std::cout << "[DEBUG] Total commands to process: " << sectionIt->commands.size() << std::endl;

        std::string currentLabel;
        AddressType currentWriteAddress = 0;
        AddressType currentBaseAddress = 0;
        bool hasValidLabel = false;
        int localAnonCounter = 0;
        std::string currentLabelSection;

        for (size_t cmdIndex = 0; cmdIndex < sectionIt->commands.size(); cmdIndex++) {
            const auto& cmd = sectionIt->commands[cmdIndex];

            try {
                // Handle label definitions (not declarations)
                if (cmd.type == CommandType::Label && cmd.name != "label") {
                    // Clean up previous empty label
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

                    std::cout << "[DEBUG] Processing label: " << baseLabelName;
                    if (offset > 0) {
                        std::cout << " + 0x" << std::hex << offset;
                    }
                    std::cout << std::dec << std::endl;

                    // Handle anonymous labels
                    if (baseLabelName == "@@") {
                        // Anonymous labels are already registered, skip
                        continue;
                    }

                    // Try to resolve base address
                    AddressType baseAddr = 0;
                    bool resolved = false;

                    auto resolvedAddr = engine_->Symbols()->ResolveAddress(baseLabelName);
                    if (resolvedAddr) {
                        baseAddr = *resolvedAddr;
                        resolved = true;
                        std::cout << "[DEBUG] Resolved from symbols: 0x" << std::hex << baseAddr << std::dec << std::endl;
                    }
                    else if (currentSection_->allocations.count(baseLabelName)) {
                        baseAddr = currentSection_->allocations[baseLabelName];
                        resolved = true;
                        std::cout << "[DEBUG] Found in allocations: 0x" << std::hex << baseAddr << std::dec << std::endl;
                    }

                    if (resolved) {
                        currentWriteAddress = baseAddr + offset;
                        currentBaseAddress = baseAddr;
                        currentLabel = baseLabelName;
                        currentLabelSection = baseLabelName;
                        localAnonCounter = 0;

                        if (offset > 0) {
                            currentLabel = baseLabelName + "_offset_" + std::to_string(offset);
                            currentLabelSection = currentLabel;
                        }

                        currentSection_->labels[currentLabel] = currentWriteAddress;
                        engine_->Symbols()->RegisterLabel(currentLabel, currentWriteAddress);
                        hasValidLabel = true;

                        std::cout << "[DEBUG] Current label: " << currentLabel
                            << " at 0x" << std::hex << currentWriteAddress << std::dec << std::endl;
                    }
                    else {
                        // Check if it's a code label that was pre-registered
                        if (currentSection_->labels.count(baseLabelName)) {
                            currentWriteAddress = currentSection_->labels[baseLabelName];
                            currentLabel = baseLabelName;
                            hasValidLabel = true;
                            std::cout << "[DEBUG] Using pre-registered label '" << baseLabelName
                                << "' at 0x" << std::hex << currentWriteAddress << std::dec << std::endl;
                        }
                        else {
                            currentLabel = baseLabelName;
                            hasValidLabel = false;
                            std::cout << "[WARNING] Label '" << baseLabelName
                                << "' has no context" << std::endl;
                        }
                    }
                }
                // Skip label() declarations
                else if (cmd.type == CommandType::Label && cmd.name == "label") {
                    continue;
                }
                // Skip data definitions - they're handled during pre-resolution
                else if (cmd.type == CommandType::Db ||
                    cmd.type == CommandType::Dd ||
                    cmd.type == CommandType::Dq) {
                    continue;
                }
                // Skip certain script commands
                else if (cmd.type == CommandType::RegisterSymbol ||
                    cmd.type == CommandType::UnregisterSymbol ||
                    cmd.type == CommandType::Aobscanmodule ||
                    cmd.type == CommandType::Alloc ||
                    cmd.type == CommandType::Dealloc) {
                    continue;
                }
                // Process assembly instructions
                else if (cmd.type == CommandType::Asm && hasValidLabel && currentWriteAddress > 0) {
                    // Build assembly instruction
                    std::string asmLine = cmd.name;
                    for (const auto& arg : cmd.arguments) {
                        asmLine += " " + arg;
                    }

                    // Handle anonymous label references
                    if (asmLine.find("__anon_forward") != std::string::npos ||
                        asmLine.find("__anon_backward") != std::string::npos ||
                        asmLine.find("@f") != std::string::npos ||
                        asmLine.find("@b") != std::string::npos) {

                        // Replace with actual anonymous label
                        if (asmLine.find("__anon_forward") != std::string::npos ||
                            asmLine.find("@f") != std::string::npos) {
                            // Find next anonymous label
                            if (anonLabels.count(currentLabelSection)) {
                                const auto& sectionAnons = anonLabels[currentLabelSection];
                                for (AddressType anonAddr : sectionAnons) {
                                    if (anonAddr > currentWriteAddress) {

                                        auto symbols = engine_->Symbols()->GetAllSymbols();
                                        for (const auto& symbol : symbols) {
                                            if (symbol.address == anonAddr && symbol.name.find("__anon_label_") == 0) {
                                                asmLine = cmd.name + " " + symbol.name;
                                                break;
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    std::cout << "[DEBUG] Assembling for " << currentLabel
                        << " at 0x" << std::hex << currentWriteAddress
                        << ": " << asmLine << std::dec << std::endl;

                    // Special handling for "nop_multiple"
                    if (cmd.name == "nop_multiple" && !cmd.arguments.empty()) {
                        int count = std::stoi(cmd.arguments[0]);
                        std::vector<uint8_t> nops(count, 0x90);

                        currentSection_->codeChunks[currentLabel].insert(
                            currentSection_->codeChunks[currentLabel].end(),
                            nops.begin(), nops.end()
                        );
                        currentWriteAddress += count;
                    }
                    else {
                        // Regular assembly instruction
                        auto result = engine_->Assembly()->AssembleInstruction(asmLine, currentWriteAddress);
                        if (result && !result->empty()) {
                            std::cout << "[DEBUG] Assembled " << result->size() << " bytes: ";
                            for (size_t i = 0; i < std::min<size_t>(result->size(), 16); ++i) {
                                std::cout << std::hex << std::setw(2) << std::setfill('0')
                                    << (int)(*result)[i] << " ";
                            }
                            std::cout << std::setfill(' ');  // Reset fill character
                            if (result->size() > 16) {
                                std::cout << "...";
                            }
                            std::cout << std::dec << std::endl;

                            currentSection_->codeChunks[currentLabel].insert(
                                currentSection_->codeChunks[currentLabel].end(),
                                result->begin(), result->end()
                            );

                            currentWriteAddress += result->size();
                        }
                        else {
                            std::cout << "[ERROR] Failed to assemble: " << asmLine << std::endl;
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                ReportError(e.what(), cmd.lineNumber);
            }
        }

        // Clean up last label if empty
        if (!currentLabel.empty() &&
            currentSection_->codeChunks.count(currentLabel) > 0 &&
            currentSection_->codeChunks[currentLabel].empty()) {
            std::cout << "[DEBUG] Removing empty code chunk for last label: " << currentLabel << std::endl;
            currentSection_->codeChunks.erase(currentLabel);
        }

        // ============ 第3遍：写入内存 ============
        WriteCodeToMemory();

        // ============ 第4遍：清理 ============
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
        if (cmd.arguments.empty()) {
            throw EngineException(ErrorCode::InvalidParameter,
                "label command requires a name");
        }

        std::string labelName = cmd.arguments[0];

        // For label() command, just mark it as a forward declaration
        // The actual address will be determined when we see labelname:
        std::cout << "[DEBUG] Forward declaration of label: " << labelName << std::endl;

        // We don't register it yet - just note that it exists
        // The actual registration happens when we process "labelname:" later
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
            // Define bytes - 正确处理捕获值
            for (const auto& arg : cmd.arguments) {
                if (engine_->Captures()->Exists(arg)) {
                    auto capture = engine_->Captures()->Get(arg);
                    if (capture) {
                        // 对于 db，如果捕获是多字节，只使用第一个字节
                        // 除非明确要求使用所有字节
                        if (capture->size == 1) {
                            data.push_back(capture->AsUInt8());
                        }
                        else {
                            // 对于 s1 这样的2字节捕获，在 db 中应该如何处理？
                            // CE 可能期望插入所有字节
                            for (uint8_t byte : capture->data) {
                                data.push_back(byte);
                            }
                        }
                    }
                }
                else {
                    // 使用通用数字解析器
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
                // Special handling for hook locations
                if (label.find("aobplayer") != std::string::npos && label.find("_offset_") != std::string::npos) {
                    // This is a hook at aobplayer+D
                    // Check if the code is trying to jump to newmem
                    if (code.size() >= 12 && code[0] == 0x48 && code[1] == 0xB8) {
                        // This looks like: mov rax, imm64; jmp rax
                        // Try to extract the target address
                        uint64_t targetAddr = 0;
                        for (int i = 0; i < 8; ++i) {
                            targetAddr |= static_cast<uint64_t>(code[2 + i]) << (i * 8);
                        }

                        std::cout << "[DEBUG] Detected absolute jump to 0x" << std::hex << targetAddr
                            << " at hook location" << std::dec << std::endl;

                        // Generate proper E9 relative jump
                        auto relJump = engine_->Assembly()->GenerateJump(baseAddr, targetAddr);
                        if (!relJump.empty() && relJump[0] == 0xE9) {
                            // Use relative jump
                            std::cout << "[DEBUG] Replacing with E9 relative jump" << std::endl;

                            // Write the relative jump
                            if (!engine_->Memory()->WriteMemory(baseAddr, relJump.data(), relJump.size())) {
                                DWORD error = GetLastError();
                                throw EngineException(ErrorCode::MemoryAccessError,
                                    "Failed to write relative jump at: 0x" +
                                    std::to_string(baseAddr) + ", error: " + std::to_string(error));
                            }

                            // Verify write
                            std::vector<uint8_t> verify(relJump.size());
                            if (engine_->Memory()->ReadMemory(baseAddr, verify.data(), verify.size())) {
                                std::cout << "[DEBUG] Hook installed: ";
                                for (size_t i = 0; i < verify.size(); ++i) {
                                    std::cout << std::hex << std::setw(2) << std::setfill('0')
                                        << (int)verify[i] << " ";
                                }
                                std::cout << std::dec << std::endl;
                            }
                            continue;
                        }
                    }
                }

                // Normal write
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

    void ScriptParser::PreResolveLabels() {
        std::cout << "[DEBUG] Pre-resolving all labels..." << std::endl;

        if (!currentSection_) return;

        // First pass: Find all label declarations and their eventual addresses
        std::map<std::string, AddressType> prelabelMap;

        // Scan through all commands looking for patterns like:
        // newmem+offset:
        // labelname:
        for (size_t i = 0; i < currentSection_->commands.size(); i++) {
            const auto& cmd = currentSection_->commands[i];

            if (cmd.type == CommandType::Label) {
                std::string baseLabelName = cmd.arguments[0];
                size_t offset = 0;

                if (cmd.arguments.size() > 1) {
                    offset = std::stoull(cmd.arguments[1]);
                }

                // Try to resolve base address
                AddressType baseAddr = 0;
                bool hasBase = false;

                auto resolvedAddr = engine_->Symbols()->ResolveAddress(baseLabelName);
                if (resolvedAddr) {
                    baseAddr = *resolvedAddr;
                    hasBase = true;
                }
                else if (currentSection_->allocations.count(baseLabelName)) {
                    baseAddr = currentSection_->allocations[baseLabelName];
                    hasBase = true;
                }

                if (hasBase) {
                    AddressType labelAddr = baseAddr + offset;

                    // Look ahead for immediate following labels
                    for (size_t j = i + 1; j < currentSection_->commands.size(); j++) {
                        const auto& nextCmd = currentSection_->commands[j];

                        // Stop if we hit another offset label
                        if (nextCmd.type == CommandType::Label && nextCmd.arguments.size() > 1) {
                            break;
                        }

                        // If it's a simple label, register it at current address
                        if (nextCmd.type == CommandType::Label && nextCmd.arguments.size() == 1) {
                            std::string sublabel = nextCmd.arguments[0];
                            prelabelMap[sublabel] = labelAddr;

                            // Advance address based on next data command
                            if (j + 1 < currentSection_->commands.size()) {
                                const auto& dataCmd = currentSection_->commands[j + 1];
                                if (dataCmd.type == CommandType::Dd) {
                                    labelAddr += 4 * dataCmd.arguments.size();
                                }
                                else if (dataCmd.type == CommandType::Dq) {
                                    labelAddr += 8 * dataCmd.arguments.size();
                                }
                                else if (dataCmd.type == CommandType::Db) {
                                    labelAddr += dataCmd.arguments.size();
                                }
                            }
                        }
                        else if (nextCmd.type == CommandType::Dd ||
                            nextCmd.type == CommandType::Dq ||
                            nextCmd.type == CommandType::Db) {
                            // Skip data definitions
                            continue;
                        }
                        else {
                            // Any other command type, stop scanning
                            break;
                        }
                    }
                }
            }
        }

        // Register all pre-resolved labels
        for (const auto& [label, addr] : prelabelMap) {
            engine_->Symbols()->RegisterSymbol(label, addr);
            currentSection_->labels[label] = addr;
            std::cout << "[DEBUG] Pre-resolved label '" << label
                << "' to 0x" << std::hex << addr << std::dec << std::endl;
        }
    }
    void ScriptParser::ResolveForwardReferences() {
        // For CE scripts, we need to handle forward references
        // Labels declared with label() may be defined later in the script

        std::cout << "[DEBUG] Resolving forward references..." << std::endl;

        // First, find all label declarations
        std::set<std::string> declaredLabels;
        std::map<std::string, AddressType> definedLabels;

        for (const auto& section : sections_) {
            for (const auto& cmd : section.commands) {
                if (cmd.type == CommandType::Label && cmd.name == "label" && !cmd.arguments.empty()) {
                    // This is a label() declaration
                    declaredLabels.insert(cmd.arguments[0]);
                    std::cout << "[DEBUG] Found label declaration: " << cmd.arguments[0] << std::endl;
                }
            }

            // Also check section labels
            for (const auto& [label, addr] : section.labels) {
                definedLabels[label] = addr;
            }
        }

        // For labels that are declared but not yet defined, we need to find where they appear
        // In CE scripts, labels often appear at specific offsets like newmem+400
        for (const auto& labelName : declaredLabels) {
            if (definedLabels.find(labelName) == definedLabels.end()) {
                // This label is declared but not yet defined
                // It might be defined later as part of data sections

                // For now, register it as a placeholder
                // The actual address will be determined during assembly
                std::cout << "[DEBUG] Forward reference label '" << labelName
                    << "' will be resolved during assembly" << std::endl;
            }
        }
    }

} // namespace AsmEngine