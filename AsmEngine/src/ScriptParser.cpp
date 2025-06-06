#include "ScriptParser.h"
#include "AsmEngine.h"
#include <fstream>
#include <regex>
#include <algorithm>
#include <sstream>
#include <iomanip>

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

            // Parse offset (can be hex or decimal)
            size_t offset = 0;
            if (offsetStr.size() > 2 && offsetStr[0] == '0' &&
                (offsetStr[1] == 'x' || offsetStr[1] == 'X')) {
                offset = std::stoull(offsetStr, nullptr, 16);
            }
            else {
                offset = std::stoull(offsetStr, nullptr, 10);
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

        // Fix parentheses for common commands
        const std::vector<std::string> commands = {
            "alloc(", "aobscanmodule(", "dealloc(", "registersymbol(", "unregistersymbol("
        };

        for (const auto& cmd : commands) {
            size_t pos = processedLine.find(cmd);
            if (pos != std::string::npos) {
                // Replace '(' with space
                processedLine[pos + cmd.length() - 1] = ' ';

                // Remove closing ')'
                size_t closePos = processedLine.find(')', pos);
                if (closePos != std::string::npos) {
                    processedLine.erase(closePos, 1);
                }
            }
        }

        // Handle multiple arguments in registersymbol/unregistersymbol
        if (processedLine.find("registersymbol") != std::string::npos ||
            processedLine.find("unregistersymbol") != std::string::npos) {
            // These can have multiple space-separated arguments
            std::istringstream iss(processedLine);
            std::string token;

            while (iss >> token) {
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

        for (const auto& cmd : sectionIt->commands) {
            try {
                switch (cmd.type) {
                case CommandType::Label:
                    currentLabel = cmd.arguments[0];
                    if (cmd.arguments.size() > 1) {
                        // Label with offset
                        size_t offset = std::stoull(cmd.arguments[1]);
                        // Create a new label at base + offset
                        auto baseAddr = currentSection_->labels[currentLabel];
                        if (baseAddr == 0 && currentSection_->allocations.count(currentLabel)) {
                            baseAddr = currentSection_->allocations[currentLabel];
                        }
                        currentLabel = currentLabel + "_" + std::to_string(offset);
                        currentSection_->labels[currentLabel] = baseAddr + offset;
                    }
                    break;

                case CommandType::Asm:
                    if (!currentLabel.empty()) {
                        // Assemble and add to current label's buffer
                        HandleAsm(cmd);
                    }
                    break;

                case CommandType::Db:
                case CommandType::Dd:
                case CommandType::Dq:
                    if (!currentLabel.empty()) {
                        HandleDataDefinition(cmd);
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

        // Third pass: resolve jumps and write code
        WriteCodeToMemory();

        // Fourth pass: cleanup commands
        for (const auto& cmd : sectionIt->commands) {
            if (cmd.type == CommandType::Dealloc) {
                HandleDealloc(cmd);
            }
            else if (cmd.type == CommandType::UnregisterSymbol) {
                HandleUnregisterSymbol(cmd);
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
        std::string pattern = cmd.arguments[1];

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

        // Parse size (can be decimal or hex)
        size_t size = 0;
        std::string sizeStr = cmd.arguments[1];

        if (sizeStr.size() > 1 && sizeStr[0] == '$') {
            // Hex with $ prefix
            size = std::stoull(sizeStr.substr(1), nullptr, 16);
        }
        else if (sizeStr.size() > 2 && sizeStr[0] == '0' &&
            (sizeStr[1] == 'x' || sizeStr[1] == 'X')) {
            // Hex with 0x prefix
            size = std::stoull(sizeStr, nullptr, 16);
        }
        else {
            // Decimal
            size = std::stoull(sizeStr, nullptr, 10);
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
                "label requires name");
        }

        std::string labelName = cmd.arguments[0];

        // If it's a label with offset, we handle it in ExecuteSection
        if (cmd.arguments.size() > 1) {
            return;
        }

        // Register label at current position
        if (currentSection_) {
            AddressType currentAddr = GetCurrentAddress();
            currentSection_->labels[labelName] = currentAddr;
            engine_->Symbols()->RegisterLabel(labelName, currentAddr);
        }
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
        // Reconstruct the assembly line
        std::string asmLine = cmd.name;
        for (const auto& arg : cmd.arguments) {
            asmLine += " " + arg;
        }

        // Get current address
        AddressType currentAddress = GetCurrentAddress();

        // Assemble the instruction
        auto result = engine_->Assembly()->AssembleInstruction(asmLine, currentAddress);

        if (!result) {
            throw EngineException(ErrorCode::AssemblyError,
                "Failed to assemble: " + asmLine);
        }

        // Store assembled code
        if (currentSection_) {
            // Find which allocation/label we're writing to
            std::string targetLabel;
            AddressType targetBase = 0;

            for (const auto& [label, addr] : currentSection_->labels) {
                if (addr <= currentAddress && addr > targetBase) {
                    targetBase = addr;
                    targetLabel = label;
                }
            }

            if (!targetLabel.empty()) {
                currentSection_->codeChunks[targetLabel].insert(
                    currentSection_->codeChunks[targetLabel].end(),
                    result->begin(), result->end()
                );
            }
        }
    }

    void ScriptParser::HandleDataDefinition(const ParsedCommand& cmd) {
        std::vector<uint8_t> data;

        if (cmd.type == CommandType::Db) {
            // Define bytes
            for (const auto& arg : cmd.arguments) {
                // Check if it's a captured value
                if (engine_->Captures()->Exists(arg)) {
                    auto capture = engine_->Captures()->Get(arg);
                    if (capture) {
                        data.insert(data.end(), capture->data.begin(), capture->data.end());
                    }
                }
                else {
                    // Parse as hex byte
                    uint8_t byte = std::stoul(arg, nullptr, 16);
                    data.push_back(byte);
                }
            }
        }
        else if (cmd.type == CommandType::Dd) {
            // Define dword
            for (const auto& arg : cmd.arguments) {
                uint32_t value = 0;
                if (!arg.empty()) {
                    if (arg.find("0x") == 0) {
                        value = std::stoul(arg, nullptr, 16);
                    }
                    else {
                        value = std::stoul(arg, nullptr, 10);
                    }
                }

                data.push_back(value & 0xFF);
                data.push_back((value >> 8) & 0xFF);
                data.push_back((value >> 16) & 0xFF);
                data.push_back((value >> 24) & 0xFF);
            }
        }
        else if (cmd.type == CommandType::Dq) {
            // Define qword
            for (const auto& arg : cmd.arguments) {
                uint64_t value = 0;
                if (!arg.empty()) {
                    if (arg.find("0x") == 0) {
                        value = std::stoull(arg, nullptr, 16);
                    }
                    else {
                        value = std::stoull(arg, nullptr, 10);
                    }
                }

                for (int i = 0; i < 8; i++) {
                    data.push_back((value >> (i * 8)) & 0xFF);
                }
            }
        }

        // Store data
        if (currentSection_ && !data.empty()) {
            // Find current label
            std::string targetLabel;
            for (const auto& [label, addr] : currentSection_->labels) {
                if (addr == GetCurrentAddress()) {
                    targetLabel = label;
                    break;
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

        // Find the current write position
        // This is simplified - in a real implementation you'd track this more carefully
        if (!currentSection_->allocations.empty()) {
            return currentSection_->allocations.begin()->second;
        }

        return 0;
    }

    void ScriptParser::WriteCodeToMemory() {
        if (!currentSection_) {
            return;
        }

        // Write each code chunk to its corresponding address
        for (const auto& [label, code] : currentSection_->codeChunks) {
            if (code.empty()) continue;

            AddressType baseAddr = 0;

            // Find base address for this label
            if (currentSection_->labels.count(label)) {
                baseAddr = currentSection_->labels[label];
            }
            else if (currentSection_->allocations.count(label)) {
                baseAddr = currentSection_->allocations[label];
            }

            if (baseAddr) {
                engine_->Memory()->WriteMemory(baseAddr, code.data(), code.size());
            }
        }
    }

} // namespace AsmEngine