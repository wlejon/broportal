#include "broportal/types.h"

namespace broportal {

std::string Variant::signature() const {
    return std::visit(
        [](const auto& val) -> std::string {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return "v";
            } else if constexpr (std::is_same_v<T, bool>) {
                return "b";
            } else if constexpr (std::is_same_v<T, uint8_t>) {
                return "y";
            } else if constexpr (std::is_same_v<T, int16_t>) {
                return "n";
            } else if constexpr (std::is_same_v<T, uint16_t>) {
                return "q";
            } else if constexpr (std::is_same_v<T, int32_t>) {
                return "i";
            } else if constexpr (std::is_same_v<T, uint32_t>) {
                return "u";
            } else if constexpr (std::is_same_v<T, int64_t>) {
                return "x";
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                return "t";
            } else if constexpr (std::is_same_v<T, double>) {
                return "d";
            } else if constexpr (std::is_same_v<T, std::string>) {
                return "s";
            } else if constexpr (std::is_same_v<T, ObjectPath>) {
                return "o";
            } else if constexpr (std::is_same_v<T, UnixFd>) {
                return "h";
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                return "as";
            } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                return "ay";
            } else if constexpr (std::is_same_v<T, RgbColor>) {
                return "(ddd)";
            } else if constexpr (std::is_same_v<T, Coord2D>) {
                return "(ii)";
            } else if constexpr (std::is_same_v<T, StringPairList>) {
                return "a(ss)";
            } else if constexpr (std::is_same_v<T, std::shared_ptr<VariantMap>>) {
                return "a{sv}";
            } else if constexpr (std::is_same_v<T, std::shared_ptr<SettingsMap>>) {
                return "a{sa{sv}}";
            } else if constexpr (std::is_same_v<T, std::shared_ptr<StreamList>>) {
                return "a(ua{sv})";
            } else if constexpr (std::is_same_v<T, std::shared_ptr<ShortcutList>>) {
                return "a(sa{sv})";
            } else {
                return "v";
            }
        },
        data_);
}

bool Variant::operator==(const Variant& other) const {
    if (data_.index() != other.data_.index()) {
        return false;
    }

    return std::visit(
        [&other](const auto& val) -> bool {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return true;
            } else if constexpr (std::is_same_v<T, std::shared_ptr<VariantMap>>) {
                auto o = std::get<std::shared_ptr<VariantMap>>(other.data_);
                if (!val && !o) return true;
                if (!val || !o) return false;
                return *val == *o;
            } else if constexpr (std::is_same_v<T, std::shared_ptr<SettingsMap>>) {
                auto o = std::get<std::shared_ptr<SettingsMap>>(other.data_);
                if (!val && !o) return true;
                if (!val || !o) return false;
                return *val == *o;
            } else if constexpr (std::is_same_v<T, std::shared_ptr<StreamList>>) {
                auto o = std::get<std::shared_ptr<StreamList>>(other.data_);
                if (!val && !o) return true;
                if (!val || !o) return false;
                return *val == *o;
            } else if constexpr (std::is_same_v<T, std::shared_ptr<ShortcutList>>) {
                auto o = std::get<std::shared_ptr<ShortcutList>>(other.data_);
                if (!val && !o) return true;
                if (!val || !o) return false;
                return *val == *o;
            } else {
                return val == std::get<T>(other.data_);
            }
        },
        data_);
}

} // namespace broportal
