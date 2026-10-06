#include "broportal/settings.h"
#include "broportal/backend.h"

#include <iostream>

namespace broportal {

const sd_bus_vtable SettingsInterface::vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("ReadAll", "as", "a{sa{sv}}", &SettingsInterface::dbus_read_all, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Read", "ss", "v", &SettingsInterface::dbus_read, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("SettingChanged", "ssv", 0),
    SD_BUS_PROPERTY("version", "u", &SettingsInterface::dbus_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

SettingsInterface::SettingsInterface(PortalBackend& backend)
    : backend_(backend) {
    init_defaults();
}

SettingsInterface::~SettingsInterface() = default;

void SettingsInterface::init_defaults() {
    // Until the host says otherwise, the user has expressed no preference:
    // color-scheme 0 (none), normal contrast and motion, and no accent color
    // (the key is absent, as the spec allows, rather than an invented one).
    set_setting("org.freedesktop.appearance", "color-scheme", Variant(static_cast<uint32_t>(0)));
    set_setting("org.freedesktop.appearance", "contrast", Variant(static_cast<uint32_t>(0)));
    set_setting("org.freedesktop.appearance", "reduced-motion", Variant(static_cast<uint32_t>(0)));
}

bool SettingsInterface::matches_namespace(const std::string& pattern, const std::string& ns) {
    if (pattern.empty() || pattern == "*") return true;
    if (pattern.ends_with(".*")) {
        std::string prefix = pattern.substr(0, pattern.size() - 2);
        return ns == prefix || ns.starts_with(prefix + ".");
    }
    return pattern == ns;
}

void SettingsInterface::set_setting(
    const std::string& ns,
    const std::string& key,
    const Variant& value) {
    std::vector<SettingChangeObserver> observers_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_[ns][key] = value;
        observers_copy = observers_;
    }

    backend_.emit_signal(
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.Settings",
        "SettingChanged",
        [&ns, &key, &value](dbus::Message& msg) {
            msg.append_string(ns);
            msg.append_string(key);
            msg.append_variant(value);
        });

    for (const auto& obs : observers_copy) {
        if (obs) {
            obs(ns, key, value);
        }
    }
}

std::optional<Variant> SettingsInterface::get_setting(
    const std::string& ns,
    const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it_ns = settings_.find(ns);
    if (it_ns != settings_.end()) {
        auto it_key = it_ns->second.find(key);
        if (it_key != it_ns->second.end()) {
            return it_key->second;
        }
    }
    return std::nullopt;
}

SettingsMap SettingsInterface::read_all(const std::vector<std::string>& namespaces) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (namespaces.empty()) {
        return settings_;
    }

    for (const auto& pattern : namespaces) {
        if (pattern.empty()) {
            return settings_;
        }
    }

    SettingsMap result;
    for (const auto& [ns, dict] : settings_) {
        bool match = false;
        for (const auto& pattern : namespaces) {
            if (matches_namespace(pattern, ns)) {
                match = true;
                break;
            }
        }
        if (match) {
            result[ns] = dict;
        }
    }
    return result;
}

void SettingsInterface::add_change_observer(SettingChangeObserver observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.push_back(std::move(observer));
}

void SettingsInterface::set_color_scheme(uint32_t scheme) {
    set_setting("org.freedesktop.appearance", "color-scheme", Variant(scheme));
}

void SettingsInterface::set_accent_color(const RgbColor& color) {
    set_setting("org.freedesktop.appearance", "accent-color", Variant(color));
}

void SettingsInterface::set_contrast(uint32_t contrast) {
    set_setting("org.freedesktop.appearance", "contrast", Variant(contrast));
}

void SettingsInterface::set_reduced_motion(uint32_t motion) {
    set_setting("org.freedesktop.appearance", "reduced-motion", Variant(motion));
}

int SettingsInterface::dbus_read_all(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<SettingsInterface*>(userdata);
    dbus::Message msg(m, false);

    std::vector<std::string> namespaces;
    if (!msg.read_string_list(&namespaces)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    SettingsMap filtered = self->read_all(namespaces);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_settings_map(filtered);

    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

int SettingsInterface::dbus_read(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<SettingsInterface*>(userdata);
    dbus::Message msg(m, false);

    std::string ns;
    std::string key;
    if (!msg.read_string(&ns) || !msg.read_string(&key)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto val = self->get_setting(ns, key);
    if (!val.has_value()) {
        return sd_bus_reply_method_errorf(m, "org.freedesktop.portal.Error.NotFound", "Requested setting not found");
    }

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_variant(*val);

    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

int SettingsInterface::dbus_get_property(
    sd_bus* /*bus*/,
    const char* /*path*/,
    const char* /*interface*/,
    const char* property,
    sd_bus_message* reply,
    void* /*userdata*/,
    sd_bus_error* /*ret_error*/) {
    if (std::string(property) == "version") {
        uint32_t val = 1;
        return sd_bus_message_append_basic(reply, 'u', &val);
    }
    return -EINVAL;
}

} // namespace broportal
