#include "TrampolineManager.h"
#include "MemoryManager.h"
#include <algorithm>
#include <cstring>

namespace AsmEngine {

    TrampolineManager::TrampolineManager(MemoryManager* memoryManager, AssemblyEngine* assemblyEngine)
        : memoryManager_(memoryManager), assemblyEngine_(assemblyEngine) {
    }

    TrampolineManager::~TrampolineManager() {
        RemoveAllHooks();
        CleanupPools();
    }

    size_t TrampolineManager::CalculatePreserveSize(AddressType address) const {
        // We need to preserve at least enough bytes for a JMP instruction (5 bytes)
        // But we must preserve complete instructions

        // For now, use a simple heuristic - preserve at least 5 bytes
        // In a real implementation, this would disassemble instructions
        // to find instruction boundaries

        size_t preserveSize = 5;

        // TODO: Use a proper disassembler to find instruction boundaries
        // For now, we'll try common instruction sizes

        ByteVector testBytes(16);
        if (memoryManager_->ReadMemory(address, testBytes.data(), testBytes.size())) {
            // Simple heuristic: look for common x64 instruction patterns
            // This is not comprehensive but handles common cases

            size_t currentSize = 0;
            size_t i = 0;

            while (currentSize < 5 && i < testBytes.size()) {
                uint8_t opcode = testBytes[i];

                // REX prefix
                if ((opcode & 0xF0) == 0x40) {
                    i++;
                    if (i >= testBytes.size()) break;
                    opcode = testBytes[i];
                }

                // Common instruction sizes (simplified)
                if (opcode == 0xE8 || opcode == 0xE9) {
                    // CALL/JMP rel32
                    currentSize = i + 5;
                }
                else if (opcode == 0xFF) {
                    // Various instructions with ModR/M
                    if (i + 1 < testBytes.size()) {
                        uint8_t modRM = testBytes[i + 1];
                        if ((modRM & 0xC0) == 0xC0) {
                            // Register direct
                            currentSize = i + 2;
                        }
                        else {
                            // Memory operand - simplified
                            currentSize = i + 6;
                        }
                    }
                }
                else if ((opcode & 0xF0) == 0x50) {
                    // PUSH/POP reg
                    currentSize = i + 1;
                }
                else if (opcode == 0x48 || opcode == 0x49 || opcode == 0x4C || opcode == 0x4D) {
                    // REX.W prefix - next instruction
                    i++;
                    continue;
                }
                else {
                    // Default: assume 3-byte instruction
                    currentSize = i + 3;
                }

                if (currentSize == 0) {
                    i++;
                }
            }

            preserveSize = max(preserveSize, currentSize);
        }

        return preserveSize;
    }

    AddressType TrampolineManager::AllocateTrampoline(size_t size, AddressType nearAddress) {
        std::lock_guard<std::mutex> lock(mutex_);

        // First, try to allocate from existing pools
        for (auto& pool : pools_) {
            // Check if pool is close enough (within 2GB for x64 relative jumps)
            int64_t distance = std::abs(static_cast<int64_t>(pool.baseAddress) -
                static_cast<int64_t>(nearAddress));

            if (distance > 0x7FFFFFFF) {
                continue; // Pool is too far
            }

            // Find a free block in this pool
            for (auto it = pool.freeBlocks.begin(); it != pool.freeBlocks.end(); ++it) {
                if (it->second >= size) {
                    // Found suitable block
                    AddressType result = pool.baseAddress + it->first;

                    // Update free block list
                    if (it->second == size) {
                        // Exact fit - remove block
                        pool.freeBlocks.erase(it);
                    }
                    else {
                        // Split block
                        it->first += size;
                        it->second -= size;
                    }

                    pool.usedSize += size;
                    return result;
                }
            }
        }

        // No suitable space in existing pools - allocate new pool
        const size_t poolSize = 64 * 1024; // 64KB pools
        AddressType poolBase = memoryManager_->AllocateNear(nearAddress, poolSize);

        if (poolBase == 0) {
            return 0; // Allocation failed
        }

        // Create new pool
        TrampolinePool newPool;
        newPool.baseAddress = poolBase;
        newPool.totalSize = poolSize;
        newPool.usedSize = size;
        newPool.freeBlocks.push_back({ size, poolSize - size });

        pools_.push_back(newPool);

        return poolBase;
    }

    void TrampolineManager::FreeTrampoline(AddressType address, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Find which pool this address belongs to
        for (auto& pool : pools_) {
            if (address >= pool.baseAddress &&
                address < pool.baseAddress + pool.totalSize) {

                // Calculate offset within pool
                size_t offset = address - pool.baseAddress;

                // Add to free blocks list
                pool.freeBlocks.push_back({ offset, size });
                pool.usedSize -= size;

                // Merge adjacent free blocks
                std::sort(pool.freeBlocks.begin(), pool.freeBlocks.end());

                for (size_t i = 0; i < pool.freeBlocks.size() - 1; ) {
                    if (pool.freeBlocks[i].first + pool.freeBlocks[i].second ==
                        pool.freeBlocks[i + 1].first) {
                        // Merge blocks
                        pool.freeBlocks[i].second += pool.freeBlocks[i + 1].second;
                        pool.freeBlocks.erase(pool.freeBlocks.begin() + i + 1);
                    }
                    else {
                        i++;
                    }
                }

                break;
            }
        }
    }

    ByteVector TrampolineManager::BuildTrampoline(AddressType originalAddress,
        const ByteVector& preservedBytes,
        AddressType returnAddress) {
        ByteVector trampoline;

        // 1. Add preserved bytes (relocated if necessary)
        ByteVector relocated = RelocateInstructions(originalAddress,
            returnAddress - preservedBytes.size(),
            preservedBytes);
        trampoline.insert(trampoline.end(), relocated.begin(), relocated.end());

        // 2. Add jump back to original function (after the hook)
        AddressType jumpTarget = originalAddress + preservedBytes.size();
        ByteVector jumpCode = assemblyEngine_->GenerateJump(
            returnAddress - 5,  // Address of JMP instruction
            jumpTarget
        );

        trampoline.insert(trampoline.end(), jumpCode.begin(), jumpCode.end());

        return trampoline;
    }

    ByteVector TrampolineManager::RelocateInstructions(AddressType from, AddressType to,
        const ByteVector& instructions) const {
        // For now, simple copy - in real implementation, this would
        // handle relative instructions that need adjustment

        ByteVector result = instructions;

        // TODO: Implement proper instruction relocation
        // This would need to:
        // 1. Disassemble instructions
        // 2. Identify relative branches/calls
        // 3. Adjust their offsets based on new location

        return result;
    }

    std::optional<HookEntry> TrampolineManager::FindHook(AddressType address) const {
        auto it = hooks_.find(address);
        if (it != hooks_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool TrampolineManager::InstallHook(AddressType targetAddress, AddressType hookFunction,
        const std::string& description) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check if already hooked
        if (hooks_.find(targetAddress) != hooks_.end()) {
            return false;
        }

        // Calculate preserve size
        size_t preserveSize = CalculatePreserveSize(targetAddress);
        if (preserveSize < kMinTrampolineSize) {
            preserveSize = kMinTrampolineSize;
        }

        // Read original bytes
        ByteVector originalBytes(preserveSize);
        if (!memoryManager_->ReadMemory(targetAddress, originalBytes.data(), preserveSize)) {
            return false;
        }

        // Allocate trampoline
        AddressType trampolineAddr = AllocateTrampoline(preserveSize + 16, targetAddress);
        if (trampolineAddr == 0) {
            return false;
        }

        // Build trampoline
        ByteVector trampolineCode = BuildTrampoline(targetAddress, originalBytes,
            trampolineAddr + preserveSize);

        // Write trampoline
        if (!memoryManager_->WriteMemory(trampolineAddr, trampolineCode.data(),
            trampolineCode.size())) {
            FreeTrampoline(trampolineAddr, preserveSize + 16);
            return false;
        }

        // Create jump to hook
        ByteVector hookJump = assemblyEngine_->GenerateJump(targetAddress, hookFunction);

        // Pad with NOPs if necessary
        while (hookJump.size() < preserveSize) {
            hookJump.push_back(0x90);
        }

        // Install hook
        if (!memoryManager_->WriteMemory(targetAddress, hookJump.data(), preserveSize)) {
            FreeTrampoline(trampolineAddr, preserveSize + 16);
            return false;
        }

        // Record hook
        HookEntry entry;
        entry.targetAddress = targetAddress;
        entry.trampolineAddress = trampolineAddr;
        entry.hookFunction = hookFunction;
        entry.originalBytes = originalBytes;
        entry.preserveSize = preserveSize;
        entry.description = description;

        hooks_[targetAddress] = entry;

        return true;
    }

    bool TrampolineManager::RemoveHook(AddressType targetAddress) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = hooks_.find(targetAddress);
        if (it == hooks_.end()) {
            return false;
        }

        const HookEntry& hook = it->second;

        // Restore original bytes
        if (!memoryManager_->WriteMemory(targetAddress, hook.originalBytes.data(),
            hook.originalBytes.size())) {
            return false;
        }

        // Free trampoline
        FreeTrampoline(hook.trampolineAddress, hook.preserveSize + 16);

        // Remove from map
        hooks_.erase(it);

        return true;
    }

    void TrampolineManager::RemoveAllHooks() {
        std::lock_guard<std::mutex> lock(mutex_);

        for (const auto& [address, hook] : hooks_) {
            // Restore original bytes
            memoryManager_->WriteMemory(address, hook.originalBytes.data(),
                hook.originalBytes.size());

            // Free trampoline
            FreeTrampoline(hook.trampolineAddress, hook.preserveSize + 16);
        }

        hooks_.clear();
    }

    bool TrampolineManager::IsHooked(AddressType address) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return hooks_.find(address) != hooks_.end();
    }

    std::optional<HookInfo> TrampolineManager::GetHookInfo(AddressType address) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = hooks_.find(address);
        if (it != hooks_.end()) {
            HookInfo info;
            info.targetAddress = it->second.targetAddress;
            info.trampolineAddress = it->second.trampolineAddress;
            info.hookFunction = it->second.hookFunction;
            info.isActive = true;
            info.description = it->second.description;
            return info;
        }

        return std::nullopt;
    }

    std::vector<HookInfo> TrampolineManager::GetAllHooks() const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<HookInfo> result;
        result.reserve(hooks_.size());

        for (const auto& [address, hook] : hooks_) {
            HookInfo info;
            info.targetAddress = hook.targetAddress;
            info.trampolineAddress = hook.trampolineAddress;
            info.hookFunction = hook.hookFunction;
            info.isActive = true;
            info.description = hook.description;
            result.push_back(info);
        }

        return result;
    }

    bool TrampolineManager::CreateDetour(AddressType targetAddress, const std::string& detourCode,
        AddressType& trampolineOut) {
        // First, create the hook code in memory
        AddressType detourAddr = memoryManager_->AllocateMemory(4096);
        if (detourAddr == 0) {
            return false;
        }

        // Assemble detour code
        auto assembled = assemblyEngine_->Assemble(detourCode, detourAddr);
        if (!assembled) {
            memoryManager_->FreeMemory(detourAddr);
            return false;
        }

        // Write assembled code
        if (!memoryManager_->WriteMemory(detourAddr, assembled->machineCode.data(),
            assembled->machineCode.size())) {
            memoryManager_->FreeMemory(detourAddr);
            return false;
        }

        // Install hook
        if (!InstallHook(targetAddress, detourAddr, "Detour")) {
            memoryManager_->FreeMemory(detourAddr);
            return false;
        }

        // Get trampoline address
        auto hookInfo = GetHookInfo(targetAddress);
        if (hookInfo) {
            trampolineOut = hookInfo->trampolineAddress;
            return true;
        }

        return false;
    }

    bool TrampolineManager::EnableHook(AddressType address) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = hooks_.find(address);
        if (it == hooks_.end()) {
            return false;
        }

        const HookEntry& hook = it->second;

        // Re-install hook jump
        ByteVector hookJump = assemblyEngine_->GenerateJump(address, hook.hookFunction);

        // Pad with NOPs
        while (hookJump.size() < hook.preserveSize) {
            hookJump.push_back(0x90);
        }

        return memoryManager_->WriteMemory(address, hookJump.data(), hook.preserveSize);
    }

    bool TrampolineManager::DisableHook(AddressType address) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = hooks_.find(address);
        if (it == hooks_.end()) {
            return false;
        }

        const HookEntry& hook = it->second;

        // Restore original bytes
        return memoryManager_->WriteMemory(address, hook.originalBytes.data(),
            hook.originalBytes.size());
    }

    std::optional<AddressType> TrampolineManager::GetTrampoline(AddressType hookedAddress) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = hooks_.find(hookedAddress);
        if (it != hooks_.end()) {
            return it->second.trampolineAddress;
        }

        return std::nullopt;
    }

    void TrampolineManager::CreatePool(AddressType nearAddress, size_t poolSize) {
        std::lock_guard<std::mutex> lock(mutex_);

        AddressType poolBase = memoryManager_->AllocateNear(nearAddress, poolSize);
        if (poolBase == 0) {
            return;
        }

        TrampolinePool pool;
        pool.baseAddress = poolBase;
        pool.totalSize = poolSize;
        pool.usedSize = 0;
        pool.freeBlocks.push_back({ 0, poolSize });

        pools_.push_back(pool);
    }

    void TrampolineManager::CleanupPools() {
        std::lock_guard<std::mutex> lock(mutex_);

        for (const auto& pool : pools_) {
            memoryManager_->FreeMemory(pool.baseAddress);
        }

        pools_.clear();
    }

    size_t TrampolineManager::FindSafeHookPoint(AddressType address, size_t minSize) const {
        // This would use a disassembler to find instruction boundaries
        // For now, use the simpler CalculatePreserveSize
        return CalculatePreserveSize(address);
    }

} // namespace AsmEngine