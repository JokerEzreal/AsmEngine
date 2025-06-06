# AsmEngine - Advanced Assembly Execution Engine for x64 Windows

AsmEngine is a powerful C++ library for runtime code manipulation on x64 Windows systems. It provides advanced features beyond traditional memory scanners, including pattern matching with capture groups, dynamic offset handling, and a sophisticated scripting system.

## Key Features

### 🔍 Advanced AOB Scanner with Capture Groups
- **Capture Groups**: Use syntax like `s1.2` to capture 2-byte values during pattern scanning
- **Dynamic References**: Captured values can be referenced in assembly code (e.g., `mov rax, [rsi+s1]`)
- **SIMD Acceleration**: AVX2-optimized scanning achieving 30+ GB/s throughput
- **Module-specific Scanning**: Target specific modules or scan entire process memory

### 💻 Assembly Engine
- **Keystone Integration**: Full x64 assembly support with NASM syntax
- **Symbol Resolution**: Automatic resolution of symbols and captured values
- **Code Generation**: Helper functions for jumps, calls, detours, and hooks
- **Label Support**: Define and reference labels within assembly code

### 📝 Script Parser
- **CE-Compatible Syntax**: Familiar syntax for Cheat Engine users
- **Section Support**: [ENABLE] and [DISABLE] sections for toggle functionality
- **Advanced Commands**: aobscanmodule, alloc, registersymbol, and more
- **Capture Integration**: Seamlessly use captured values in scripts

### 🎯 Memory Management
- **Smart Allocation**: Proximity-based allocation for relative jumps
- **Protection Handling**: Automatic memory protection management
- **Cleanup Tracking**: Automatic cleanup of allocations on exit
- **Region Queries**: Query memory information and find suitable regions

## Quick Start

### Basic Usage

```cpp
#include "AsmEngine.h"

using namespace AsmEngine;

int main() {
    // Create engine instance
    AsmEngine engine;
    
    // Attach to process
    if (!engine.AttachToProcess("game.exe")) {
        std::cerr << "Failed to attach!" << std::endl;
        return 1;
    }
    
    // Scan with capture groups
    auto result = engine.FindPattern("game.exe", 
        "48 8B 05 s1.4 48 83 C0 s2.1");
    
    if (result) {
        // Get captured values
        auto s1 = engine.Captures()->Get("s1");
        auto s2 = engine.Captures()->Get("s2");
        
        std::cout << "Found at: 0x" << std::hex << *result << std::endl;
        std::cout << "Captured offset s1: 0x" << s1->AsUInt32() << std::endl;
        std::cout << "Captured offset s2: 0x" << (int)s2->AsUInt8() << std::endl;
    }
    
    return 0;
}
```

### Script Example

```nasm
[ENABLE]
// Find pattern with dynamic offsets
aobscanmodule(PlayerBase, game.exe, 48 8B 05 s1.4 48 83 C0 s2.1)
alloc(HealthHook, 1024, PlayerBase)

HealthHook:
    mov rax, [rax+s1]    // s1 automatically replaced with captured value
    add rax, s2          // s2 automatically replaced with captured value
    mov dword ptr [rax], 9999
    ret

PlayerBase:
    jmp HealthHook

[DISABLE]
PlayerBase:
    mov rax, [rax+s1]
    add rax, s2

dealloc(HealthHook)
```

## Building

### Requirements
- Windows x64
- Visual Studio 2019 or later (or compatible compiler)
- CMake 3.16+
- Keystone Engine

### Build Steps

1. Clone the repository:
```bash
git clone https://github.com/yourusername/AsmEngine.git
cd AsmEngine
```

2. Download Keystone Engine:
   - Get prebuilt binaries from https://www.keystone-engine.org/
   - Extract to `external/keystone/`

3. Build with CMake:
```bash
mkdir build
cd build
cmake .. -A x64
cmake --build . --config Release
```

4. Run tests:
```bash
ctest -C Release
```

## Advanced Features

### Capture Group Syntax

Capture groups allow you to extract values during pattern scanning:

- `s1.1` - Capture 1 byte as "s1"
- `s1.2` - Capture 2 bytes (word) as "s1"
- `s1.4` - Capture 4 bytes (dword) as "s1"
- `s1.8` - Capture 8 bytes (qword) as "s1"

Multiple captures can have different names: `48 8B s1.1 s2.4 ?? s3.2`

### Symbol Management

```cpp
// Register symbols
engine.Symbols()->RegisterSymbol("PlayerHealth", 0x140001234);
engine.Symbols()->RegisterAllocation("CodeCave", address, size);

// Create aliases
engine.Symbols()->CreateAlias("HP", "PlayerHealth");

// Resolve symbols
auto addr = engine.Symbols()->ResolveAddress("PlayerHealth");

// Persistence
engine.Symbols()->ExportToFile("symbols.dat");
engine.Symbols()->ImportFromFile("symbols.dat");
```

### Error Handling

```cpp
// Set custom error handler
engine.SetErrorHandler([](ErrorCode code, const std::string& msg) {
    std::cerr << "Error [" << (int)code << "]: " << msg << std::endl;
});

// Error codes include:
// - InvalidPattern
// - PatternNotFound
// - MemoryAccessError
// - AssemblyError
// - SymbolNotFound
// - AllocationError
// - ProcessNotFound
```

### Batch Operations

```cpp
std::vector<AsmEngine::BatchOperation> ops = {
    {BatchOperation::Scan, "module.dll", "48 8B 05 ? ? ? ?", 0},
    {BatchOperation::Allocate, "CodeCave", "", 1024},
    {BatchOperation::Write, "CodeCave", "mov rax, 123\nret", 0},
    {BatchOperation::Hook, "TargetFunc", "call CodeCave\nret", 0}
};

engine.ExecuteBatch(ops);
```

## Performance Tips

1. **Use SIMD Scanner**: Patterns ≥32 bytes automatically use AVX2 acceleration
2. **Module-specific Scans**: Always specify module when possible
3. **Batch Operations**: Group multiple operations for better performance
4. **Symbol Caching**: Enable symbol persistence to avoid re-scanning
5. **Proximity Allocation**: Use AllocateNear() for jump targets

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues for bugs and feature requests.

## Acknowledgments

- Keystone Engine for assembly support
- Cheat Engine for inspiration and syntax compatibility
- The reverse engineering community

## Safety Notice

This library is intended for legitimate purposes such as game modding, software debugging, and educational research. Always respect software licenses and terms of service. The authors are not responsible for misuse of this software.
