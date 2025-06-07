#pragma once

#include "Common.h"
#include "MemoryManager.h"
#include <mutex>
#include <vector>
#include <map>

namespace AsmEngine {

    // 跳转表项
    struct JumpTableEntry {
        AddressType entryAddress;      // 在跳转表中的地址
        AddressType targetAddress;     // 目标地址（低位地址）
        bool inUse;
        std::string description;       // 描述信息
    };

    // 跳转表
    struct JumpTable {
        AddressType baseAddress;       // 跳转表基址（高位地址）
        size_t capacity;              // 容量（可容纳的跳转项数）
        size_t used;                  // 已使用的跳转项数
        std::vector<JumpTableEntry> entries;
    };

    // Hook信息
    struct HookEntry {
        AddressType originalAddress;   // 原始地址（被hook的地址）
        AddressType jumpTableEntry;    // 跳转表项地址
        AddressType hookCodeAddress;   // 实际hook代码地址
        ByteVector originalBytes;      // 原始字节
        size_t hookSize;              // hook大小（通常是5字节）
    };

    class TrampolineManager {
    private:
        MemoryManager* memoryManager_;
        mutable std::mutex mutex_;

        // 跳转表管理
        std::map<AddressType, JumpTable> jumpTables_;  // key是需要hook的地址范围

        // Hook管理
        std::map<AddressType, HookEntry> hooks_;       // key是原始地址

        // 配置
        size_t jumpTableEntrySize_ = 14;  // 每个跳转表项的大小（FF 25 + 8字节地址）
        size_t jumpTableCapacity_ = 256;   // 每个跳转表的默认容量

        // 寻找或创建适合的跳转表
        AddressType FindOrCreateJumpTable(AddressType nearAddress);

        // 在跳转表中分配一个条目
        std::optional<AddressType> AllocateJumpTableEntry(AddressType jumpTableBase,
            AddressType targetAddress);

        // 生成跳转表项代码
        ByteVector GenerateJumpTableEntry(AddressType targetAddress);

        // 尝试在指定范围内分配内存
        AddressType AllocateNearMemory(AddressType nearAddress, size_t size,
            int64_t maxDistance = 0x7FFFFFFF);

    public:
        TrampolineManager(MemoryManager* memoryManager);
        ~TrampolineManager();

        // 创建Hook
        bool CreateHook(AddressType originalAddress,     // 要hook的地址
            AddressType hookCodeAddress,       // hook代码地址
            const std::string& description = "");

        // 移除Hook
        bool RemoveHook(AddressType originalAddress);

        // 获取Hook信息
        std::optional<HookEntry> GetHookInfo(AddressType address) const;

        // 获取所有活动的Hook
        std::vector<HookEntry> GetAllHooks() const;

        // 清理所有Hook和跳转表
        void Cleanup();

        // 设置跳转表容量
        void SetJumpTableCapacity(size_t capacity) { jumpTableCapacity_ = capacity; }

        // 获取跳转表信息
        std::vector<JumpTable> GetJumpTables() const;
    };

} // namespace AsmEngine