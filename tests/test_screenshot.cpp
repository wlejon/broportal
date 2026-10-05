#include "broportal/backend.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    using namespace broportal;

    std::string err;
    BackendConfig config;
    config.bus_name = "org.freedesktop.impl.portal.desktop.bro.test_screenshot";
    config.object_path = "/org/freedesktop/portal/desktop";

    auto backend = PortalBackend::create_on_user_bus(config, &err);
    if (!backend) {
        std::cout << "Skipping test_screenshot: user bus not available: " << err << "\n";
        return 77;
    }

    bool ok = backend->start(&err);
    assert(ok);
    assert(backend->run_in_background());

    auto client_bus = dbus::Bus::open_user(&err);
    assert(client_bus);

    // 1. Call Screenshot
    uint32_t resp_code = 999;
    VariantMap results;

    bool called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.Screenshot",
        "Screenshot",
        [](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/sc_shot_1"});
            msg.append_string("org.example.App");
            msg.append_string("");
            VariantMap opts;
            opts["interactive"] = Variant(false);
            msg.append_variant_map(opts);
        },
        [&resp_code, &results](dbus::Message& reply) {
            reply.read_uint32(&resp_code);
            reply.read_variant_map(&results);
        },
        &err);

    assert(called);
    assert(resp_code == 0);
    assert(results.contains("uri"));
    std::string uri = results["uri"].get_value_or<std::string>("");
    assert(!uri.empty());
    assert(uri.starts_with("file://"));
    std::string filepath = uri.substr(7);
    assert(fs::exists(filepath));
    assert(fs::file_size(filepath) > 54); // BMP header size

    // Verify BMP magic header 'BM'
    std::ifstream in(filepath, std::ios::binary);
    char magic[2];
    in.read(magic, 2);
    assert(magic[0] == 'B' && magic[1] == 'M');
    in.close();
    fs::remove(filepath);
    std::cout << "Screenshot verified valid BMP at: " << uri << "\n";

    // 2. Call PickColor
    resp_code = 999;
    results.clear();
    backend->screenshot().set_default_color(RgbColor{0.25, 0.5, 0.75});

    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.Screenshot",
        "PickColor",
        [](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/sc_color_1"});
            msg.append_string("org.example.App");
            msg.append_string("");
            VariantMap opts;
            msg.append_variant_map(opts);
        },
        [&resp_code, &results](dbus::Message& reply) {
            reply.read_uint32(&resp_code);
            reply.read_variant_map(&results);
        },
        &err);

    assert(called);
    assert(resp_code == 0);
    assert(results.contains("color"));
    const auto* color = results["color"].get_if<RgbColor>();
    assert(color != nullptr);
    assert(color->r > 0.24 && color->r < 0.26);
    assert(color->g > 0.49 && color->g < 0.51);
    assert(color->b > 0.74 && color->b < 0.76);
    std::cout << "PickColor returned RGB: (" << color->r << ", " << color->g << ", " << color->b << ")\n";

    backend->stop();
    std::cout << "test_screenshot PASSED\n";
    return 0;
}
