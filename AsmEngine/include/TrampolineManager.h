// TrampolineManager.h
#pragma once

#include "Common.h"
#include "MemoryManager.h"
#include <map>
#include <mutex>

namespace AsmEngine {

    // 跳板入口结构
    struct TrampolineEntry {
        AddressType entryAddress;      // 高位内存中的入口地址
        AddressType targetAddress;     // 目标hook代码地址
        AddressType originalAddress;   // 原始代码地址（用于返回）
        bool isUsed;
        std::string name;
    };

    class TrampolineManager {
    private:
        MemoryManager* memoryManager_;
        AddressType trampolineBase_;   // 高位内存基址
        size_t trampolineSize_;        // 高位内存大小
        size_t entrySize_;             // 每个跳板入口大小（14字节：FF 25 + 8字节地址）
        size_t maxEntries_;            // 最大跳板数量

        std::map<std::string, TrampolineEntry> entries_;
        std::mutex mutex_;

        // 查找可用的跳板槽位
        size_t FindFreeSlot() const;

    public:
        TrampolineManager(MemoryManager* memoryManager);
        ~TrampolineManager();

        // 初始化跳板内存（在高位地址）
        bool Initialize(size_t maxEntries = 1024);

        // 分配一个跳板
        std::optional<AddressType> AllocateTrampoline(const std::string& name,
            AddressType targetAddress);

        // 释放跳板
        bool FreeTrampoline(const std::string& name);

        // 获取跳板信息
        std::optional<TrampolineEntry> GetTrampoline(const std::string& name) const;

        // 清理所有跳板
        void Cleanup();

        // 获取跳板基址
        AddressType GetBase() const { return trampolineBase_; }
    };

    // 改进的Hook生成器
    class ImprovedHookGenerator {
    private:
        MemoryManager* memoryManager_;
        TrampolineManager* trampolineManager_;

    public:
        ImprovedHookGenerator(MemoryManager* memoryManager,
            TrampolineManager* trampolineManager)
            : memoryManager_(memoryManager),
            trampolineManager_(trampolineManager) {
        }

        // Hook信息
        struct HookInfo {
            AddressType originalAddress;    // 原始地址
            AddressType trampolineAddress;  // 高位跳板地址
            AddressType hookCodeAddress;    // Hook代码地址
            ByteVector originalBytes;       // 原始字节
            ByteVector hookBytes;           // Hook字节（E9 相对跳转）
            std::string name;
        };

        // 创建Hook
        std::optional<HookInfo> CreateHook(const std::string& name,
            AddressType originalAddress,
            AddressType hookCodeAddress);

        // 生成E9相对跳转（5字节）
        static ByteVector GenerateRelativeJump(AddressType from, AddressType to);

        // 生成FF25绝对跳转（14字节）
        static ByteVector GenerateAbsoluteJump(AddressType targetAddress);

        // 生成返回跳板代码
        static ByteVector GenerateReturnTrampoline(AddressType returnAddress);
    };

} // namespace AsmEngine