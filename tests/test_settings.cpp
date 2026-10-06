// org.freedesktop.impl.portal.Settings on a private bus, one thread
// dispatching everything: the defaults state no preference, gdbus reads them
// back (ReadAll with namespace globs, Read, NotFound for a missing key), and
// a host change is visible to readers and announced by SettingChanged. Linux.
#include "check.h"
#include "fixture.h"

using namespace broportal;

int main() {
    bstest::PortalFixture f("test_settings", false);
    if (!bstest::have_gdbus()) bstest::skip("test_settings", "gdbus (the outside client) is not installed");
    std::string err;
    REQUIRE(f.backend->start(&err));

    auto& s = f.backend->settings();
    auto gdbus = [&](const std::string& method, const std::string& args) {
        return bstest::run_while_dispatching(
            f.backend->bus(), "gdbus call --address '" + f.bus.address + "' --dest " + f.config.bus_name +
                                  " --object-path " + f.config.object_path +
                                  " --method org.freedesktop.impl.portal.Settings." + method + " " + args);
    };

    // No invented preferences.
    CHECK_EQ(s.get_setting("org.freedesktop.appearance", "color-scheme").value_or(Variant()).get_value_or<uint32_t>(9),
             0u);
    CHECK(!s.get_setting("org.freedesktop.appearance", "accent-color").has_value());

    auto read = gdbus("Read", "org.freedesktop.appearance color-scheme");
    CHECK_EQ(read.status, 0);
    CHECK(read.out.find("uint32 0") != std::string::npos);

    auto missing = gdbus("Read", "org.freedesktop.appearance accent-color");
    CHECK(missing.status != 0);
    CHECK(missing.out.find("org.freedesktop.portal.Error.NotFound") != std::string::npos);

    // Host settings in two namespaces; ReadAll filters by glob.
    s.set_accent_color(RgbColor{1.0, 0.5, 0.0});
    s.set_setting("org.gnome.desktop.interface", "gtk-theme", Variant(std::string("Adwaita")));
    s.set_setting("org.gnome.desktop.a11y", "always-show-text-caret", Variant(true));

    auto all = gdbus("ReadAll", "\"['org.gnome.desktop.*']\"");
    CHECK_EQ(all.status, 0);
    CHECK(all.out.find("'gtk-theme': <'Adwaita'>") != std::string::npos);
    CHECK(all.out.find("always-show-text-caret") != std::string::npos);
    CHECK(all.out.find("org.freedesktop.appearance") == std::string::npos);

    auto everything = gdbus("ReadAll", "\"@as []\"");
    CHECK_EQ(everything.status, 0);
    CHECK(everything.out.find("'accent-color': <(1.0, 0.5, 0.0)>") != std::string::npos);
    CHECK(everything.out.find("org.gnome.desktop.interface") != std::string::npos);

    auto filtered = s.read_all({"org.gnome.desktop.interface"});
    CHECK_EQ(filtered.size(), 1u);

    // SettingChanged reaches a listener, and the observer runs.
    std::string obs_key;
    s.add_change_observer([&](const std::string&, const std::string& key, const Variant&) { obs_key = key; });
    bool got = false;
    std::string ns, key;
    uint32_t value = 0;
    auto slot = f.client->add_match(
        "type='signal',interface='org.freedesktop.impl.portal.Settings',member='SettingChanged'",
        [&](dbus::Message& m) {
            got = true;
            m.read_string(&ns);
            m.read_string(&key);
            Variant v;
            m.read_variant(&v);
            value = v.get_value_or<uint32_t>(0);
        });
    REQUIRE(slot.is_valid());
    while (f.client->process() > 0) {
    }
    s.set_color_scheme(2);
    for (int i = 0; i < 100 && !got; ++i) {
        while (f.backend->bus().process() > 0) {
        }
        f.client->wait(20000);
        while (f.client->process() > 0) {
        }
    }
    CHECK(got);
    CHECK_EQ(ns, std::string("org.freedesktop.appearance"));
    CHECK_EQ(key, std::string("color-scheme"));
    CHECK_EQ(value, 2u);
    CHECK_EQ(obs_key, std::string("color-scheme"));

    auto after = gdbus("Read", "org.freedesktop.appearance color-scheme");
    CHECK(after.out.find("uint32 2") != std::string::npos);

    return bstest::finish("test_settings");
}
