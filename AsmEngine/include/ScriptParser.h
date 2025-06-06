#pragma once

#include "Common.h"

namespace AsmEngine {

    // Forward declarations
    class AsmEngine;

    // Script command types
    enum class CommandType {
        // Memory operations
        Aobscan,
        Aobscanmodule,
        Alloc,
        Dealloc,

        // Symbol operations
        Label,
        RegisterSymbol,
        UnregisterSymbol,

        // Assembly operations
        Asm,
        Code,

        // Control flow
        Enable,
        Disable,

        // Data definition
        Db,     // Define byte
        Dw,     // Define word
        Dd,     // Define dword
        Dq,     // Define qword

        // Comments and misc
        Comment,
        Include,
        Define
    };

    // Parsed command
    struct ParsedCommand {
        CommandType type;
        std::string name;
        std::vector<std::string> arguments;
        size_t lineNumber;
    };

    // Script section
    struct ScriptSection {
        std::string name;
        std::vector<ParsedCommand> commands;
        bool isEnabled;
    };

    class ScriptParser {
    private:
        AsmEngine* engine_;

        // Current script state
        std::unordered_map<std::string, std::string> defines_;
        std::vector<ScriptSection> sections_;
        ScriptSection* currentSection_;

        // Parse helpers
        ParsedCommand ParseLine(const std::string& line, size_t lineNumber) const;
        CommandType IdentifyCommand(const std::string& command) const;
        std::vector<std::string> TokenizeLine(const std::string& line) const;
        std::string ExpandDefines(const std::string& line) const;

        // Command handlers
        void HandleAobscan(const ParsedCommand& cmd);
        void HandleAobscanmodule(const ParsedCommand& cmd);
        void HandleAlloc(const ParsedCommand& cmd);
        void HandleDealloc(const ParsedCommand& cmd);
        void HandleLabel(const ParsedCommand& cmd);
        void HandleRegisterSymbol(const ParsedCommand& cmd);
        void HandleUnregisterSymbol(const ParsedCommand& cmd);
        void HandleAsm(const ParsedCommand& cmd);
        void HandleDefine(const ParsedCommand& cmd);
        void HandleInclude(const ParsedCommand& cmd);

        // Section handlers
        void BeginSection(const std::string& name);
        void EndSection();

    public:
        ScriptParser(AsmEngine* engine);
        ~ScriptParser() = default;

        // Parse script from string
        void Parse(const std::string& script);

        // Parse script from file
        void ParseFile(const std::string& filename);

        // Execute parsed script
        void Execute();

        // Execute specific section
        void ExecuteSection(const std::string& sectionName);

        // Enable/disable sections
        void EnableSection(const std::string& sectionName);
        void DisableSection(const std::string& sectionName);

        // Get parsed sections
        std::vector<std::string> GetSectionNames() const;
        std::optional<ScriptSection> GetSection(const std::string& name) const;

        // Clear parser state
        void Clear();

        // Error handling
        using ErrorCallback = std::function<void(const std::string&, size_t)>;
        void SetErrorCallback(ErrorCallback callback);

    private:
        ErrorCallback errorCallback_;

        void ReportError(const std::string& message, size_t lineNumber);
    };

    // Script execution context
    class ScriptContext {
    private:
        std::unordered_map<std::string, std::string> variables_;
        std::vector<AddressType> allocations_;
        std::vector<std::pair<AddressType, ByteVector>> patches_;

    public:
        ScriptContext() = default;
        ~ScriptContext() = default;

        // Variable management
        void SetVariable(const std::string& name, const std::string& value);
        std::optional<std::string> GetVariable(const std::string& name) const;

        // Track allocations
        void AddAllocation(AddressType address);

        // Track patches
        void AddPatch(AddressType address, const ByteVector& originalBytes);

        // Cleanup
        void Cleanup();
    };

} // namespace AsmEngine