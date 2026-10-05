#include "broportal/backend.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace broportal;

    std::string err;
    BackendConfig config;
    config.bus_name = "org.freedesktop.impl.portal.desktop.bro.test_fc";
    config.object_path = "/org/freedesktop/portal/desktop";

    auto backend = PortalBackend::create_on_user_bus(config, &err);
    if (!backend) {
        std::cout << "Skipping test_filechooser: user bus not available: " << err << "\n";
        return 77;
    }

    backend->file_chooser().set_default_selected_files({"/home/j/projects/broportal/README.md"});

    bool ok = backend->start(&err);
    assert(ok);
    assert(backend->run_in_background());

    auto client_bus = dbus::Bus::open_user(&err);
    assert(client_bus);

    // 1. Call OpenFile
    uint32_t resp_code = 999;
    VariantMap results;

    bool called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.FileChooser",
        "OpenFile",
        [](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/fc_open_1"});
            msg.append_string("org.example.App");
            msg.append_string("");
            msg.append_string("Open File Dialog");
            VariantMap opts;
            opts["multiple"] = Variant(false);
            msg.append_variant_map(opts);
        },
        [&resp_code, &results](dbus::Message& reply) {
            reply.read_uint32(&resp_code);
            reply.read_variant_map(&results);
        },
        &err);

    assert(called);
    assert(resp_code == 0);
    assert(results.contains("uris"));
    const auto* uris = results["uris"].get_if<std::vector<std::string>>();
    assert(uris != nullptr);
    assert(!uris->empty());
    assert(uris->front().starts_with("file://"));
    std::cout << "OpenFile returned URI: " << uris->front() << "\n";

    // 2. Call SaveFile
    resp_code = 999;
    results.clear();

    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.FileChooser",
        "SaveFile",
        [](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/fc_save_1"});
            msg.append_string("org.example.App");
            msg.append_string("");
            msg.append_string("Save File Dialog");
            VariantMap opts;
            opts["current_name"] = Variant("exported_document.pdf");
            msg.append_variant_map(opts);
        },
        [&resp_code, &results](dbus::Message& reply) {
            reply.read_uint32(&resp_code);
            reply.read_variant_map(&results);
        },
        &err);

    assert(called);
    assert(resp_code == 0);
    assert(results.contains("uris"));
    const auto* save_uris = results["uris"].get_if<std::vector<std::string>>();
    assert(save_uris != nullptr);
    assert(!save_uris->empty());
    assert(save_uris->front().starts_with("file://"));
    assert(save_uris->front().find("exported_document.pdf") != std::string::npos);
    std::cout << "SaveFile returned URI: " << save_uris->front() << "\n";

    backend->stop();
    std::cout << "test_filechooser PASSED\n";
    return 0;
}
