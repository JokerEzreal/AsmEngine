#include "MemoryManager.h"

namespace AsmEngine {

    MemoryManager::MemoryManager(HANDLE processHandle, DWORD processId)
        : processHandle_(processHandle), processId_(processId) {
    }

    MemoryManager::~MemoryManager() {
        CleanupAllocations();
    }

    bool MemoryManager::ReadMemory(AddressType address, void* buffer, size_t size) const {
        SIZE_T bytesRead;
        return ReadProcessMemory(processHandle_,
            reinterpret_cast<LPCVOID>(address),
            buffer, size, &bytesRead) && bytesRead == size;
    }

    bool MemoryManager::WriteMemory(AddressType address, const void* buffer, size_t size) {
        SIZE_T bytesWritten;
        return WriteProcessMemory(processHandle_,
            reinterpret_cast<LPVOID>(address),
            buffer, size, &bytesWritten) && bytesWritten == size;
    }

    AddressType MemoryManager::AllocateMemory(size_t size, MemoryProtection protection) {
        std::lock_guard lock(mutex_);

        LPVOID allocatedAddress = VirtualAllocEx(processHandle_, nullptr, size,
            MEM_COMMIT | MEM_RESERVE,
            static_cast<DWORD>(protection));

        if (allocatedAddress) {
            AllocatedRegion region;
            region.address = reinterpret_cast<AddressType>(allocatedAddress);
            region.size = size;
            region.protection = protection;
            region.allocationTime = std::chrono::system_clock::now();

            allocatedRegions_.push_back(region);
        }

        return reinterpret_cast<AddressType>(allocatedAddress);
    }

    AddressType MemoryManager::FindNearAddress(AddressType nearAddress, size_t size) const {
        // Try to find free memory within +/- 2GB for x64 relative jumps
        const size_t maxDistance = 0x7FFFFFFF; // 2GB - 1
        const size_t pageSize = 0x1000; // 4KB

        MEMORY_BASIC_INFORMATION mbi;

        // Search forward
        for (AddressType addr = nearAddress;
            addr < nearAddress + maxDistance && addr > nearAddress;
            addr = reinterpret_cast<AddressType>(mbi.BaseAddress) + mbi.RegionSize) {

            if (!VirtualQueryEx(processHandle_, reinterpret_cast<LPCVOID>(addr),
                &mbi, sizeof(mbi))) {
                break;
            }

            if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
                // Align to page boundary
                AddressType aligned = (addr + pageSize - 1) & ~(pageSize - 1);
                if (aligned + size <= reinterpret_cast<AddressType>(mbi.BaseAddress) + mbi.RegionSize) {
                    return aligned;
                }
            }
        }

        // Search backward
        for (AddressType addr = nearAddress - pageSize;
            addr > nearAddress - maxDistance && addr < nearAddress;
            addr -= pageSize) {

            if (!VirtualQueryEx(processHandle_, reinterpret_cast<LPCVOID>(addr),
                &mbi, sizeof(mbi))) {
                continue;
            }

            if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
                return reinterpret_cast<AddressType>(mbi.BaseAddress);
            }

            // Move to start of this region
            addr = reinterpret_cast<AddressType>(mbi.BaseAddress);
        }

        return 0;
    }

    AddressType MemoryManager::AllocateNear(AddressType nearAddress, size_t size,
        MemoryProtection protection) {
        std::lock_guard lock(mutex_);

        // Try to find suitable address near target
        AddressType targetAddress = FindNearAddress(nearAddress, size);
        if (!targetAddress) {
            // If no suitable address found near target, try regular allocation
            return AllocateMemory(size, protection);
        }

        // Try to allocate at the specific address
        LPVOID allocatedAddress = VirtualAllocEx(processHandle_,
            reinterpret_cast<LPVOID>(targetAddress),
            size, MEM_COMMIT | MEM_RESERVE,
            static_cast<DWORD>(protection));

        if (!allocatedAddress) {
            // If specific address fails, let Windows choose within range
            // This is done by trying multiple addresses
            const size_t maxAttempts = 100;
            const size_t stepSize = 0x10000; // 64KB steps

            // Try addresses above target
            for (size_t i = 0; i < maxAttempts / 2; i++) {
                targetAddress = nearAddress + (i * stepSize);
                allocatedAddress = VirtualAllocEx(processHandle_,
                    reinterpret_cast<LPVOID>(targetAddress),
                    size, MEM_COMMIT | MEM_RESERVE,
                    static_cast<DWORD>(protection));

                if (allocatedAddress) {
                    // Verify it's within acceptable range
                    AddressType allocAddr = reinterpret_cast<AddressType>(allocatedAddress);
                    int64_t distance = static_cast<int64_t>(allocAddr) - static_cast<int64_t>(nearAddress);
                    if (std::abs(distance) <= 0x7FFFFFFF) {
                        break;
                    }
                    else {
                        // Too far, free and continue
                        VirtualFreeEx(processHandle_, allocatedAddress, 0, MEM_RELEASE);
                        allocatedAddress = nullptr;
                    }
                }
            }

            // Try addresses below target if still not found
            if (!allocatedAddress) {
                for (size_t i = 1; i <= maxAttempts / 2; i++) {
                    if (nearAddress < i * stepSize) break;
                    targetAddress = nearAddress - (i * stepSize);

                    allocatedAddress = VirtualAllocEx(processHandle_,
                        reinterpret_cast<LPVOID>(targetAddress),
                        size, MEM_COMMIT | MEM_RESERVE,
                        static_cast<DWORD>(protection));

                    if (allocatedAddress) {
                        AddressType allocAddr = reinterpret_cast<AddressType>(allocatedAddress);
                        int64_t distance = static_cast<int64_t>(allocAddr) - static_cast<int64_t>(nearAddress);
                        if (std::abs(distance) <= 0x7FFFFFFF) {
                            break;
                        }
                        else {
                            VirtualFreeEx(processHandle_, allocatedAddress, 0, MEM_RELEASE);
                            allocatedAddress = nullptr;
                        }
                    }
                }
            }
        }

        if (allocatedAddress) {
            AllocatedRegion region;
            region.address = reinterpret_cast<AddressType>(allocatedAddress);
            region.size = size;
            region.protection = protection;
            region.allocationTime = std::chrono::system_clock::now();

            allocatedRegions_.push_back(region);
        }

        return reinterpret_cast<AddressType>(allocatedAddress);
    }

    bool MemoryManager::FreeMemory(AddressType address) {
        std::lock_guard lock(mutex_);

        bool result = VirtualFreeEx(processHandle_,
            reinterpret_cast<LPVOID>(address),
            0, MEM_RELEASE) != 0;

        if (result) {
            // Remove from tracked allocations
            allocatedRegions_.erase(
                std::remove_if(allocatedRegions_.begin(), allocatedRegions_.end(),
                    [address](const AllocatedRegion& region) {
                        return region.address == address;
                    }),
                allocatedRegions_.end()
            );
        }

        return result;
    }

    bool MemoryManager::ProtectMemory(AddressType address, size_t size,
        MemoryProtection newProtection,
        MemoryProtection* oldProtection) {
        DWORD oldProtect;
        bool result = VirtualProtectEx(processHandle_,
            reinterpret_cast<LPVOID>(address),
            size, static_cast<DWORD>(newProtection),
            &oldProtect) != 0;

        if (result && oldProtection) {
            *oldProtection = static_cast<MemoryProtection>(oldProtect);
        }

        return result;
    }

    std::optional<MemoryManager::MemoryInfo> MemoryManager::QueryMemory(
        AddressType address) const {

        MEMORY_BASIC_INFORMATION mbi;

        if (VirtualQueryEx(processHandle_, reinterpret_cast<LPCVOID>(address),
            &mbi, sizeof(mbi))) {
            MemoryInfo info;
            info.baseAddress = reinterpret_cast<AddressType>(mbi.BaseAddress);
            info.allocationBase = reinterpret_cast<AddressType>(mbi.AllocationBase);
            info.regionSize = mbi.RegionSize;
            info.protection = static_cast<MemoryProtection>(mbi.Protect);
            info.isCommitted = (mbi.State == MEM_COMMIT);
            info.isFree = (mbi.State == MEM_FREE);

            return info;
        }

        return std::nullopt;
    }

    std::vector<AddressType> MemoryManager::FindPattern(const ByteVector& pattern,
        const ByteVector& mask,
        AddressType startAddress,
        AddressType endAddress) const {
        std::vector<AddressType> results;

        if (pattern.empty() || pattern.size() != mask.size()) {
            return results;
        }

        // Get memory regions if range not specified
        if (startAddress == 0 && endAddress == 0) {
            auto regions = GetMemoryRegions();
            if (!regions.empty()) {
                startAddress = regions.front().first;
                endAddress = regions.back().first + regions.back().second;
            }
        }

        const size_t bufferSize = 64 * 1024; // 64KB
        std::vector<uint8_t> buffer(bufferSize);

        for (AddressType currentAddress = startAddress;
            currentAddress < endAddress;
            currentAddress += bufferSize) {

            SIZE_T bytesRead;
            if (!ReadProcessMemory(processHandle_,
                reinterpret_cast<LPCVOID>(currentAddress),
                buffer.data(), bufferSize, &bytesRead)) {
                continue;
            }

            // Search in buffer
            for (size_t i = 0; i <= bytesRead - pattern.size(); ++i) {
                bool match = true;

                for (size_t j = 0; j < pattern.size(); ++j) {
                    if (mask[j] && buffer[i + j] != pattern[j]) {
                        match = false;
                        break;
                    }
                }

                if (match) {
                    results.push_back(currentAddress + i);
                }
            }
        }

        return results;
    }

    std::vector<std::pair<AddressType, size_t>> MemoryManager::GetMemoryRegions() const {
        std::vector<std::pair<AddressType, size_t>> regions;

        MEMORY_BASIC_INFORMATION mbi;
        AddressType address = 0;

        while (VirtualQueryEx(processHandle_, reinterpret_cast<LPCVOID>(address),
            &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                    PAGE_READWRITE | PAGE_READONLY))) {
                regions.emplace_back(
                    reinterpret_cast<AddressType>(mbi.BaseAddress),
                    mbi.RegionSize
                );
            }

            address = reinterpret_cast<AddressType>(mbi.BaseAddress) + mbi.RegionSize;
        }

        return regions;
    }

    void MemoryManager::CleanupAllocations() {
        std::lock_guard lock(mutex_);

        for (const auto& region : allocatedRegions_) {
            VirtualFreeEx(processHandle_,
                reinterpret_cast<LPVOID>(region.address),
                0, MEM_RELEASE);
        }

        allocatedRegions_.clear();
    }

    std::optional<AllocatedRegion> MemoryManager::GetAllocationInfo(
        AddressType address) const {

        std::lock_guard lock(mutex_);

        for (const auto& region : allocatedRegions_) {
            if (address >= region.address &&
                address < region.address + region.size) {
                return region;
            }
        }

        return std::nullopt;
    }

    std::vector<AllocatedRegion> MemoryManager::GetAllAllocations() const {
        std::lock_guard lock(mutex_);
        return allocatedRegions_;
    }

} // namespace AsmEngine