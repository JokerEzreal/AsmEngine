#include "ScriptParser.h"
#include "AsmEngine.h"
#include <fstream>
#include <regex>
#include <algorithm>

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
        : engine_(engine), currentSection_(nullptr) {
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
        std::string current;
        bool inQuotes = false;
        bool inParentheses = false;
        int parenDepth = 0;

        for (size_t i = 0; i < line.length(); ++i) {
            char c = line[i];

            if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
                inQuotes = !inQuotes;
                current += c;
            }
            else if (!inQuotes) {
                if (c == '(') {
                    inParentheses = true;
                    parenDepth++;
                    current += c;
                }
                else if (c == ')') {
                    parenDepth--;
                    if (parenDepth == 0) {
                        inParentheses = false;
                    }
                    current += c;
                }
                else if ((c == ' ' || c == '\t' || c == ',') && !inParentheses) {
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
        for (auto& section : sections_) {
            if (section.isEnabled) {
                ExecuteSection(section.name);
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

        // Create context for this execution
        ScriptContext context;

        // Execute each command in the section
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

                case CommandType::Dealloc:
                    HandleDealloc(cmd);
                    break;

                case CommandType::Label:
                    HandleLabel(cmd);
                    break;

                case CommandType::RegisterSymbol:
                    HandleRegisterSymbol(cmd);
                    break;

                case CommandType::UnregisterSymbol:
                    HandleUnregisterSymbol(cmd);
                    break;

                case CommandType::Define:
                    HandleDefine(cmd);
                    break;

                case CommandType::Include:
                    HandleInclude(cmd);
                    break;

                case CommandType::Asm:
                    HandleAsm(cmd);
                    break;

                default:
                    // Handle as assembly
                    HandleAsm(cmd);
                    break;
                }
            }
            catch (const std::exception& e) {
                ReportError(e.what(), cmd.lineNumber);
            }
        }
    }

    void ScriptParser::HandleAobscan(const ParsedCommand& cmd) {
        if (cmd.arguments.size() < 2) {
            throw EngineException(ErrorCode::InvalidParameter,
                "aobscan requires name and pattern");
        }

        std::string name = cmd.arguments[0];
        std::string pattern = cmd.arguments[1];

        // Remove parentheses if present
        if (name.front() == '(' && name.back() == ')') {
            name = name.substr(1, name.length() - 2);
        }

        // Scan all modules
        auto results = engine_->Scanner()->ScanAll(pattern);
        if (!results.empty()) {
            // Register the first result as a symbol
            engine_->Symbols()->RegisterSymbol(name, results[0].address);
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

        // Remove parentheses if present
        if (name.front() == '(' && name.back() == ')') {
            name = name.substr(1, name.length() - 2);
        }

        // Scan specific module
        auto result = engine_->Scanner()->ScanModule(module, pattern);
        if (result) {
            // Register as a symbol
            engine_->Symbols()->RegisterSymbol(name, result->address);
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
            allocated = engine_->Memory()->AllocateNear(nearAddress, size);
        }
        else {
            allocated = engine_->Memory()->AllocateMemory(size);
        }

        if (allocated) {
            // Register as symbol
            engine_->Symbols()->RegisterAllocation(name, allocated, size);
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

        // Labels are handled during assembly
        // Just register as placeholder for now
        engine_->Symbols()->RegisterLabel(cmd.arguments[0], 0);
    }

    void ScriptParser::HandleRegisterSymbol(const ParsedCommand& cmd) {
        if (cmd.arguments.empty()) {
            throw EngineException(ErrorCode::InvalidParameter,
                "registersymbol requires name");
        }

        // Symbol should already exist from previous operations
        // This just marks it for persistence/export
        std::string name = cmd.arguments[0];

        if (!engine_->Symbols()->Exists(name)) {
            throw EngineException(ErrorCode::SymbolNotFound,
                "Symbol not found: " + name);
        }
    }

    void ScriptParser::HandleUnregisterSymbol(const ParsedCommand& cmd) {
        if (cmd.arguments.empty()) {
            throw EngineException(ErrorCode::InvalidParameter,
                "unregistersymbol requires name");
        }

        engine_->Symbols()->UnregisterSymbol(cmd.arguments[0]);
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

        // Get current address (would need context)
        AddressType currentAddress = 0;

        // If we have a current allocation, use that
        auto symbols = engine_->Symbols()->GetLocalSymbols();
        if (!symbols.empty()) {
            currentAddress = symbols.back().address;
        }

        // Assemble the instruction
        auto result = engine_->Assembly()->AssembleInstruction(asmLine, currentAddress);

        if (!result) {
            throw EngineException(ErrorCode::AssemblyError,
                "Failed to assemble: " + asmLine);
        }

        // Write to memory if we have an address
        if (currentAddress) {
            engine_->Memory()->WriteMemory(currentAddress, result->data(), result->size());
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

} // namespace AsmEngine