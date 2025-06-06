#pragma once

// ∑¿÷π Windows.h ∂®“Â min/max ∫Í
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <functional>

// AVX2 intrinsics
#include <immintrin.h>

// Keystone assembler
#include <keystone/keystone.h>

namespace AsmEngine {

    // Forward declarations
    class AOBScanner;
    class SymbolManager;
    class MemoryManager;
    class ScriptParser;
    class AssemblyEngine;
    class CaptureStorage;

    // Common types
    using ByteVector = std::vector<uint8_t>;
    using AddressType = uintptr_t;

    // Error handling
    enum class ErrorCode {
        Success = 0,
        InvalidPattern,
        PatternNotFound,
        MemoryAccessError,
        AssemblyError,
        SymbolNotFound,
        AllocationError,
        ProcessNotFound,
        InvalidParameter
    };

    class EngineException : public std::exception {
    private:
        ErrorCode code_;
        std::string message_;

    public:
        EngineException(ErrorCode code, const std::string& message)
            : code_(code), message_(message) {
        }

        const char* what() const noexcept override {
            return message_.c_str();
        }

        ErrorCode code() const { return code_; }
    };

    // Utility functions
    inline std::string BytesToString(const ByteVector& bytes) {
        std::stringstream ss;
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (i > 0) ss << " ";
            ss << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(bytes[i]);
        }
        return ss.str();
    }

    inline ByteVector StringToBytes(const std::string& str) {
        ByteVector bytes;
        std::stringstream ss(str);
        std::string byte;

        while (ss >> byte) {
            if (byte == "?" || byte == "??") {
                bytes.push_back(0);
            }
            else {
                bytes.push_back(static_cast<uint8_t>(std::stoi(byte, nullptr, 16)));
            }
        }

        return bytes;
    }

    // Enable debug privileges for process access
    inline bool EnableDebugPrivilege() {
        HANDLE hToken;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
            return false;

        LUID luid;
        if (!LookupPrivilegeValue(nullptr, SE_DEBUG_NAME, &luid)) {
            CloseHandle(hToken);
            return false;
        }

        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        bool result = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        CloseHandle(hToken);

        return result && GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    }

} // namespace AsmEngine