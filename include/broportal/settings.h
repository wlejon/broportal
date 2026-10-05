#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/types.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace broportal {

class PortalBackend;

using SettingChangeObserver = std::function<void(
    const std::string& ns,
    const std::string& key,
    const Variant& value)>;

class SettingsInterface {
public:
    explicit SettingsInterface(PortalBackend& backend);
    ~SettingsInterface();

    void set_setting(const std::string& ns, const std::string& key, const Variant& value);
    std::optional<Variant> get_setting(const std::string& ns, const std::string& key) const;
    SettingsMap read_all(const std::vector<std::string>& namespaces) const;

    void add_change_observer(SettingChangeObserver observer);

    // Standard appearance shortcuts
    void set_color_scheme(uint32_t scheme); // 0 = none, 1 = dark, 2 = light
    void set_accent_color(const RgbColor& color);
    void set_contrast(uint32_t contrast);
    void set_reduced_motion(uint32_t motion);

    // D-Bus method and property handlers
    static int dbus_read_all(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_read(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_get_property(
        sd_bus* bus,
        const char* path,
        const char* interface,
        const char* property,
        sd_bus_message* reply,
        void* userdata,
        sd_bus_error* ret_error);

    static const sd_bus_vtable vtable[];

private:
    PortalBackend& backend_;
    mutable std::mutex mutex_;
    SettingsMap settings_;
    std::vector<SettingChangeObserver> observers_;

    void init_defaults();
    static bool matches_namespace(const std::string& pattern, const std::string& ns);
};

} // namespace broportal
