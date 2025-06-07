#pragma once

#include "Common.h"
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include <string>

namespace AsmEngine {

    // Forward declaration
    class MemoryManager;
    class AssemblyEngine;

    // Hook entry information
    struct HookEntry {
        AddressType targetAddress;      // Original function address
        AddressType trampolineAddress;  // Trampoline allocation
        AddressType hookFunction;       // Hook function address
        ByteVector originalBytes;       // Original bytes backed up
        size_t preserveSize;           // Size of preserved instructions
        std::string description;       // Optional description
    };

    // Hook information for external use
    struct HookInfo {
        AddressType targetAddress;
        AddressType trampolineAddress;
        AddressType hookFunction;
        bool isActive;
        std::string description;
    };

    class TrampolineManager {
    private:
        MemoryManager* memoryManager_;
        AssemblyEngine* assemblyEngine_;
        mutable std::mutex mutex_;

        // Minimum size for trampoline
        static constexpr size_t kMinTrampolineSize = 5;  // Minimum for JMP

        // Active hooks
        std::map<AddressType, HookEntry> hooks_;

        // Trampoline pools for efficient allocation
        struct TrampolinePool {
            AddressType baseAddress;
            size_t totalSize;
            size_t usedSize;
            std::vector<std::pair<size_t, size_t>> freeBlocks; // offset, size
        };

        std::vector<TrampolinePool> pools_;

        // Calculate how many bytes to preserve from the original function
        size_t CalculatePreserveSize(AddressType address) const;

        // Allocate space for a trampoline
        AddressType AllocateTrampoline(size_t size, AddressType nearAddress);

        // Free a trampoline allocation
        void FreeTrampoline(AddressType address, size_t size);

        // Build the trampoline code
        ByteVector BuildTrampoline(AddressType originalAddress,
            const ByteVector& preservedBytes,
            AddressType returnAddress);

        // Find hook by address
        std::optional<HookEntry> FindHook(AddressType address) const;

        // Disassemble instructions to find safe hook point
        size_t FindSafeHookPoint(AddressType address, size_t minSize) const;

        // Relocate instructions for the trampoline
        ByteVector RelocateInstructions(AddressType from, AddressType to,
            const ByteVector& instructions) const;

    public:
        TrampolineManager(MemoryManager* memoryManager, AssemblyEngine* assemblyEngine);
        ~TrampolineManager();

        // Install a hook with automatic trampoline creation
        bool InstallHook(AddressType targetAddress, AddressType hookFunction,
            const std::string& description = "");

        // Remove a hook
        bool RemoveHook(AddressType targetAddress);

        // Remove all hooks
        void RemoveAllHooks();

        // Check if address is hooked
        bool IsHooked(AddressType address) const;

        // Get hook information
        std::optional<HookInfo> GetHookInfo(AddressType address) const;

        // Get all active hooks
        std::vector<HookInfo> GetAllHooks() const;

        // Create a detour (hook with custom code)
        bool CreateDetour(AddressType targetAddress, const std::string& detourCode,
            AddressType& trampolineOut);

        // Enable/disable hook temporarily
        bool EnableHook(AddressType address);
        bool DisableHook(AddressType address);

        // Get trampoline address for a hooked function
        std::optional<AddressType> GetTrampoline(AddressType hookedAddress) const;

        // Pool management
        void CreatePool(AddressType nearAddress, size_t poolSize);
        void CleanupPools();
    };

} // namespace AsmEngine