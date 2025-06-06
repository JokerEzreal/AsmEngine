#include "AsmEngine.h"
#include <iostream>
#include <iomanip>

// 不要使用 using namespace AsmEngine;
// 而是使用具体的类型别名或完整限定名

// Example: Using the engine to patch a game
void ExampleGamePatch() {
    std::cout << "=== Game Patching Example ===" << std::endl;

    // Create engine with custom configuration
    AsmEngine::EngineConfig config;
    config.enableDebugPrivileges = true;
    config.autoCleanupOnExit = false;  // Keep patches active
    config.enableSymbolPersistence = true;
    config.symbolPersistenceFile = "game_symbols.dat";

    AsmEngine::AsmEngine engine(config);  // 使用完整限定名

    // Attach to game process
    if (!engine.AttachToProcess("game.exe")) {
        std::cerr << "Failed to attach to game.exe" << std::endl;
        return;
    }

    std::cout << "Attached to process: " << engine.GetProcessName()
        << " (PID: " << engine.GetProcessId() << ")" << std::endl;

    // Example script with capture groups
    const char* script = R"(
[ENABLE]
// Find player health structure with dynamic offset
aobscanmodule(PlayerBase, game.exe, 48 8B 05 s1.4 48 83 C0 s2.1)
registersymbol(PlayerBase)

// The captured values s1 and s2 are now available
// s1 contains the 4-byte offset to player structure
// s2 contains the 1-byte offset to health within structure

// Allocate memory for our code
alloc(HealthHook, 1024, PlayerBase)
registersymbol(HealthHook)

label(code)
label(return)

HealthHook:
    // Original code
    mov rax,[rax+s1]  // s1 is automatically replaced with captured value
    add rax,s2        // s2 is automatically replaced with captured value
    
    // Our modification - set health to 9999
    push rbx
    mov rbx,9999
    mov [rax],rbx
    pop rbx
    
    jmp return

code:
    jmp HealthHook
    nop
    nop

return:

// Write jump at original location
PlayerBase:
    jmp code
    nop
    nop

[DISABLE]
// Restore original code
PlayerBase:
    mov rax,[rax+s1]
    add rax,s2

dealloc(HealthHook)
unregistersymbol(HealthHook)
unregistersymbol(PlayerBase)
)";

    // Execute the script
    if (engine.ExecuteScript(script)) {
        std::cout << "Script executed successfully!" << std::endl;

        // Display captured values
        auto captures = engine.Captures()->ExportAll();
        std::cout << "\nCaptured values:" << std::endl;
        for (const auto& [name, value] : captures) {
            std::cout << "  " << name << " = 0x" << std::hex;

            switch (value.size) {
            case 1:
                std::cout << static_cast<int>(value.AsUInt8());
                break;
            case 2:
                std::cout << value.AsUInt16();
                break;
            case 4:
                std::cout << value.AsUInt32();
                break;
            case 8:
                std::cout << value.AsUInt64();
                break;
            }

            std::cout << " (size: " << std::dec << value.size << " bytes)" << std::endl;
        }

        // Display registered symbols
        auto symbols = engine.Symbols()->GetAllSymbols();
        std::cout << "\nRegistered symbols:" << std::endl;
        for (const auto& symbol : symbols) {
            std::cout << "  " << symbol.name << " = 0x"
                << std::hex << symbol.address << std::endl;
        }
    }
    else {
        std::cerr << "Failed to execute script!" << std::endl;
    }
}

// Example: Manual operations without script
void ExampleManualOperations() {
    std::cout << "\n=== Manual Operations Example ===" << std::endl;

    AsmEngine::AsmEngine engine;  // 使用完整限定名

    // Attach to process by PID
    DWORD pid = 1234; // Replace with actual PID
    if (!engine.AttachToProcess(pid)) {
        std::cerr << "Failed to attach to process" << std::endl;
        return;
    }

    // Scan for pattern with capture groups
    std::string pattern = "48 89 5C 24 s1.1 48 89 74 24 s2.1";
    auto result = engine.FindPattern("module.dll", pattern);

    if (result) {
        std::cout << "Pattern found at: 0x" << std::hex << *result << std::endl;

        // Get captured values
        auto s1 = engine.Captures()->Get("s1");
        auto s2 = engine.Captures()->Get("s2");

        if (s1 && s2) {
            std::cout << "Captured s1 = " << static_cast<int>(s1->AsUInt8()) << std::endl;
            std::cout << "Captured s2 = " << static_cast<int>(s2->AsUInt8()) << std::endl;

            // Allocate memory
            AsmEngine::AddressType codeAddress = engine.AllocateMemory("MyCode", 4096);
            std::cout << "Allocated memory at: 0x" << std::hex << codeAddress << std::endl;

            // Write assembly using captured values
            std::stringstream asmCode;
            asmCode << "push rax\n";
            asmCode << "mov rax, [rsp+" << static_cast<int>(s1->AsUInt8()) << "]\n";
            asmCode << "add rax, " << static_cast<int>(s2->AsUInt8()) << "\n";
            asmCode << "pop rax\n";
            asmCode << "ret\n";

            if (engine.WriteAssembly(codeAddress, asmCode.str())) {
                std::cout << "Assembly written successfully!" << std::endl;
            }
        }
    }
}

// Example: Batch operations
void ExampleBatchOperations() {
    std::cout << "\n=== Batch Operations Example ===" << std::endl;

    AsmEngine::AsmEngine engine;  // 使用完整限定名
    engine.AttachToProcess("target.exe");

    // Create batch operations
    std::vector<AsmEngine::AsmEngine::BatchOperation> operations;

    // Scan for pattern
    operations.push_back({
        AsmEngine::AsmEngine::BatchOperation::Scan,
        "module.dll",
        "48 8B 05 ? ? ? ? 48 85 C0",
        0
        });

    // Allocate memory
    operations.push_back({
        AsmEngine::AsmEngine::BatchOperation::Allocate,
        "CodeCave",
        "",
        1024
        });

    // Write assembly
    operations.push_back({
        AsmEngine::AsmEngine::BatchOperation::Write,
        "CodeCave",
        "mov rax, 0x12345678\nret",
        0
        });

    // Create hook
    operations.push_back({
        AsmEngine::AsmEngine::BatchOperation::Hook,
        "TargetFunction",
        "call CodeCave\nret",
        0
        });

    // Execute all operations
    if (engine.ExecuteBatch(operations)) {
        std::cout << "All batch operations completed successfully!" << std::endl;
    }
}

// Example: Error handling
void ExampleErrorHandling() {
    std::cout << "\n=== Error Handling Example ===" << std::endl;

    AsmEngine::AsmEngine engine;  // 使用完整限定名

    // Set custom error handler
    engine.SetErrorHandler([](AsmEngine::ErrorCode code, const std::string& message) {
        std::cerr << "Engine Error [" << static_cast<int>(code) << "]: "
            << message << std::endl;
        });

    // This will trigger an error
    engine.ExecuteScript("invalid script");
}

// Example: Using the Quick namespace for simple operations
void ExampleQuickOperations() {
    std::cout << "\n=== Quick Operations Example ===" << std::endl;

    DWORD pid = 1234; // Replace with actual PID

    // Quick scan
    auto address = AsmEngine::Quick::Scan(pid, "48 89 5C 24 08");
    if (address) {
        std::cout << "Found at: 0x" << std::hex << *address << std::endl;

        // Quick read
        auto data = AsmEngine::Quick::Read(pid, *address, 16);
        if (data) {
            std::cout << "Read data: ";
            for (uint8_t byte : *data) {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(byte) << " ";
            }
            std::cout << std::endl;
        }

        // Quick write
        AsmEngine::ByteVector newData = { 0x90, 0x90, 0x90, 0x90, 0x90 }; // NOPs
        if (AsmEngine::Quick::Write(pid, *address, newData)) {
            std::cout << "Write successful!" << std::endl;
        }
    }
}

// Main function demonstrating various uses
int main() {
    try {
        // Run examples
        ExampleGamePatch();
        ExampleManualOperations();
        ExampleBatchOperations();
        ExampleErrorHandling();
        ExampleQuickOperations();

        std::cout << "\nAll examples completed!" << std::endl;
    }
    catch (const AsmEngine::EngineException& e) {
        std::cerr << "Engine exception: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception& e) {
        std::cerr << "Standard exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

// Advanced example: Complex script with multiple capture groups
void AdvancedExample() {
    const char* complexScript = R"(
// Find dynamic structure with multiple offsets
aobscanmodule(CharacterStruct, game.exe, 48 8B 86 s1.2 00 00 0F 57 ? ? 85 ? 74 ? 41 8B 80 s2.2 00 00)

// Find related offsets
aobscanmodule(DreadOffset, game.exe, 48 8B ? sr1.2 00 00 ? 85 ? 74 ? ? 8B ? sr2.2 00 00)
aobscanmodule(PietyOffset, game.exe, 48 8D ? ? ? 00 00 FF ? ? 48 83 ? sr3.2 00 00 00 75)

alloc(UpdateStats, 2048, CharacterStruct)

UpdateStats:
    push rbx
    push rcx
    
    // Use captured offsets
    mov r8, [rsi+s1]
    test r8, r8
    je .skip
    
    // Update dread using captured offset
    mov rcx, [rsi+sr1]
    test rcx, rcx
    je .skipDread
    mov rbx, 100000
    mov [rcx+sr2], rbx
    
.skipDread:
    // Update piety using captured offset
    lea rcx, [r8+sr3]
    mov rbx, 999999
    mov [rcx], rbx
    
    // Update other stats
    mov ebx, [health]
    test ebx, ebx
    je .skip
    mov [rcx-08], rbx
    mov [rcx-10], rbx
    
.skip:
    pop rcx
    pop rbx
    ret

// Data section
health:
    dd 100

CharacterStruct:
    jmp UpdateStats
    nop
)";
}

// Example: Creating a trainer menu
class SimpleTrainer {
private:
    AsmEngine::AsmEngine engine_;  // 使用完整限定名
    bool attached_ = false;

public:
    void Run() {
        while (true) {
            ShowMenu();

            int choice;
            std::cin >> choice;

            switch (choice) {
            case 1:
                AttachToGame();
                break;
            case 2:
                EnableGodMode();
                break;
            case 3:
                EnableInfiniteAmmo();
                break;
            case 4:
                ShowStats();
                break;
            case 0:
                return;
            default:
                std::cout << "Invalid choice!" << std::endl;
            }
        }
    }

private:
    void ShowMenu() {
        std::cout << "\n=== Simple Game Trainer ===" << std::endl;
        std::cout << "1. Attach to game" << std::endl;
        std::cout << "2. Enable God Mode" << std::endl;
        std::cout << "3. Enable Infinite Ammo" << std::endl;
        std::cout << "4. Show captured values" << std::endl;
        std::cout << "0. Exit" << std::endl;
        std::cout << "Choice: ";
    }

    void AttachToGame() {
        if (engine_.AttachToProcess("game.exe")) {
            attached_ = true;
            std::cout << "Successfully attached to game!" << std::endl;
        }
        else {
            std::cout << "Failed to attach to game!" << std::endl;
        }
    }

    void EnableGodMode() {
        if (!attached_) {
            std::cout << "Not attached to game!" << std::endl;
            return;
        }

        const char* godModeScript = R"(
            aobscanmodule(HealthCheck, game.exe, 48 8B ? ? ? ? ? 48 2B s1.1)
            alloc(GodMode, 128, HealthCheck)
            
            GodMode:
                mov rax, 9999
                ret
            
            HealthCheck:
                jmp GodMode
        )";

        if (engine_.ExecuteScript(godModeScript)) {
            std::cout << "God Mode enabled!" << std::endl;
        }
    }

    void EnableInfiniteAmmo() {
        if (!attached_) {
            std::cout << "Not attached to game!" << std::endl;
            return;
        }

        const char* ammoScript = R"(
            aobscanmodule(AmmoCheck, game.exe, 83 ? s1.1 74 ? 48 8B)
            
            AmmoCheck:
                nop
                nop
                nop
        )";

        if (engine_.ExecuteScript(ammoScript)) {
            std::cout << "Infinite Ammo enabled!" << std::endl;

            // Show captured offset
            auto s1 = engine_.Captures()->Get("s1");
            if (s1) {
                std::cout << "Ammo offset was: 0x"
                    << std::hex << static_cast<int>(s1->AsUInt8())
                    << std::endl;
            }
        }
    }

    void ShowStats() {
        if (!attached_) {
            std::cout << "Not attached to game!" << std::endl;
            return;
        }

        auto captures = engine_.Captures()->ExportAll();
        auto symbols = engine_.Symbols()->GetAllSymbols();

        std::cout << "\nCaptured Values:" << std::endl;
        for (const auto& [name, value] : captures) {
            std::cout << "  " << name << " = " << AsmEngine::BytesToString(value.data)
                << std::endl;
        }

        std::cout << "\nSymbols:" << std::endl;
        for (const auto& symbol : symbols) {
            std::cout << "  " << symbol.name << " @ 0x"
                << std::hex << symbol.address << std::endl;
        }
    }
};