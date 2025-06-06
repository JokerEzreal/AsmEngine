#pragma once

#include "Common.h"

namespace AsmEngine {

    // Represents a captured value from AOB scanning
    struct CapturedValue {
        std::string name;
        ByteVector data;
        AddressType address;
        size_t size;
        std::chrono::system_clock::time_point captureTime;

        // Convert to various types
        uint8_t  AsUInt8() const;
        uint16_t AsUInt16() const;
        uint32_t AsUInt32() const;
        uint64_t AsUInt64() const;
        int8_t   AsInt8() const;
        int16_t  AsInt16() const;
        int32_t  AsInt32() const;
        int64_t  AsInt64() const;
    };

    // Capture definition for pattern parsing
    struct CaptureDefinition {
        std::string name;
        size_t offset;      // Offset in pattern
        size_t size;        // Size in bytes (1, 2, 4, 8)
    };

    class CaptureStorage {
    private:
        mutable std::shared_mutex mutex_;
        std::unordered_map<std::string, CapturedValue> captures_;

    public:
        CaptureStorage() = default;
        ~CaptureStorage() = default;

        // Store a captured value
        void Store(const std::string& name, const ByteVector& data,
            AddressType address, size_t size);

        // Retrieve a captured value
        std::optional<CapturedValue> Get(const std::string& name) const;

        // Check if a capture exists
        bool Exists(const std::string& name) const;

        // Remove a capture
        void Remove(const std::string& name);

        // Clear all captures
        void Clear();

        // Get all capture names
        std::vector<std::string> GetAllNames() const;

        // Resolve a capture reference in assembly code
        std::string ResolveReference(const std::string& name) const;

        // Export captures to a map
        std::unordered_map<std::string, CapturedValue> ExportAll() const;
    };

} // namespace AsmEngine