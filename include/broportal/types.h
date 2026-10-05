#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace broportal {

// Standard portal numeric response codes
enum class ResponseCode : uint32_t {
    Success = 0,
    Cancelled = 1,
    OtherError = 2
};

struct ObjectPath {
    std::string path;

    bool operator==(const ObjectPath& other) const = default;
    auto operator<=>(const ObjectPath& other) const = default;
};

struct UnixFd {
    int fd = -1;

    bool operator==(const UnixFd& other) const = default;
};

struct RgbColor {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    bool operator==(const RgbColor& other) const = default;
};

struct Coord2D {
    int32_t x = 0;
    int32_t y = 0;

    bool operator==(const Coord2D& other) const = default;
};

// Forward declaration of recursive Variant container
class Variant;
using VariantMap = std::map<std::string, Variant>;
using StringPairList = std::vector<std::pair<std::string, std::string>>;
using StreamDescriptor = std::pair<uint32_t, VariantMap>; // (node_id, properties)
using StreamList = std::vector<StreamDescriptor>;         // a(ua{sv})
using ShortcutDescriptor = std::pair<std::string, VariantMap>; // (shortcut_id, properties)
using ShortcutList = std::vector<ShortcutDescriptor>;         // a(sa{sv})
using SettingsMap = std::map<std::string, VariantMap>;        // a{sa{sv}}

using VariantBase = std::variant<
    std::monostate,
    bool,
    uint8_t,
    int16_t,
    uint16_t,
    int32_t,
    uint32_t,
    int64_t,
    uint64_t,
    double,
    std::string,
    ObjectPath,
    UnixFd,
    std::vector<std::string>,
    std::vector<uint8_t>,
    RgbColor,
    Coord2D,
    StringPairList,
    std::shared_ptr<VariantMap>,
    std::shared_ptr<SettingsMap>,
    std::shared_ptr<StreamList>,
    std::shared_ptr<ShortcutList>
>;

class Variant {
public:
    Variant() : data_(std::monostate{}) {}

    template <typename T>
        requires(!std::is_same_v<std::decay_t<T>, Variant> &&
                 !std::is_same_v<std::decay_t<T>, VariantMap> &&
                 !std::is_same_v<std::decay_t<T>, SettingsMap> &&
                 !std::is_same_v<std::decay_t<T>, StreamList> &&
                 !std::is_same_v<std::decay_t<T>, ShortcutList>)
    Variant(T&& value) : data_(std::forward<T>(value)) {}

    // Convenience constructor for const char* -> std::string
    Variant(const char* str) : data_(std::string(str ? str : "")) {}

    // Deep constructors for nested container maps/lists
    Variant(const VariantMap& map)
        : data_(std::make_shared<VariantMap>(map)) {}
    Variant(VariantMap&& map)
        : data_(std::make_shared<VariantMap>(std::move(map))) {}

    Variant(const SettingsMap& map)
        : data_(std::make_shared<SettingsMap>(map)) {}
    Variant(SettingsMap&& map)
        : data_(std::make_shared<SettingsMap>(std::move(map))) {}

    Variant(const StreamList& list)
        : data_(std::make_shared<StreamList>(list)) {}
    Variant(StreamList&& list)
        : data_(std::make_shared<StreamList>(std::move(list))) {}

    Variant(const ShortcutList& list)
        : data_(std::make_shared<ShortcutList>(list)) {}
    Variant(ShortcutList&& list)
        : data_(std::make_shared<ShortcutList>(std::move(list))) {}

    bool is_null() const noexcept {
        return std::holds_alternative<std::monostate>(data_);
    }

    template <typename T>
    bool holds() const noexcept {
        if constexpr (std::is_same_v<T, VariantMap>) {
            return std::holds_alternative<std::shared_ptr<VariantMap>>(data_);
        } else if constexpr (std::is_same_v<T, SettingsMap>) {
            return std::holds_alternative<std::shared_ptr<SettingsMap>>(data_);
        } else if constexpr (std::is_same_v<T, StreamList>) {
            return std::holds_alternative<std::shared_ptr<StreamList>>(data_);
        } else if constexpr (std::is_same_v<T, ShortcutList>) {
            return std::holds_alternative<std::shared_ptr<ShortcutList>>(data_);
        } else {
            return std::holds_alternative<T>(data_);
        }
    }

    template <typename T>
    const T* get_if() const noexcept {
        if constexpr (std::is_same_v<T, VariantMap>) {
            auto ptr = std::get_if<std::shared_ptr<VariantMap>>(&data_);
            return ptr && *ptr ? ptr->get() : nullptr;
        } else if constexpr (std::is_same_v<T, SettingsMap>) {
            auto ptr = std::get_if<std::shared_ptr<SettingsMap>>(&data_);
            return ptr && *ptr ? ptr->get() : nullptr;
        } else if constexpr (std::is_same_v<T, StreamList>) {
            auto ptr = std::get_if<std::shared_ptr<StreamList>>(&data_);
            return ptr && *ptr ? ptr->get() : nullptr;
        } else if constexpr (std::is_same_v<T, ShortcutList>) {
            auto ptr = std::get_if<std::shared_ptr<ShortcutList>>(&data_);
            return ptr && *ptr ? ptr->get() : nullptr;
        } else {
            return std::get_if<T>(&data_);
        }
    }

    template <typename T>
    T get_value_or(T default_value) const {
        const T* val = get_if<T>();
        return val ? *val : default_value;
    }

    const VariantBase& raw() const noexcept { return data_; }

    std::string signature() const;

    bool operator==(const Variant& other) const;

private:
    VariantBase data_;
};

// Helper utilities for VariantMap
inline std::optional<std::string> get_string(const VariantMap& map, const std::string& key) {
    auto it = map.find(key);
    if (it != map.end()) {
        if (const auto* s = it->second.get_if<std::string>()) {
            return *s;
        }
    }
    return std::nullopt;
}

inline bool get_bool_or(const VariantMap& map, const std::string& key, bool default_val) {
    auto it = map.find(key);
    if (it != map.end()) {
        return it->second.get_value_or<bool>(default_val);
    }
    return default_val;
}

inline uint32_t get_uint32_or(const VariantMap& map, const std::string& key, uint32_t default_val) {
    auto it = map.find(key);
    if (it != map.end()) {
        return it->second.get_value_or<uint32_t>(default_val);
    }
    return default_val;
}

} // namespace broportal
