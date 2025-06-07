#include "TrampolineManager.h"
#include <algorithm>
#include <iostream>
#include <iomanip>

namespace AsmEngine {

    TrampolineManager::TrampolineManager(MemoryManager* memoryManager)
        : memoryManager_(memoryManager) {
    }

    TrampolineManager::~TrampolineManager() {
        Cleanup();
    }

    AddressType TrampolineManager::AllocateNearMemory(AddressType nearAddress,
        size_t size,
        int64_t maxDistance) {
        // 尝试在目标地址的±2GB范围内分配内存
        const size_t pageSize = 0x1000; // 4KB
        const size_t allocSize = (size + pageSize - 1) & ~(pageSize - 1); // 对齐到页边界

        // 首先尝试在高地址分配（优先使用高位地址）
        AddressType targetAddr = nearAddress + 0x70000000; // 从较高地址开始

        // 向上搜索
        for (int attempts = 0; attempts < 100; attempts++) {
            AddressType addr = memoryManager_->AllocateNear(targetAddr, allocSize);
            if (addr != 0) {
                // 检查是否在可达范围内
                int64_t distance = static_cast<int64_t>(addr) - static_cast<int64_t>(nearAddress);
                if (std::abs(distance) <= maxDistance) {
                    std::cout << "[TrampolineManager] Allocated high memory at 0x"
                        << std::hex << addr << " (distance: 0x"
                        << std::abs(distance) << ")" << std::dec << std::endl;
                    return addr;
                }
                else {
                    // 距离太远，释放并继续尝试
                    memoryManager_->FreeMemory(addr);
                }
            }
            targetAddr -= 0x1000000; // 每次减少16MB

            // 如果已经太接近原始地址，改为向下搜索
            if (targetAddr < nearAddress + 0x10000000) {
                break;
            }
        }

        // 如果高地址失败，尝试低地址
        targetAddr = nearAddress - 0x10000000;
        for (int attempts = 0; attempts < 100; attempts++) {
            AddressType addr = memoryManager_->AllocateNear(targetAddr, allocSize);
            if (addr != 0) {
                int64_t distance = static_cast<int64_t>(addr) - static_cast<int64_t>(nearAddress);
                if (std::abs(distance) <= maxDistance) {
                    std::cout << "[TrampolineManager] Allocated low memory at 0x"
                        << std::hex << addr << " (distance: 0x"
                        << std::abs(distance) << ")" << std::dec << std::endl;
                    return addr;
                }
                memoryManager_->FreeMemory(addr);
            }
            targetAddr -= 0x1000000;
        }

        return 0;
    }

    AddressType TrampolineManager::FindOrCreateJumpTable(AddressType nearAddress) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 查找现有的跳转表
        for (auto& [baseAddr, table] : jumpTables_) {
            // 检查是否在可达范围内且有空间
            int64_t distance = static_cast<int64_t>(table.baseAddress) - static_cast<int64_t>(nearAddress);
            if (std::abs(distance) <= 0x7FFFFFFF && table.used < table.capacity) {
                return table.baseAddress;
            }
        }

        // 创建新的跳转表
        size_t tableSize = jumpTableCapacity_ * jumpTableEntrySize_;
        AddressType tableAddr = AllocateNearMemory(nearAddress, tableSize);

        if (tableAddr == 0) {
            std::cerr << "[TrampolineManager] Failed to allocate jump table near 0x"
                << std::hex << nearAddress << std::dec << std::endl;
            return 0;
        }

        // 初始化跳转表
        JumpTable newTable;
        newTable.baseAddress = tableAddr;
        newTable.capacity = jumpTableCapacity_;
        newTable.used = 0;

        // 初始化所有条目
        for (size_t i = 0; i < jumpTableCapacity_; i++) {
            JumpTableEntry entry;
            entry.entryAddress = tableAddr + (i * jumpTableEntrySize_);
            entry.targetAddress = 0;
            entry.inUse = false;
            newTable.entries.push_back(entry);
        }

        jumpTables_[nearAddress] = newTable;

        std::cout << "[TrampolineManager] Created jump table at 0x"
            << std::hex << tableAddr << " for hooks near 0x"
            << nearAddress << std::dec << std::endl;

        return tableAddr;
    }

    ByteVector TrampolineManager::GenerateJumpTableEntry(AddressType targetAddress) {
        ByteVector code;

        // 生成绝对跳转指令: FF 25 00 00 00 00 [8字节地址]
        // 这是 jmp qword ptr [rip+0]
        code.push_back(0xFF);
        code.push_back(0x25);
        code.push_back(0x00);
        code.push_back(0x00);
        code.push_back(0x00);
        code.push_back(0x00);

        // 添加目标地址（小端序）
        for (int i = 0; i < 8; i++) {
            code.push_back((targetAddress >> (i * 8)) & 0xFF);
        }

        return code;
    }

    std::optional<AddressType> TrampolineManager::AllocateJumpTableEntry(
        AddressType jumpTableBase, AddressType targetAddress) {

        // 找到对应的跳转表
        auto it = std::find_if(jumpTables_.begin(), jumpTables_.end(),
            [jumpTableBase](const auto& pair) {
                return pair.second.baseAddress == jumpTableBase;
            });

        if (it == jumpTables_.end()) {
            return std::nullopt;
        }

        JumpTable& table = it->second;

        // 找到空闲的条目
        for (auto& entry : table.entries) {
            if (!entry.inUse) {
                entry.inUse = true;
                entry.targetAddress = targetAddress;
                table.used++;

                // 生成并写入跳转代码
                ByteVector jumpCode = GenerateJumpTableEntry(targetAddress);
                if (!memoryManager_->WriteMemory(entry.entryAddress,
                    jumpCode.data(),
                    jumpCode.size())) {
                    entry.inUse = false;
                    table.used--;
                    return std::nullopt;
                }

                return entry.entryAddress;
            }
        }

        return std::nullopt;
    }

    bool TrampolineManager::CreateHook(AddressType originalAddress,
        AddressType hookCodeAddress,
        const std::string& description) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 检查是否已经存在
        if (hooks_.find(originalAddress) != hooks_.end()) {
            std::cerr << "[TrampolineManager] Hook already exists at 0x"
                << std::hex << originalAddress << std::dec << std::endl;
            return false;
        }

        // 找到或创建跳转表
        AddressType jumpTableBase = FindOrCreateJumpTable(originalAddress);
        if (jumpTableBase == 0) {
            std::cerr << "[TrampolineManager] Failed to create jump table for 0x"
                << std::hex << originalAddress << std::dec << std::endl;
            return false;
        }

        // 在跳转表中分配条目
        auto jumpEntry = AllocateJumpTableEntry(jumpTableBase, hookCodeAddress);
        if (!jumpEntry) {
            std::cerr << "[TrampolineManager] Failed to allocate jump table entry" << std::endl;
            return false;
        }

        // 读取原始字节
        ByteVector originalBytes(5);
        if (!memoryManager_->ReadMemory(originalAddress,
            originalBytes.data(),
            originalBytes.size())) {
            std::cerr << "[TrampolineManager] Failed to read original bytes" << std::endl;
            return false;
        }

        // 生成E9跳转到跳转表
        ByteVector hookJump;
        hookJump.push_back(0xE9); // jmp rel32

        int32_t relOffset = static_cast<int32_t>(*jumpEntry - originalAddress - 5);
        hookJump.push_back(relOffset & 0xFF);
        hookJump.push_back((relOffset >> 8) & 0xFF);
        hookJump.push_back((relOffset >> 16) & 0xFF);
        hookJump.push_back((relOffset >> 24) & 0xFF);

        // 写入hook
        if (!memoryManager_->WriteMemory(originalAddress,
            hookJump.data(),
            hookJump.size())) {
            std::cerr << "[TrampolineManager] Failed to write hook jump" << std::endl;
            return false;
        }

        // 保存hook信息
        HookEntry hook;
        hook.originalAddress = originalAddress;
        hook.jumpTableEntry = *jumpEntry;
        hook.hookCodeAddress = hookCodeAddress;
        hook.originalBytes = originalBytes;
        hook.hookSize = 5;

        hooks_[originalAddress] = hook;

        std::cout << "[TrampolineManager] Hook created:" << std::endl;
        std::cout << "  Original: 0x" << std::hex << originalAddress << std::endl;
        std::cout << "  Jump Table Entry: 0x" << *jumpEntry << std::endl;
        std::cout << "  Hook Code: 0x" << hookCodeAddress << std::endl;
        std::cout << "  Description: " << description << std::dec << std::endl;

        return true;
    }

    bool TrampolineManager::RemoveHook(AddressType originalAddress) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = hooks_.find(originalAddress);
        if (it == hooks_.end()) {
            return false;
        }

        HookEntry& hook = it->second;

        // 恢复原始字节
        if (!memoryManager_->WriteMemory(hook.originalAddress,
            hook.originalBytes.data(),
            hook.originalBytes.size())) {
            return false;
        }

        // 释放跳转表条目
        for (auto& [addr, table] : jumpTables_) {
            for (auto& entry : table.entries) {
                if (entry.entryAddress == hook.jumpTableEntry) {
                    entry.inUse = false;
                    entry.targetAddress = 0;
                    table.used--;

                    // 清空跳转表条目
                    ByteVector nops(jumpTableEntrySize_, 0x90);
                    memoryManager_->WriteMemory(entry.entryAddress,
                        nops.data(),
                        nops.size());
                    break;
                }
            }
        }

        hooks_.erase(it);
        return true;
    }

    std::optional<HookEntry> TrampolineManager::GetHookInfo(AddressType address) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = hooks_.find(address);
        if (it != hooks_.end()) {
            return it->second;
        }

        return std::nullopt;
    }

    std::vector<HookEntry> TrampolineManager::GetAllHooks() const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<HookEntry> result;
        for (const auto& [addr, hook] : hooks_) {
            result.push_back(hook);
        }

        return result;
    }

    std::vector<JumpTable> TrampolineManager::GetJumpTables() const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<JumpTable> result;
        for (const auto& [addr, table] : jumpTables_) {
            result.push_back(table);
        }

        return result;
    }

    void TrampolineManager::Cleanup() {
        std::lock_guard<std::mutex> lock(mutex_);

        // 先恢复所有hook
        for (const auto& [addr, hook] : hooks_) {
            memoryManager_->WriteMemory(hook.originalAddress,
                hook.originalBytes.data(),
                hook.originalBytes.size());
        }

        // 释放所有跳转表
        for (const auto& [addr, table] : jumpTables_) {
            memoryManager_->FreeMemory(table.baseAddress);
        }

        hooks_.clear();
        jumpTables_.clear();
    }

} // namespace AsmEngine