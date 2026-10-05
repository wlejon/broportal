#include "broportal/backend.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace broportal;

    std::string err;
    BackendConfig config;
    config.bus_name = "org.freedesktop.impl.portal.desktop.bro.test_settings";
    config.object_path = "/org/freedesktop/portal/desktop";

    auto backend = PortalBackend::create_on_user_bus(config, &err);
    if (!backend) {
        std::cout << "Skipping test_settings: user bus not available: " << err << "\n";
        return 77;
    }

    bool ok = backend->start(&err);
    assert(ok);
    assert(backend->run_in_background());

    auto client_bus = dbus::Bus::open_user(&err);
    assert(client_bus);

    // 1. ReadAll
    SettingsMap all_settings;
    bool called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.Settings",
        "ReadAll",
        [](dbus::Message& msg) {
            msg.append_string_list({});
        },
        [&all_settings](dbus::Message& reply) {
            reply.read_settings_map(&all_settings);
        },
        &err);

    assert(called);
    assert(all_settings.contains("org.freedesktop.appearance"));
    const auto& app_settings = all_settings["org.freedesktop.appearance"];
    assert(app_settings.contains("color-scheme"));
    uint32_t scheme = app_settings.at("color-scheme").get_value_or<uint32_t>(999);
    assert(scheme == 1); // 1 = dark mode default
    std::cout << "ReadAll color-scheme: " << scheme << "\n";

    // 2. Read single key
    Variant read_val;
    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.Settings",
        "Read",
        [](dbus::Message& msg) {
            msg.append_string("org.freedesktop.appearance");
            msg.append_string("color-scheme");
        },
        [&read_val](dbus::Message& reply) {
            reply.read_variant(&read_val);
        },
        &err);

    assert(called);
    assert(read_val.get_value_or<uint32_t>(0) == 1);

    // 3. Listen for SettingChanged signal
    bool signal_received = false;
    std::string sig_ns, sig_key;
    uint32_t sig_val = 0;

    auto slot = client_bus->add_match(
        "type='signal',interface='org.freedesktop.impl.portal.Settings',member='SettingChanged'",
        [&signal_received, &sig_ns, &sig_key, &sig_val](dbus::Message& msg) {
            signal_received = true;
            msg.read_string(&sig_ns);
            msg.read_string(&sig_key);
            Variant v;
            msg.read_variant(&v);
            sig_val = v.get_value_or<uint32_t>(0);
        });

    assert(slot.is_valid());

    // Update setting to light mode (2)
    backend->settings().set_color_scheme(2);

    for (int i = 0; i < 30; ++i) {
        client_bus->process();
        if (signal_received) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    assert(signal_received);
    assert(sig_ns == "org.freedesktop.appearance");
    assert(sig_key == "color-scheme");
    assert(sig_val == 2);
    std::cout << "SettingChanged signal verified: " << sig_ns << "." << sig_key << " = " << sig_val << "\n";

    backend->stop();
    std::cout << "test_settings PASSED\n";
    return 0;
}
