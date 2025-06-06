#pragma once

#include "Common.h"

namespace AsmEngine {

    // Memory protection flags
    enum class MemoryProtection {
        NoAccess = PAGE_NOACCESS,
        ReadOnly = PAGE_READONLY,
        ReadWrite = PAGE_READWRITE,
        WriteCopy = PAGE_WRITECOPY,
        Execute = PAGE_EXECUTE,
        ExecuteRead = PAGE_EXECUTE_READ,
        ExecuteReadWrite = PAGE_EXECUTE_READWRITE,
        ExecuteWriteCopy = PAGE_EXECUTE_WRITECOPY
    };

    // Allocated memory region info
    struct AllocatedRegion {
        AddressType address;
        size_t size;
        MemoryProtection protection;
        std::string name;
        std::chrono::system_clock::time_point allocationTime;
    };

    class MemoryManager {
    private:
        HANDLE processHandle_;
        DWORD processId_;
        mutable std::mutex mutex_;

        // Track allocated regions
        std::vector<AllocatedRegion> allocatedRegions_;

        // Find suitable address near target (for relative jumps)
        AddressType FindNearAddress(AddressType nearAddress, size_t size) const;

    public:
        MemoryManager(HANDLE processHandle, DWORD processId);
        ~MemoryManager();

        // Basic memory operations
        bool ReadMemory(AddressType address, void* buffer, size_t size) const;
        bool WriteMemory(AddressType address, const void* buffer, size_t size);

        // Template versions for convenience
        template<typename T>
        std::optional<T> Read(AddressType address) const {
            T value;
            if (ReadMemory(address, &value, sizeof(T))) {
                return value;
            }
            return std::nullopt;
        }

        template<typename T>
        bool Write(AddressType address, const T& value) {
            return WriteMemory(address, &value, sizeof(T));
        }

        // Memory allocation
        AddressType AllocateMemory(size_t size,
            MemoryProtection protection = MemoryProtection::ExecuteReadWrite);
        AddressType AllocateNear(AddressType nearAddress, size_t size,
            MemoryProtection protection = MemoryProtection::ExecuteReadWrite);

        // Free memory
        bool FreeMemory(AddressType address);

        // Change memory protection
        bool ProtectMemory(AddressType address, size_t size, MemoryProtection newProtection,
            MemoryProtection* oldProtection = nullptr);

        // Query memory information
        struct MemoryInfo {
            AddressType baseAddress;
            AddressType allocationBase;
            size_t regionSize;
            MemoryProtection protection;
            bool isCommitted;
            bool isFree;
        };

        std::optional<MemoryInfo> QueryMemory(AddressType address) const;

        // Pattern operations
        std::vector<AddressType> FindPattern(const ByteVector& pattern,
            const ByteVector& mask,
            AddressType startAddress = 0,
            AddressType endAddress = 0) const;

        // Get process information
        std::vector<std::pair<AddressType, size_t>> GetMemoryRegions() const;

        // Cleanup all allocated memory
        void CleanupAllocations();

        // Get allocation info
        std::optional<AllocatedRegion> GetAllocationInfo(AddressType address) const;
        std::vector<AllocatedRegion> GetAllAllocations() const;
    };

} // namespace AsmEngine