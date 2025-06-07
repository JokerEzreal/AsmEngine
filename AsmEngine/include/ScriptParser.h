#pragma once

#include "Common.h"
#include <map>
#include <set>
#include <string>
#include <vector>
#include <optional>
#include <functional>

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

    struct AssemblyBlock {
        std::string code;
        std::set<std::string> localLabels;
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

        // Track memory allocations for this section
        std::map<std::string, AddressType> allocations;

        // Track assembled code
        std::map<std::string, std::vector<uint8_t>> codeChunks;

        // Track label positions
        std::map<std::string, AddressType> labels;
    };

    class ScriptParser {
    private:
        AsmEngine* engine_;

        // Current script state
        std::unordered_map<std::string, std::string> defines_;
        std::vector<ScriptSection> sections_;
        ScriptSection* currentSection_;

        // For tracking anonymous labels
        std::map<std::string, std::vector<size_t>> forwardJumps_;
        std::map<std::string, AddressType> labelAddresses_;
        int anonymousLabelCounter_;

        // Parse helpers
        ParsedCommand ParseLine(const std::string& line, size_t lineNumber) const;
        CommandType IdentifyCommand(const std::string& command) const;
        std::vector<std::string> TokenizeLine(const std::string& line) const;
        std::string ExpandDefines(const std::string& line) const;

        // New helpers for CE script support
        std::string PreprocessLine(const std::string& line) const;
        bool IsLabel(const std::string& line) const;
        std::pair<std::string, size_t> ParseLabelWithOffset(const std::string& line) const;
        std::string ConvertFloatValue(const std::string& value) const;
        std::string ProcessAnonymousLabels(const std::string& line);
        std::vector<uint8_t> ParseDataBytes(const std::vector<std::string>& args) const;

        // General number parsing function
        uint64_t ParseNumber(const std::string& str) const;

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
        void HandleDataDefinition(const ParsedCommand& cmd);

        // Section handlers
        void BeginSection(const std::string& name);
        void EndSection();

        // Assembly helpers
        AddressType GetCurrentAddress() const;
        void WriteCodeToMemory();

        void PreResolveLabels();
        void ResolveForwardReferences();

        // Script execution helpers
        void ProcessAllocationsAndScans();
        void BuildAssemblyBlocks(std::map<std::string, AssemblyBlock>& blocks);
        std::string BuildAssemblyLine(const ParsedCommand& cmd);
        std::string BuildDataDirective(const ParsedCommand& cmd);
        AddressType ResolveBaseAddress(const std::string& label);
        void ProcessCleanup();

        // Helper method to fix memory offset formatting
        std::string FixMemoryOffsets(const std::string& line) const;

        // Error callback
        using ErrorCallback = std::function<void(const std::string&, size_t)>;
        ErrorCallback errorCallback_;

        void ReportError(const std::string& message, size_t lineNumber);

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
        void SetErrorCallback(ErrorCallback callback);
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