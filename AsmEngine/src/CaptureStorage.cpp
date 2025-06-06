#include "CaptureStorage.h"
#include <cstring>

namespace AsmEngine {

    // CapturedValue methods
    uint8_t CapturedValue::AsUInt8() const {
        if (data.empty()) return 0;
        return data[0];
    }

    uint16_t CapturedValue::AsUInt16() const {
        if (data.size() < 2) return 0;
        return *reinterpret_cast<const uint16_t*>(data.data());
    }

    uint32_t CapturedValue::AsUInt32() const {
        if (data.size() < 4) return 0;
        return *reinterpret_cast<const uint32_t*>(data.data());
    }

    uint64_t CapturedValue::AsUInt64() const {
        if (data.size() < 8) return 0;
        return *reinterpret_cast<const uint64_t*>(data.data());
    }

    int8_t CapturedValue::AsInt8() const {
        return static_cast<int8_t>(AsUInt8());
    }

    int16_t CapturedValue::AsInt16() const {
        return static_cast<int16_t>(AsUInt16());
    }

    int32_t CapturedValue::AsInt32() const {
        return static_cast<int32_t>(AsUInt32());
    }

    int64_t CapturedValue::AsInt64() const {
        return static_cast<int64_t>(AsUInt64());
    }

    // CaptureStorage methods
    void CaptureStorage::Store(const std::string& name, const ByteVector& data,
        AddressType address, size_t size) {
        std::unique_lock lock(mutex_);

        CapturedValue capture;
        capture.name = name;
        capture.data = data;
        capture.address = address;
        capture.size = size;
        capture.captureTime = std::chrono::system_clock::now();

        captures_[name] = std::move(capture);
    }

    std::optional<CapturedValue> CaptureStorage::Get(const std::string& name) const {
        std::shared_lock lock(mutex_);

        auto it = captures_.find(name);
        if (it != captures_.end()) {
            return it->second;
        }

        return std::nullopt;
    }

    bool CaptureStorage::Exists(const std::string& name) const {
        std::shared_lock lock(mutex_);
        return captures_.find(name) != captures_.end();
    }

    void CaptureStorage::Remove(const std::string& name) {
        std::unique_lock lock(mutex_);
        captures_.erase(name);
    }

    void CaptureStorage::Clear() {
        std::unique_lock lock(mutex_);
        captures_.clear();
    }

    std::vector<std::string> CaptureStorage::GetAllNames() const {
        std::shared_lock lock(mutex_);

        std::vector<std::string> names;
        names.reserve(captures_.size());

        for (const auto& [name, _] : captures_) {
            names.push_back(name);
        }

        return names;
    }

    std::string CaptureStorage::ResolveReference(const std::string& name) const {
        auto capture = Get(name);
        if (!capture) {
            return name; // Return original if not found
        }

        // Convert to appropriate format based on size
        switch (capture->size) {
        case 1:
            return std::to_string(capture->AsUInt8());
        case 2:
            return std::to_string(capture->AsUInt16());
        case 4:
            return std::to_string(capture->AsUInt32());
        case 8:
            return std::to_string(capture->AsUInt64());
        default:
            // Return as hex string for non-standard sizes
            return "0x" + BytesToString(capture->data);
        }
    }

    std::unordered_map<std::string, CapturedValue> CaptureStorage::ExportAll() const {
        std::shared_lock lock(mutex_);
        return captures_;
    }

} // namespace AsmEngine