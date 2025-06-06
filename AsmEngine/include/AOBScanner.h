#pragma once

#include "Common.h"
#include "CaptureStorage.h"

namespace AsmEngine {

    // Pattern element types
    enum class PatternElementType {
        Byte,       // Exact byte match
        Wildcard,   // ? or ??
        Capture     // Capture group (e.g., s1.2)
    };

    // Represents a single element in the pattern
    struct PatternElement {
        PatternElementType type;
        uint8_t value;              // For Byte type
        CaptureDefinition capture;  // For Capture type
    };

    // Parsed pattern ready for scanning
    class ParsedPattern {
    private:
        std::vector<PatternElement> elements_;
        std::vector<CaptureDefinition> captures_;
        size_t patternLength_;

    public:
        ParsedPattern() : patternLength_(0) {}

        void AddByte(uint8_t value);
        void AddWildcard();
        void AddCapture(const std::string& name, size_t size);

        const std::vector<PatternElement>& GetElements() const { return elements_; }
        const std::vector<CaptureDefinition>& GetCaptures() const { return captures_; }
        size_t GetPatternLength() const { return patternLength_; }
    };

    // Scan result with captured values
    struct ScanResult {
        AddressType address;
        std::unordered_map<std::string, ByteVector> captures;
    };

    class AOBScanner {
    private:
        HANDLE processHandle_;
        CaptureStorage* captureStorage_;

        // Parse pattern string into ParsedPattern
        ParsedPattern ParsePattern(const std::string& pattern) const;

        // Perform SIMD-accelerated comparison
        bool CompareMemorySIMD(const uint8_t* memory, const ParsedPattern& pattern,
            std::unordered_map<std::string, ByteVector>& captures) const;

        // Standard byte-by-byte comparison (fallback)
        bool CompareMemory(const uint8_t* memory, const ParsedPattern& pattern,
            std::unordered_map<std::string, ByteVector>& captures) const;

        // Extract captures from matched memory
        void ExtractCaptures(const uint8_t* memory, const ParsedPattern& pattern,
            std::unordered_map<std::string, ByteVector>& captures) const;

    public:
        AOBScanner(HANDLE processHandle, CaptureStorage* storage);
        ~AOBScanner() = default;

        // Scan for pattern in specific module
        std::optional<ScanResult> ScanModule(const std::string& moduleName,
            const std::string& pattern);

        // Scan for pattern in address range
        std::optional<ScanResult> ScanRange(AddressType startAddress,
            AddressType endAddress,
            const std::string& pattern);

        // Scan all modules for pattern
        std::vector<ScanResult> ScanAll(const std::string& pattern);

        // Get module information
        struct ModuleInfo {
            std::string name;
            AddressType baseAddress;
            size_t size;
        };

        std::optional<ModuleInfo> GetModuleInfo(const std::string& moduleName) const;
        std::vector<ModuleInfo> GetAllModules() const;
    };

} // namespace AsmEngine