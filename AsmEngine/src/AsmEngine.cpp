#include "AsmEngine.h"
#include <TlHelp32.h>
#include <algorithm>
#include <iostream>

namespace AsmEngine {

    AsmEngine::AsmEngine() : AsmEngine(EngineConfig{}) {
    }

    AsmEngine::AsmEngine(const EngineConfig& config)
        : processHandle_(nullptr), processId_(0), config_(config), isAttached_(false) {

        if (config_.enableDebugPrivileges) {
            EnableDebugPrivilege();
        }
    }

    AsmEngine::~AsmEngine() {
        if (isAttached_) {
            Detach();
        }
    }

    void AsmEngine::InitializeComponents() {
        if (!processHandle_) {
            throw EngineException(ErrorCode::ProcessNotFound, "Not attached to process");
        }

        // Initialize components in order
        memoryManager_ = std::make_unique<MemoryManager>(processHandle_, processId_);
        symbolManager_ = std::make_unique<SymbolManager>();
        captureStorage_ = std::make_unique<CaptureStorage>();
        aobScanner_ = std::make_unique<AOBScanner>(processHandle_, captureStorage_.get());
        assemblyEngine_ = std::make_unique<AssemblyEngine>(symbolManager_.get(),
            captureStorage_.get());
        scriptParser_ = std::make_unique<ScriptParser>(this);
        trampolineManager_ = std::make_unique<TrampolineManager>(memoryManager_.get());

        // Load persisted symbols if enabled
        if (config_.enableSymbolPersistence && !config_.symbolPersistenceFile.empty()) {
            try {
                symbolManager_->ImportFromFile(config_.symbolPersistenceFile);
            }
            catch (...) {
                // Ignore errors loading symbols
            }
        }
    }

    void AsmEngine::ShutdownComponents() {
        // Save symbols if persistence enabled
        if (config_.enableSymbolPersistence && !config_.symbolPersistenceFile.empty() &&
            symbolManager_) {
            try {
                symbolManager_->ExportToFile(config_.symbolPersistenceFile);
            }
            catch (...) {
                // Ignore errors saving symbols
            }
        }

        // Cleanup allocations if enabled
        if (config_.autoCleanupOnExit && memoryManager_) {
            memoryManager_->CleanupAllocations();
        }

        // Destroy components in reverse order
        scriptParser_.reset();
        assemblyEngine_.reset();
        aobScanner_.reset();
        captureStorage_.reset();
        symbolManager_.reset();
        memoryManager_.reset();
    }

    bool AsmEngine::AttachToProcess(DWORD processId) {
        if (isAttached_) {
            Detach();
        }

        processHandle_ = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
        if (!processHandle_) {
            HandleError(ErrorCode::ProcessNotFound,
                "Failed to open process: " + std::to_string(processId));
            return false;
        }

        processId_ = processId;
        processName_ = GetProcessName(processId);
        isAttached_ = true;

        try {
            InitializeComponents();
        }
        catch (const std::exception& e) {
            Detach();
            throw;
        }

        return true;
    }

    bool AsmEngine::AttachToProcess(const std::string& processName) {
        auto pid = FindProcessId(processName);
        if (!pid) {
            HandleError(ErrorCode::ProcessNotFound,
                "Process not found: " + processName);
            return false;
        }

        return AttachToProcess(*pid);
    }

    void AsmEngine::Detach() {
        if (!isAttached_) {
            return;
        }

        ShutdownComponents();

        if (processHandle_) {
            CloseHandle(processHandle_);
            processHandle_ = nullptr;
        }

        processId_ = 0;
        processName_.clear();
        isAttached_ = false;
    }

    bool AsmEngine::ExecuteScript(const std::string& script) {
        if (!isAttached_) {
            HandleError(ErrorCode::ProcessNotFound, "Not attached to process");
            return false;
        }

        try {
            scriptParser_->Parse(script);
            scriptParser_->Execute();
            return true;
        }
        catch (const EngineException& e) {
            HandleError(e.code(), e.what());
            return false;
        }
    }

    bool AsmEngine::ExecuteScriptFile(const std::string& filename) {
        if (!isAttached_) {
            HandleError(ErrorCode::ProcessNotFound, "Not attached to process");
            return false;
        }

        try {
            scriptParser_->ParseFile(filename);
            scriptParser_->Execute();
            return true;
        }
        catch (const EngineException& e) {
            HandleError(e.code(), e.what());
            return false;
        }
    }

    std::optional<AddressType> AsmEngine::FindPattern(const std::string& moduleName,
        const std::string& pattern) {
        if (!isAttached_) {
            HandleError(ErrorCode::ProcessNotFound, "Not attached to process");
            return std::nullopt;
        }

        auto result = aobScanner_->ScanModule(moduleName, pattern);
        if (result) {
            return result->address;
        }

        return std::nullopt;
    }

    AddressType AsmEngine::AllocateMemory(const std::string& name, size_t size) {
        if (!isAttached_) {
            HandleError(ErrorCode::ProcessNotFound, "Not attached to process");
            return 0;
        }

        if (size > config_.maxAllocationSize) {
            HandleError(ErrorCode::InvalidParameter,
                "Allocation size exceeds maximum: " + std::to_string(size));
            return 0;
        }

        AddressType address = memoryManager_->AllocateMemory(size);
        if (address) {
            symbolManager_->RegisterAllocation(name, address, size);
        }

        return address;
    }

    bool AsmEngine::CreateDetour(const std::string& targetSymbol,
        const std::string& detourCode) {
        if (!isAttached_) {
            HandleError(ErrorCode::ProcessNotFound, "Not attached to process");
            return false;
        }

        // Resolve target address
        auto targetAddress = symbolManager_->ResolveAddress(targetSymbol);
        if (!targetAddress) {
            HandleError(ErrorCode::SymbolNotFound,
                "Target symbol not found: " + targetSymbol);
            return false;
        }

        // Allocate memory for detour code
        AddressType detourAddress = memoryManager_->AllocateMemory(4096);
        if (!detourAddress) {
            HandleError(ErrorCode::AllocationError, "Failed to allocate detour memory");
            return false;
        }

        // Assemble detour code
        auto assembled = assemblyEngine_->Assemble(detourCode, detourAddress);
        if (!assembled) {
            HandleError(ErrorCode::AssemblyError, "Failed to assemble detour code");
            memoryManager_->FreeMemory(detourAddress);
            return false;
        }

        // Write detour code
        if (!memoryManager_->WriteMemory(detourAddress, assembled->machineCode.data(),
            assembled->machineCode.size())) {
            HandleError(ErrorCode::MemoryAccessError, "Failed to write detour code");
            memoryManager_->FreeMemory(detourAddress);
            return false;
        }

        // Use TrampolineManager to create the hook
        if (!trampolineManager_->CreateHook(*targetAddress, detourAddress, targetSymbol)) {
            HandleError(ErrorCode::AllocationError, "Failed to create hook via trampoline");
            memoryManager_->FreeMemory(detourAddress);
            return false;
        }

        std::cout << "[AsmEngine] Detour created successfully:" << std::endl;
        std::cout << "  Target: " << targetSymbol << " (0x" << std::hex << *targetAddress << ")" << std::endl;
        std::cout << "  Detour Code: 0x" << detourAddress << std::dec << std::endl;

        return true;
    }

    bool AsmEngine::CreateHook(AddressType targetAddress,
        const std::string& hookCode,
        const std::string& description) {
        if (!isAttached_) {
            HandleError(ErrorCode::ProcessNotFound, "Not attached to process");
            return false;
        }

        // Allocate memory for hook code
        AddressType hookCodeAddress = memoryManager_->AllocateMemory(4096);
        if (!hookCodeAddress) {
            HandleError(ErrorCode::AllocationError, "Failed to allocate hook memory");
            return false;
        }

        // Assemble hook code
        auto assembled = assemblyEngine_->Assemble(hookCode, hookCodeAddress);
        if (!assembled) {
            HandleError(ErrorCode::AssemblyError, "Failed to assemble hook code");
            memoryManager_->FreeMemory(hookCodeAddress);
            return false;
        }

        // Write hook code
        if (!memoryManager_->WriteMemory(hookCodeAddress, assembled->machineCode.data(),
            assembled->machineCode.size())) {
            HandleError(ErrorCode::MemoryAccessError, "Failed to write hook code");
            memoryManager_->FreeMemory(hookCodeAddress);
            return false;
        }

        // Use TrampolineManager to create the hook
        if (!trampolineManager_->CreateHook(targetAddress, hookCodeAddress, description)) {
            HandleError(ErrorCode::AllocationError, "Failed to create hook via trampoline");
            memoryManager_->FreeMemory(hookCodeAddress);
            return false;
        }

        return true;
    }

    bool AsmEngine::RemoveHook(AddressType targetAddress) {
        if (!isAttached_) {
            HandleError(ErrorCode::ProcessNotFound, "Not attached to process");
            return false;
        }

        return trampolineManager_->RemoveHook(targetAddress);
    }

    std::vector<std::pair<AddressType, AddressType>> AsmEngine::GetActiveHooks() const {
        std::vector<std::pair<AddressType, AddressType>> result;

        if (trampolineManager_) {
            auto hooks = trampolineManager_->GetAllHooks();
            for (const auto& hook : hooks) {
                result.push_back({ hook.originalAddress, hook.hookCodeAddress });
            }
        }

        return result;
    }

    bool AsmEngine::WriteAssembly(AddressType address, const std::string& assembly) {
        if (!isAttached_) {
            HandleError(ErrorCode::ProcessNotFound, "Not attached to process");
            return false;
        }

        auto assembled = assemblyEngine_->Assemble(assembly, address);
        if (!assembled) {
            HandleError(ErrorCode::AssemblyError, "Failed to assemble code");
            return false;
        }

        return memoryManager_->WriteMemory(address, assembled->machineCode.data(),
            assembled->machineCode.size());
    }

    bool AsmEngine::WriteAssembly(const std::string& symbol, const std::string& assembly) {
        auto address = symbolManager_->ResolveAddress(symbol);
        if (!address) {
            HandleError(ErrorCode::SymbolNotFound, "Symbol not found: " + symbol);
            return false;
        }

        return WriteAssembly(*address, assembly);
    }

    bool AsmEngine::ExecuteBatch(const std::vector<BatchOperation>& operations) {
        if (!isAttached_) {
            HandleError(ErrorCode::ProcessNotFound, "Not attached to process");
            return false;
        }

        // Execute each operation
        for (const auto& op : operations) {
            try {
                switch (op.type) {
                case BatchOperation::Write: {
                    if (!WriteAssembly(op.param1, op.param2)) {
                        return false;
                    }
                    break;
                }

                case BatchOperation::Allocate: {
                    if (!AllocateMemory(op.param1, op.size)) {
                        return false;
                    }
                    break;
                }

                case BatchOperation::Hook: {
                    if (!CreateDetour(op.param1, op.param2)) {
                        return false;
                    }
                    break;
                }

                case BatchOperation::Scan: {
                    if (!FindPattern(op.param1, op.param2)) {
                        return false;
                    }
                    break;
                }
                }
            }
            catch (const EngineException& e) {
                HandleError(e.code(), e.what());
                return false;
            }
        }

        return true;
    }

    void AsmEngine::SetConfig(const EngineConfig& config) {
        config_ = config;
    }

    void AsmEngine::SaveState(const std::string& filename) {
        if (symbolManager_) {
            symbolManager_->ExportToFile(filename);
        }
    }

    void AsmEngine::LoadState(const std::string& filename) {
        if (symbolManager_) {
            symbolManager_->ImportFromFile(filename);
        }
    }

    void AsmEngine::SetErrorHandler(ErrorHandler handler) {
        errorHandler_ = handler;
    }

    void AsmEngine::HandleError(ErrorCode code, const std::string& message) {
        if (errorHandler_) {
            errorHandler_(code, message);
        }
    }

    std::optional<DWORD> AsmEngine::FindProcessId(const std::string& processName) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return std::nullopt;
        }

        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        if (Process32FirstW(snapshot, &pe32)) {
            do {
                // Convert wide string to string for comparison
                std::wstring wExeFile(pe32.szExeFile);
                std::string exeFile(wExeFile.begin(), wExeFile.end());

                if (_stricmp(exeFile.c_str(), processName.c_str()) == 0) {
                    CloseHandle(snapshot);
                    return pe32.th32ProcessID;
                }
            } while (Process32NextW(snapshot, &pe32));
        }

        CloseHandle(snapshot);
        return std::nullopt;
    }

    std::string AsmEngine::GetProcessName(DWORD processId) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return "";
        }

        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        if (Process32FirstW(snapshot, &pe32)) {
            do {
                if (pe32.th32ProcessID == processId) {
                    std::wstring wExeFile(pe32.szExeFile);
                    std::string exeFile(wExeFile.begin(), wExeFile.end());
                    CloseHandle(snapshot);
                    return exeFile;
                }
            } while (Process32NextW(snapshot, &pe32));
        }

        CloseHandle(snapshot);
        return "";
    }

    // Quick namespace functions
    namespace Quick {

        std::optional<AddressType> Scan(DWORD processId, const std::string& pattern) {
            AsmEngine engine;
            if (!engine.AttachToProcess(processId)) {
                return std::nullopt;
            }

            auto results = engine.Scanner()->ScanAll(pattern);
            if (!results.empty()) {
                return results[0].address;
            }

            return std::nullopt;
        }

        bool Write(DWORD processId, AddressType address, const ByteVector& data) {
            HANDLE process = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                FALSE, processId);
            if (!process) {
                return false;
            }

            SIZE_T written;
            bool result = WriteProcessMemory(process,
                reinterpret_cast<LPVOID>(address),
                data.data(), data.size(), &written);

            CloseHandle(process);
            return result && written == data.size();
        }

        std::optional<ByteVector> Read(DWORD processId, AddressType address, size_t size) {
            HANDLE process = OpenProcess(PROCESS_VM_READ, FALSE, processId);
            if (!process) {
                return std::nullopt;
            }

            ByteVector data(size);
            SIZE_T read;
            bool result = ReadProcessMemory(process,
                reinterpret_cast<LPCVOID>(address),
                data.data(), size, &read);

            CloseHandle(process);

            if (result && read == size) {
                return data;
            }

            return std::nullopt;
        }

        bool ExecuteCEScript(const std::string& processName, const std::string& script) {
            try {
                AsmEngine engine;

                // Attach to process
                if (!engine.AttachToProcess(processName)) {
                    return false;
                }

                // Execute script
                return engine.ExecuteScript(script);
            }
            catch (...) {
                return false;
            }
        }

        bool ExecuteCEScript(DWORD processId, const std::string& script) {
            try {
                AsmEngine engine;

                // Attach to process
                if (!engine.AttachToProcess(processId)) {
                    return false;
                }

                // Execute script
                return engine.ExecuteScript(script);
            }
            catch (...) {
                return false;
            }
        }

    } // namespace Quick

} // namespace AsmEngine