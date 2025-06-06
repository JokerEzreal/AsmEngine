#pragma once

// Windows headers
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

// Standard library
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <optional>
#include <unordered_map>
#include <functional>
#include <exception>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <mutex>
#include <shared_mutex>

// SIMD support
#include <immintrin.h>

namespace AsmEngine {

    // Basic types
    using AddressType = std::uintptr_t;
    using ByteVector = std::vector<uint8_t>;

    // Error codes
    enum class ErrorCode {
        Success = 0,
        InvalidParameter,
        ProcessNotFound,
        MemoryAccessError,
        PatternNotFound,
        InvalidPattern,
        AssemblyError,
        DisassemblyError,
        SymbolNotFound,
        AllocationError,
        Unknown
    };

    // Exception class
    class EngineException : public std::exception {
    private:
        ErrorCode code_;
        std::string message_;

    public:
        EngineException(ErrorCode code, const std::string& message)
            : code_(code), message_(message) {
        }

        ErrorCode code() const { return code_; }
        const char* what() const noexcept override { return message_.c_str(); }
    };

    // Utility functions
    inline std::string BytesToString(const ByteVector& bytes) {
        std::stringstream ss;
        for (uint8_t byte : bytes) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        return ss.str();
    }

    inline ByteVector StringToBytes(const std::string& str) {
        ByteVector bytes;
        for (size_t i = 0; i < str.length(); i += 2) {
            std::string byteString = str.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, nullptr, 16));
            bytes.push_back(byte);
        }
        return bytes;
    }

    // Enable debug privileges
    inline bool EnableDebugPrivilege() {
        HANDLE hToken;
        TOKEN_PRIVILEGES tkp;

        if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            return false;
        }

        LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &tkp.Privileges[0].Luid);
        tkp.PrivilegeCount = 1;
        tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, NULL, NULL);
        CloseHandle(hToken);

        return GetLastError() == ERROR_SUCCESS;
    }

} // namespace AsmEngine