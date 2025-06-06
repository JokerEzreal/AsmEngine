#include "AOBScanner.h"
#include <Psapi.h>
#include <regex>

namespace AsmEngine {

    // ParsedPattern methods
    void ParsedPattern::AddByte(uint8_t value) {
        PatternElement elem;
        elem.type = PatternElementType::Byte;
        elem.value = value;
        elements_.push_back(elem);
        patternLength_++;
    }

    void ParsedPattern::AddWildcard() {
        PatternElement elem;
        elem.type = PatternElementType::Wildcard;
        elem.value = 0;
        elements_.push_back(elem);
        patternLength_++;
    }

    void ParsedPattern::AddCapture(const std::string& name, size_t size) {
        PatternElement elem;
        elem.type = PatternElementType::Capture;
        elem.capture.name = name;
        elem.capture.offset = patternLength_;
        elem.capture.size = size;

        // Add elements for each byte of the capture
        for (size_t i = 0; i < size; ++i) {
            elements_.push_back(elem);
            patternLength_++;
        }

        captures_.push_back(elem.capture);
    }

    // AOBScanner methods
    AOBScanner::AOBScanner(HANDLE processHandle, CaptureStorage* storage)
        : processHandle_(processHandle), captureStorage_(storage) {
    }

    ParsedPattern AOBScanner::ParsePattern(const std::string& pattern) const {
        ParsedPattern parsed;
        std::istringstream stream(pattern);
        std::string token;

        // Regex for capture groups (e.g., s1.2, sr1.4)
        std::regex captureRegex(R"(([a-zA-Z]\w*)\.(\d+))");

        while (stream >> token) {
            std::smatch match;

            if (token == "?" || token == "??") {
                parsed.AddWildcard();
            }
            else if (std::regex_match(token, match, captureRegex)) {
                // Capture group
                std::string captureName = match[1].str();
                size_t captureSize = std::stoul(match[2].str());

                if (captureSize != 1 && captureSize != 2 &&
                    captureSize != 4 && captureSize != 8) {
                    throw EngineException(ErrorCode::InvalidPattern,
                        "Capture size must be 1, 2, 4, or 8 bytes");
                }

                parsed.AddCapture(captureName, captureSize);
            }
            else {
                // Regular byte
                try {
                    uint8_t byte = static_cast<uint8_t>(std::stoul(token, nullptr, 16));
                    parsed.AddByte(byte);
                }
                catch (...) {
                    throw EngineException(ErrorCode::InvalidPattern,
                        "Invalid pattern token: " + token);
                }
            }
        }

        return parsed;
    }

    bool AOBScanner::CompareMemory(const uint8_t* memory, const ParsedPattern& pattern,
        std::unordered_map<std::string, ByteVector>& captures) const {
        const auto& elements = pattern.GetElements();

        for (size_t i = 0; i < elements.size(); ++i) {
            const auto& elem = elements[i];

            switch (elem.type) {
            case PatternElementType::Byte:
                if (memory[i] != elem.value) {
                    return false;
                }
                break;

            case PatternElementType::Wildcard:
                // Always matches
                break;

            case PatternElementType::Capture:
                // Will be extracted if pattern matches
                break;
            }
        }

        // Pattern matched, extract captures
        ExtractCaptures(memory, pattern, captures);
        return true;
    }

    bool AOBScanner::CompareMemorySIMD(const uint8_t* memory, const ParsedPattern& pattern,
        std::unordered_map<std::string, ByteVector>& captures) const {
        // For patterns shorter than 32 bytes, use standard comparison
        if (pattern.GetPatternLength() < 32) {
            return CompareMemory(memory, pattern, captures);
        }

        // Build pattern and mask for SIMD comparison
        alignas(32) uint8_t patternBytes[32] = { 0 };
        alignas(32) uint8_t maskBytes[32] = { 0 };

        const auto& elements = pattern.GetElements();
        size_t simdSize = min(size_t(32), elements.size());

        for (size_t i = 0; i < simdSize; ++i) {
            const auto& elem = elements[i];

            switch (elem.type) {
            case PatternElementType::Byte:
                patternBytes[i] = elem.value;
                maskBytes[i] = 0xFF;
                break;

            case PatternElementType::Wildcard:
            case PatternElementType::Capture:
                patternBytes[i] = 0;
                maskBytes[i] = 0;
                break;
            }
        }

        // Load pattern and mask into AVX registers
        __m256i patternVec = _mm256_load_si256((__m256i*)patternBytes);
        __m256i maskVec = _mm256_load_si256((__m256i*)maskBytes);

        // Load memory and compare
        __m256i memoryVec = _mm256_loadu_si256((__m256i*)memory);
        __m256i maskedMemory = _mm256_and_si256(memoryVec, maskVec);
        __m256i cmpResult = _mm256_cmpeq_epi8(maskedMemory, patternVec);

        // Check if all masked bytes match
        int mask = _mm256_movemask_epi8(cmpResult);

        // For the first 32 bytes, check SIMD result
        uint32_t expectedMask = 0;
        for (size_t i = 0; i < simdSize; ++i) {
            if (maskBytes[i] != 0) {
                expectedMask |= (1 << i);
            }
        }

        if ((mask & expectedMask) != expectedMask) {
            return false;
        }

        // Check remaining bytes with standard comparison
        for (size_t i = 32; i < elements.size(); ++i) {
            const auto& elem = elements[i];

            if (elem.type == PatternElementType::Byte && memory[i] != elem.value) {
                return false;
            }
        }

        // Pattern matched, extract captures
        ExtractCaptures(memory, pattern, captures);
        return true;
    }

    void AOBScanner::ExtractCaptures(const uint8_t* memory, const ParsedPattern& pattern,
        std::unordered_map<std::string, ByteVector>& captures) const {
        for (const auto& capture : pattern.GetCaptures()) {
            ByteVector data(capture.size);
            std::memcpy(data.data(), memory + capture.offset, capture.size);
            captures[capture.name] = std::move(data);
        }
    }

    std::optional<ScanResult> AOBScanner::ScanModule(const std::string& moduleName,
        const std::string& pattern) {
        auto moduleInfo = GetModuleInfo(moduleName);
        if (!moduleInfo) {
            return std::nullopt;
        }

        return ScanRange(moduleInfo->baseAddress,
            moduleInfo->baseAddress + moduleInfo->size,
            pattern);
    }

    std::optional<ScanResult> AOBScanner::ScanRange(AddressType startAddress,
        AddressType endAddress,
        const std::string& pattern) {
        ParsedPattern parsed = ParsePattern(pattern);
        size_t patternLength = parsed.GetPatternLength();

        if (patternLength == 0 || patternLength > (endAddress - startAddress)) {
            return std::nullopt;
        }

        // Allocate buffer for reading memory
        const size_t bufferSize = 64 * 1024; // 64KB chunks
        std::vector<uint8_t> buffer(bufferSize + patternLength);

        AddressType currentAddress = startAddress;

        while (currentAddress < endAddress - patternLength) {
            size_t bytesToRead = min(bufferSize,
                static_cast<size_t>(endAddress - currentAddress));
            SIZE_T bytesRead = 0;

            if (!ReadProcessMemory(processHandle_,
                reinterpret_cast<LPCVOID>(currentAddress),
                buffer.data(), bytesToRead, &bytesRead)) {
                currentAddress += 4096; // Skip inaccessible page
                continue;
            }

            // Scan the buffer
            for (size_t i = 0; i <= bytesRead - patternLength; ++i) {
                std::unordered_map<std::string, ByteVector> captures;

                if (CompareMemorySIMD(buffer.data() + i, parsed, captures)) {
                    // Found match
                    ScanResult result;
                    result.address = currentAddress + i;
                    result.captures = std::move(captures);

                    // Store captures if storage is available
                    if (captureStorage_) {
                        for (const auto& [name, data] : result.captures) {
                            captureStorage_->Store(name, data, result.address, data.size());
                        }
                    }

                    return result;
                }
            }

            // Move to next chunk (with overlap to handle patterns across boundaries)
            currentAddress += bytesRead - patternLength;
        }

        return std::nullopt;
    }

    std::vector<ScanResult> AOBScanner::ScanAll(const std::string& pattern) {
        std::vector<ScanResult> results;
        auto modules = GetAllModules();

        for (const auto& module : modules) {
            auto result = ScanRange(module.baseAddress,
                module.baseAddress + module.size,
                pattern);
            if (result) {
                results.push_back(*result);
            }
        }

        return results;
    }

    std::optional<AOBScanner::ModuleInfo> AOBScanner::GetModuleInfo(
        const std::string& moduleName) const {

        auto modules = GetAllModules();

        for (const auto& module : modules) {
            if (_stricmp(module.name.c_str(), moduleName.c_str()) == 0) {
                return module;
            }
        }

        return std::nullopt;
    }

    std::vector<AOBScanner::ModuleInfo> AOBScanner::GetAllModules() const {
        std::vector<ModuleInfo> modules;

        HMODULE moduleHandles[1024];
        DWORD bytesNeeded;

        if (EnumProcessModules(processHandle_, moduleHandles,
            sizeof(moduleHandles), &bytesNeeded)) {
            size_t moduleCount = bytesNeeded / sizeof(HMODULE);

            for (size_t i = 0; i < moduleCount; ++i) {
                MODULEINFO modInfo;
                char moduleName[MAX_PATH];

                if (GetModuleFileNameExA(processHandle_, moduleHandles[i],
                    moduleName, sizeof(moduleName)) &&
                    GetModuleInformation(processHandle_, moduleHandles[i],
                        &modInfo, sizeof(modInfo))) {

                    ModuleInfo info;

                    // Extract just the filename from the full path
                    std::string fullPath = moduleName;
                    size_t lastSlash = fullPath.find_last_of("\\/");
                    info.name = (lastSlash != std::string::npos)
                        ? fullPath.substr(lastSlash + 1)
                        : fullPath;

                    info.baseAddress = reinterpret_cast<AddressType>(modInfo.lpBaseOfDll);
                    info.size = modInfo.SizeOfImage;

                    modules.push_back(info);
                }
            }
        }

        return modules;
    }

} // namespace AsmEngine