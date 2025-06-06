#pragma once

#include "Common.h"

namespace AsmEngine {

    // Symbol types
    enum class SymbolType {
        Address,        // Direct memory address
        Label,          // Label in assembled code
        Capture,        // Captured value from AOB scan
        Offset,         // Offset value
        Allocated,      // Allocated memory region
        External        // External/imported symbol
    };

    // Symbol information
    struct Symbol {
        std::string name;
        SymbolType type;
        AddressType address;
        size_t size;
        ByteVector data;        // For captured values
        bool isGlobal;          // Global vs local scope

        Symbol() : type(SymbolType::Address), address(0), size(0), isGlobal(false) {}
    };

    class SymbolManager {
    private:
        mutable std::shared_mutex mutex_;

        // Symbol tables
        std::unordered_map<std::string, Symbol> globalSymbols_;
        std::unordered_map<std::string, Symbol> localSymbols_;

        // Symbol aliases
        std::unordered_map<std::string, std::string> aliases_;

    public:
        SymbolManager();
        ~SymbolManager() = default;

        // Register a symbol
        void RegisterSymbol(const std::string& name, const Symbol& symbol);
        void RegisterSymbol(const std::string& name, AddressType address,
            SymbolType type = SymbolType::Address, bool isGlobal = true);

        // Register allocated memory
        void RegisterAllocation(const std::string& name, AddressType address, size_t size);

        // Register a label
        void RegisterLabel(const std::string& name, AddressType address);

        // Unregister a symbol
        void UnregisterSymbol(const std::string& name);

        // Resolve a symbol to its address
        std::optional<AddressType> ResolveAddress(const std::string& name) const;

        // Resolve a symbol to full information
        std::optional<Symbol> ResolveSymbol(const std::string& name) const;

        // Check if symbol exists
        bool Exists(const std::string& name) const;

        // Create an alias
        void CreateAlias(const std::string& alias, const std::string& target);

        // Clear local symbols (between script executions)
        void ClearLocalSymbols();

        // Clear all symbols
        void ClearAll();

        // Export symbols
        std::vector<Symbol> GetAllSymbols() const;
        std::vector<Symbol> GetGlobalSymbols() const;
        std::vector<Symbol> GetLocalSymbols() const;

        // Import/Export for persistence
        void ExportToFile(const std::string& filename) const;
        void ImportFromFile(const std::string& filename);
    };

} // namespace AsmEngine