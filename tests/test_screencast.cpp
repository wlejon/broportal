#include "broportal/backend.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace broportal;

    std::string err;
    BackendConfig config;
    config.bus_name = "org.freedesktop.impl.portal.desktop.bro.test_screencast";
    config.object_path = "/org/freedesktop/portal/desktop";

    auto backend = PortalBackend::create_on_user_bus(config, &err);
    if (!backend) {
        std::cout << "Skipping test_screencast: user bus not available: " << err << "\n";
        return 77;
    }

    bool ok = backend->start(&err);
    assert(ok);
    assert(backend->run_in_background());

    auto client_bus = dbus::Bus::open_user(&err);
    assert(client_bus);

    ObjectPath sess_handle{"/org/freedesktop/portal/desktop/session/sc_sess_1"};
    uint32_t resp_code = 999;
    VariantMap results;

    // 1. CreateSession
    bool called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.ScreenCast",
        "CreateSession",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/sc_req_1"});
            msg.append_object_path(sess_handle);
            msg.append_string("org.example.App");
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
    assert(results.contains("session_id"));
    std::cout << "Created ScreenCast session: " << results["session_id"].get_value_or<std::string>("") << "\n";

    // 2. SelectSources
    resp_code = 999;
    results.clear();
    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.ScreenCast",
        "SelectSources",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/sc_req_2"});
            msg.append_object_path(sess_handle);
            msg.append_string("org.example.App");
            VariantMap opts;
            opts["types"] = Variant(static_cast<uint32_t>(SourceType::Monitor));
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

    // 3. Start (creates real PipeWire node)
    resp_code = 999;
    results.clear();
    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.ScreenCast",
        "Start",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/sc_req_3"});
            msg.append_object_path(sess_handle);
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
    assert(results.contains("streams"));
    const auto* stream_list = results["streams"].get_if<StreamList>();
    assert(stream_list != nullptr);
    assert(!stream_list->empty());

    uint32_t node_id = stream_list->front().first;
    const VariantMap& sprops = stream_list->front().second;
    assert(node_id > 0);
    assert(sprops.contains("position"));
    assert(sprops.contains("size"));
    assert(sprops.contains("source_type"));
    assert(sprops.contains("pipewire-serial"));

    std::cout << "ScreenCast Start returned PipeWire node ID: " << node_id
              << ", serial: " << sprops.at("pipewire-serial").get_value_or<uint64_t>(0) << "\n";

    // 4. Close Session
    called = client_bus->call_method(
        config.bus_name,
        sess_handle.path,
        "org.freedesktop.impl.portal.Session",
        "Close",
        nullptr,
        nullptr,
        &err);
    assert(called);

    backend->stop();
    std::cout << "test_screencast PASSED\n";
    return 0;
}
