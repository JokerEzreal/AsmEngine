// TrampolineManager.cpp
#include "TrampolineManager.h"
#include <iostream>

namespace AsmEngine {

    TrampolineManager::TrampolineManager(MemoryManager* memoryManager)
        : memoryManager_(memoryManager), trampolineBase_(0),
        trampolineSize_(0), entrySize_(14), maxEntries_(0) {
    }

    TrampolineManager::~TrampolineManager() {
        Cleanup();
    }

    bool TrampolineManager::Initialize(size_t maxEntries) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (trampolineBase_ != 0) {
            return true; // Already initialized
        }

        maxEntries_ = maxEntries;
        trampolineSize_ = maxEntries * entrySize_;

        // 尝试在高位地址分配内存（靠近原程序代码）
        // 这样可以使用32位相对跳转
        AddressType nearAddress = 0x7FF600000000; // 典型的高位地址

        // 尝试分配
        trampolineBase_ = memoryManager_->AllocateNear(nearAddress, trampolineSize_,
            MemoryProtection::ExecuteReadWrite);

        if (trampolineBase_ == 0) {
            // 如果失败，尝试普通分配
            trampolineBase_ = memoryManager_->AllocateMemory(trampolineSize_,
                MemoryProtection::ExecuteReadWrite);
        }

        if (trampolineBase_ == 0) {
            std::cerr << "[ERROR] Failed to allocate trampoline memory" << std::endl;
            return false;
        }

        std::cout << "[DEBUG] Trampoline memory allocated at: 0x"
            << std::hex << trampolineBase_ << std::dec
            << " (size: 0x" << std::hex << trampolineSize_ << ")" << std::dec << std::endl;

        // 初始化所有槽位为空
        std::vector<uint8_t> emptySlot(entrySize_, 0xCC); // INT3
        for (size_t i = 0; i < maxEntries_; ++i) {
            memoryManager_->WriteMemory(trampolineBase_ + i * entrySize_,
                emptySlot.data(), emptySlot.size());
        }

        return true;
    }

    size_t TrampolineManager::FindFreeSlot() const {
        for (size_t i = 0; i < maxEntries_; ++i) {
            bool used = false;
            for (const auto& [name, entry] : entries_) {
                if (entry.entryAddress == trampolineBase_ + i * entrySize_) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                return i;
            }
        }
        return maxEntries_; // No free slot
    }

    std::optional<AddressType> TrampolineManager::AllocateTrampoline(
        const std::string& name, AddressType targetAddress) {

        std::lock_guard<std::mutex> lock(mutex_);

        // 检查是否已存在
        if (entries_.find(name) != entries_.end()) {
            return entries_[name].entryAddress;
        }

        // 查找空闲槽位
        size_t slot = FindFreeSlot();
        if (slot >= maxEntries_) {
            std::cerr << "[ERROR] No free trampoline slots available" << std::endl;
            return std::nullopt;
        }

        AddressType entryAddress = trampolineBase_ + slot * entrySize_;

        // 生成 FF 25 绝对跳转代码
        ByteVector jumpCode = ImprovedHookGenerator::GenerateAbsoluteJump(targetAddress);

        // 写入跳转代码
        if (!memoryManager_->WriteMemory(entryAddress, jumpCode.data(), jumpCode.size())) {
            std::cerr << "[ERROR] Failed to write trampoline code" << std::endl;
            return std::nullopt;
        }

        // 记录入口
        TrampolineEntry entry;
        entry.entryAddress = entryAddress;
        entry.targetAddress = targetAddress;
        entry.originalAddress = 0; // 稍后设置
        entry.isUsed = true;
        entry.name = name;

        entries_[name] = entry;

        std::cout << "[DEBUG] Allocated trampoline '" << name << "' at 0x"
            << std::hex << entryAddress << " -> 0x" << targetAddress << std::dec << std::endl;

        return entryAddress;
    }

    bool TrampolineManager::FreeTrampoline(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = entries_.find(name);
        if (it == entries_.end()) {
            return false;
        }

        // 用INT3填充槽位
        std::vector<uint8_t> int3(entrySize_, 0xCC);
        memoryManager_->WriteMemory(it->second.entryAddress, int3.data(), int3.size());

        entries_.erase(it);
        return true;
    }

    std::optional<TrampolineEntry> TrampolineManager::GetTrampoline(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = entries_.find(name);
        if (it != entries_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void TrampolineManager::Cleanup() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (trampolineBase_ != 0) {
            memoryManager_->FreeMemory(trampolineBase_);
            trampolineBase_ = 0;
        }

        entries_.clear();
    }

    // ImprovedHookGenerator 实现
    ByteVector ImprovedHookGenerator::GenerateRelativeJump(AddressType from, AddressType to) {
        ByteVector jump;

        // 计算相对偏移
        int64_t offset = static_cast<int64_t>(to) - static_cast<int64_t>(from) - 5;

        // E9 相对跳转
        jump.push_back(0xE9);

        // 添加32位偏移（小端序）
        int32_t offset32 = static_cast<int32_t>(offset);
        jump.push_back(offset32 & 0xFF);
        jump.push_back((offset32 >> 8) & 0xFF);
        jump.push_back((offset32 >> 16) & 0xFF);
        jump.push_back((offset32 >> 24) & 0xFF);

        return jump;
    }

    ByteVector ImprovedHookGenerator::GenerateAbsoluteJump(AddressType targetAddress) {
        ByteVector jump;

        // FF 25 00 00 00 00 - JMP [RIP+0]
        jump.push_back(0xFF);
        jump.push_back(0x25);
        jump.push_back(0x00);
        jump.push_back(0x00);
        jump.push_back(0x00);
        jump.push_back(0x00);

        // 8字节绝对地址（小端序）
        for (int i = 0; i < 8; ++i) {
            jump.push_back((targetAddress >> (i * 8)) & 0xFF);
        }

        return jump;
    }

    ByteVector ImprovedHookGenerator::GenerateReturnTrampoline(AddressType returnAddress) {
        // 生成返回原始代码的跳转
        return GenerateAbsoluteJump(returnAddress);
    }

    std::optional<ImprovedHookGenerator::HookInfo> ImprovedHookGenerator::CreateHook(
        const std::string& name,
        AddressType originalAddress,
        AddressType hookCodeAddress) {

        HookInfo info;
        info.name = name;
        info.originalAddress = originalAddress;
        info.hookCodeAddress = hookCodeAddress;

        // 1. 分配跳板
        auto trampolineOpt = trampolineManager_->AllocateTrampoline(name, hookCodeAddress);
        if (!trampolineOpt) {
            std::cerr << "[ERROR] Failed to allocate trampoline for hook: " << name << std::endl;
            return std::nullopt;
        }

        info.trampolineAddress = *trampolineOpt;

        // 2. 读取原始字节
        info.originalBytes.resize(5);
        if (!memoryManager_->ReadMemory(originalAddress, info.originalBytes.data(), 5)) {
            std::cerr << "[ERROR] Failed to read original bytes at: 0x"
                << std::hex << originalAddress << std::dec << std::endl;
            trampolineManager_->FreeTrampoline(name);
            return std::nullopt;
        }

        // 3. 生成E9跳转到跳板
        info.hookBytes = GenerateRelativeJump(originalAddress, info.trampolineAddress);

        std::cout << "[DEBUG] Hook '" << name << "' created:" << std::endl;
        std::cout << "  Original: 0x" << std::hex << originalAddress << std::endl;
        std::cout << "  Trampoline: 0x" << info.trampolineAddress << std::endl;
        std::cout << "  Hook code: 0x" << hookCodeAddress << std::dec << std::endl;

        return info;
    }

} // namespace AsmEngine