#include "SymbolManager.h"
#include <fstream>
#include <iostream>

namespace AsmEngine {

    SymbolManager::SymbolManager() {
        // Register some common symbols
        RegisterSymbol("nullptr", 0, SymbolType::Address, true);
    }

    void SymbolManager::RegisterSymbol(const std::string& name, const Symbol& symbol) {
        std::unique_lock lock(mutex_);

        if (symbol.isGlobal) {
            globalSymbols_[name] = symbol;
        }
        else {
            localSymbols_[name] = symbol;
        }
    }

    void SymbolManager::RegisterSymbol(const std::string& name, AddressType address,
        SymbolType type, bool isGlobal) {
        Symbol symbol;
        symbol.name = name;
        symbol.address = address;
        symbol.type = type;
        symbol.isGlobal = isGlobal;
        symbol.size = 0;

        RegisterSymbol(name, symbol);
    }

    void SymbolManager::RegisterAllocation(const std::string& name, AddressType address,
        size_t size) {
        Symbol symbol;
        symbol.name = name;
        symbol.address = address;
        symbol.type = SymbolType::Allocated;
        symbol.size = size;
        symbol.isGlobal = true;

        RegisterSymbol(name, symbol);
    }

    void SymbolManager::RegisterLabel(const std::string& name, AddressType address) {
        RegisterSymbol(name, address, SymbolType::Label, false);
    }

    void SymbolManager::UnregisterSymbol(const std::string& name) {
        std::unique_lock lock(mutex_);

        globalSymbols_.erase(name);
        localSymbols_.erase(name);
        aliases_.erase(name);
    }

    std::optional<AddressType> SymbolManager::ResolveAddress(const std::string& name) const {
        auto symbol = ResolveSymbol(name);
        if (symbol) {
            return symbol->address;
        }

        // Try to parse as hex address
        if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
            try {
                return std::stoull(name, nullptr, 16);
            }
            catch (...) {
                // Not a valid hex number
            }
        }

        // Try to parse as decimal
        try {
            return std::stoull(name, nullptr, 10);
        }
        catch (...) {
            // Not a valid number
        }

        return std::nullopt;
    }

    std::optional<Symbol> SymbolManager::ResolveSymbol(const std::string& name) const {
        std::shared_lock lock(mutex_);

        // Check for alias first
        std::string resolvedName = name;
        auto aliasIt = aliases_.find(name);
        if (aliasIt != aliases_.end()) {
            resolvedName = aliasIt->second;
        }

        // Check local symbols first (higher priority)
        auto localIt = localSymbols_.find(resolvedName);
        if (localIt != localSymbols_.end()) {
            return localIt->second;
        }

        // Then check global symbols
        auto globalIt = globalSymbols_.find(resolvedName);
        if (globalIt != globalSymbols_.end()) {
            return globalIt->second;
        }

        return std::nullopt;
    }

    bool SymbolManager::Exists(const std::string& name) const {
        std::shared_lock lock(mutex_);

        return localSymbols_.find(name) != localSymbols_.end() ||
            globalSymbols_.find(name) != globalSymbols_.end() ||
            aliases_.find(name) != aliases_.end();
    }

    void SymbolManager::CreateAlias(const std::string& alias, const std::string& target) {
        std::unique_lock lock(mutex_);
        aliases_[alias] = target;
    }

    void SymbolManager::ClearLocalSymbols() {
        std::unique_lock lock(mutex_);
        localSymbols_.clear();
    }

    void SymbolManager::ClearAll() {
        std::unique_lock lock(mutex_);
        localSymbols_.clear();
        globalSymbols_.clear();
        aliases_.clear();
    }

    std::vector<Symbol> SymbolManager::GetAllSymbols() const {
        std::shared_lock lock(mutex_);

        std::vector<Symbol> symbols;
        symbols.reserve(localSymbols_.size() + globalSymbols_.size());

        for (const auto& [name, symbol] : globalSymbols_) {
            symbols.push_back(symbol);
        }

        for (const auto& [name, symbol] : localSymbols_) {
            symbols.push_back(symbol);
        }

        return symbols;
    }

    std::vector<Symbol> SymbolManager::GetGlobalSymbols() const {
        std::shared_lock lock(mutex_);

        std::vector<Symbol> symbols;
        symbols.reserve(globalSymbols_.size());

        for (const auto& [name, symbol] : globalSymbols_) {
            symbols.push_back(symbol);
        }

        return symbols;
    }

    std::vector<Symbol> SymbolManager::GetLocalSymbols() const {
        std::shared_lock lock(mutex_);

        std::vector<Symbol> symbols;
        symbols.reserve(localSymbols_.size());

        for (const auto& [name, symbol] : localSymbols_) {
            symbols.push_back(symbol);
        }

        return symbols;
    }

    void SymbolManager::ExportToFile(const std::string& filename) const {
        std::shared_lock lock(mutex_);

        std::ofstream file(filename, std::ios::binary);
        if (!file) {
            throw EngineException(ErrorCode::InvalidParameter,
                "Failed to open file for writing: " + filename);
        }

        // Write header
        const char* magic = "ASMSYM01";
        file.write(magic, 8);

        // Write global symbols count
        size_t globalCount = globalSymbols_.size();
        file.write(reinterpret_cast<const char*>(&globalCount), sizeof(globalCount));

        // Write global symbols
        for (const auto& [name, symbol] : globalSymbols_) {
            // Write name length and name
            size_t nameLen = name.length();
            file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
            file.write(name.c_str(), nameLen);

            // Write symbol data
            file.write(reinterpret_cast<const char*>(&symbol.type), sizeof(symbol.type));
            file.write(reinterpret_cast<const char*>(&symbol.address), sizeof(symbol.address));
            file.write(reinterpret_cast<const char*>(&symbol.size), sizeof(symbol.size));

            // Write data size and data
            size_t dataSize = symbol.data.size();
            file.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
            if (dataSize > 0) {
                file.write(reinterpret_cast<const char*>(symbol.data.data()), dataSize);
            }
        }

        // Write aliases count
        size_t aliasCount = aliases_.size();
        file.write(reinterpret_cast<const char*>(&aliasCount), sizeof(aliasCount));

        // Write aliases
        for (const auto& [alias, target] : aliases_) {
            // Write alias length and alias
            size_t aliasLen = alias.length();
            file.write(reinterpret_cast<const char*>(&aliasLen), sizeof(aliasLen));
            file.write(alias.c_str(), aliasLen);

            // Write target length and target
            size_t targetLen = target.length();
            file.write(reinterpret_cast<const char*>(&targetLen), sizeof(targetLen));
            file.write(target.c_str(), targetLen);
        }
    }

    void SymbolManager::ImportFromFile(const std::string& filename) {
        std::unique_lock lock(mutex_);

        std::ifstream file(filename, std::ios::binary);
        if (!file) {
            throw EngineException(ErrorCode::InvalidParameter,
                "Failed to open file for reading: " + filename);
        }

        // Read and verify header
        char magic[8];
        file.read(magic, 8);
        if (std::memcmp(magic, "ASMSYM01", 8) != 0) {
            throw EngineException(ErrorCode::InvalidParameter,
                "Invalid symbol file format");
        }

        // Clear existing globals (keep locals)
        globalSymbols_.clear();
        aliases_.clear();

        // Read global symbols count
        size_t globalCount;
        file.read(reinterpret_cast<char*>(&globalCount), sizeof(globalCount));

        // Read global symbols
        for (size_t i = 0; i < globalCount; ++i) {
            // Read name
            size_t nameLen;
            file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));

            std::string name(nameLen, '\0');
            file.read(&name[0], nameLen);

            Symbol symbol;
            symbol.name = name;
            symbol.isGlobal = true;

            // Read symbol data
            file.read(reinterpret_cast<char*>(&symbol.type), sizeof(symbol.type));
            file.read(reinterpret_cast<char*>(&symbol.address), sizeof(symbol.address));
            file.read(reinterpret_cast<char*>(&symbol.size), sizeof(symbol.size));

            // Read data
            size_t dataSize;
            file.read(reinterpret_cast<char*>(&dataSize), sizeof(dataSize));
            if (dataSize > 0) {
                symbol.data.resize(dataSize);
                file.read(reinterpret_cast<char*>(symbol.data.data()), dataSize);
            }

            globalSymbols_[name] = symbol;
        }

        // Read aliases count
        size_t aliasCount;
        file.read(reinterpret_cast<char*>(&aliasCount), sizeof(aliasCount));

        // Read aliases
        for (size_t i = 0; i < aliasCount; ++i) {
            // Read alias
            size_t aliasLen;
            file.read(reinterpret_cast<char*>(&aliasLen), sizeof(aliasLen));

            std::string alias(aliasLen, '\0');
            file.read(&alias[0], aliasLen);

            // Read target
            size_t targetLen;
            file.read(reinterpret_cast<char*>(&targetLen), sizeof(targetLen));

            std::string target(targetLen, '\0');
            file.read(&target[0], targetLen);

            aliases_[alias] = target;
        }
    }

} // namespace AsmEngine