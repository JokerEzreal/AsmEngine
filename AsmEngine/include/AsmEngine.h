#pragma once

#include "Common.h"
#include "AOBScanner.h"
#include "SymbolManager.h"
#include "MemoryManager.h"
#include "AssemblyEngine.h"
#include "ScriptParser.h"
#include "CaptureStorage.h"

namespace AsmEngine {

    // Engine configuration
    struct EngineConfig {
        bool enableDebugPrivileges = true;
        bool autoCleanupOnExit = true;
        bool enableSymbolPersistence = false;
        std::string symbolPersistenceFile = "symbols.dat";
        size_t maxAllocationSize = 1024 * 1024; // 1MB
        std::chrono::milliseconds scanTimeout = std::chrono::milliseconds(5000);
    };

    // Main engine class
    class AsmEngine {
    private:
        // Process information
        HANDLE processHandle_;
        DWORD processId_;
        std::string processName_;

        // Core components
        std::unique_ptr<MemoryManager> memoryManager_;
        std::unique_ptr<SymbolManager> symbolManager_;
        std::unique_ptr<CaptureStorage> captureStorage_;
        std::unique_ptr<AOBScanner> aobScanner_;
        std::unique_ptr<AssemblyEngine> assemblyEngine_;
        std::unique_ptr<ScriptParser> scriptParser_;
        std::unique_ptr<TrampolineManager> trampolineManager_;

        // Configuration
        EngineConfig config_;

        // State
        bool isAttached_;
        std::vector<ScriptContext> scriptContexts_;

        // Initialize components
        void InitializeComponents();
        void ShutdownComponents();

    public:
        AsmEngine();
        explicit AsmEngine(const EngineConfig& config);
        ~AsmEngine();

        // Process attachment
        bool AttachToProcess(DWORD processId);
        bool AttachToProcess(const std::string& processName);
        void Detach();

        // Check attachment status
        bool IsAttached() const { return isAttached_; }
        DWORD GetProcessId() const { return processId_; }
        std::string GetProcessName() const { return processName_; }

        // Component access
        MemoryManager* Memory() { return memoryManager_.get(); }
        SymbolManager* Symbols() { return symbolManager_.get(); }
        CaptureStorage* Captures() { return captureStorage_.get(); }
        AOBScanner* Scanner() { return aobScanner_.get(); }
        AssemblyEngine* Assembly() { return assemblyEngine_.get(); }
        ScriptParser* Script() { return scriptParser_.get(); }

        // High-level operations

        // Execute a complete script
        bool ExecuteScript(const std::string& script);
        bool ExecuteScriptFile(const std::string& filename);

        // AOB scan with automatic capture storage
        std::optional<AddressType> FindPattern(const std::string& moduleName,
            const std::string& pattern);

        // Allocate memory with symbol registration
        AddressType AllocateMemory(const std::string& name, size_t size);

        // Create a detour hook
        bool CreateDetour(const std::string& targetSymbol,
            const std::string& detourCode);

        // Write assembly at address
        bool WriteAssembly(AddressType address, const std::string& assembly);
        bool WriteAssembly(const std::string& symbol, const std::string& assembly);

        // Batch operations
        struct BatchOperation {
            enum Type { Write, Allocate, Hook, Scan };
            Type type;
            std::string param1;
            std::string param2;
            size_t size;
        };

        bool ExecuteBatch(const std::vector<BatchOperation>& operations);

        // Configuration
        void SetConfig(const EngineConfig& config);
        const EngineConfig& GetConfig() const { return config_; }

        // Persistence
        void SaveState(const std::string& filename);
        void LoadState(const std::string& filename);

        // Error handling
        using ErrorHandler = std::function<void(ErrorCode, const std::string&)>;
        void SetErrorHandler(ErrorHandler handler);

    private:
        ErrorHandler errorHandler_;

        void HandleError(ErrorCode code, const std::string& message);

        // Process utilities
        static std::optional<DWORD> FindProcessId(const std::string& processName);
        static std::string GetProcessName(DWORD processId);
    };

    // Convenience functions for single operations
    namespace Quick {
        // Quick scan in process
        std::optional<AddressType> Scan(DWORD processId, const std::string& pattern);

        // Quick write
        bool Write(DWORD processId, AddressType address, const ByteVector& data);

        // Quick read
        std::optional<ByteVector> Read(DWORD processId, AddressType address, size_t size);

        bool ExecuteCEScript(const std::string& processName, const std::string& script);
        bool ExecuteCEScript(DWORD processId, const std::string& script);
    }

} // namespace AsmEngine

// Global convenience function for CE script execution
inline bool ExecuteCEScript(const std::string& processName, const std::string& script) {
    return AsmEngine::Quick::ExecuteCEScript(processName, script);
}