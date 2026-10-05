#include "broportal/backend.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace broportal;

    std::string err;
    BackendConfig config;
    config.bus_name = "org.freedesktop.impl.portal.desktop.bro.test_shortcuts";
    config.object_path = "/org/freedesktop/portal/desktop";

    auto backend = PortalBackend::create_on_user_bus(config, &err);
    if (!backend) {
        std::cout << "Skipping test_globalshortcuts: user bus not available: " << err << "\n";
        return 77;
    }

    bool ok = backend->start(&err);
    assert(ok);
    assert(backend->run_in_background());

    auto client_bus = dbus::Bus::open_user(&err);
    assert(client_bus);

    ObjectPath sess_handle{"/org/freedesktop/portal/desktop/session/gs_sess_1"};
    uint32_t resp_code = 999;
    VariantMap results;

    // 1. CreateSession
    bool called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.GlobalShortcuts",
        "CreateSession",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/gs_req_1"});
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

    // 2. BindShortcuts
    resp_code = 999;
    results.clear();

    ShortcutList shortcuts_to_bind;
    VariantMap sc1;
    sc1["description"] = Variant("Mute Microphone");
    sc1["trigger_description"] = Variant("F9");
    shortcuts_to_bind.emplace_back("mute_mic", std::move(sc1));

    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.GlobalShortcuts",
        "BindShortcuts",
        [&sess_handle, &shortcuts_to_bind](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/gs_req_2"});
            msg.append_object_path(sess_handle);
            msg.append_shortcut_list(shortcuts_to_bind);
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
    assert(results.contains("shortcuts"));

    // 3. ListShortcuts
    resp_code = 999;
    results.clear();

    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.GlobalShortcuts",
        "ListShortcuts",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/gs_req_3"});
            msg.append_object_path(sess_handle);
        },
        [&resp_code, &results](dbus::Message& reply) {
            reply.read_uint32(&resp_code);
            reply.read_variant_map(&results);
        },
        &err);

    assert(called);
    assert(resp_code == 0);
    assert(results.contains("shortcuts"));
    const auto* listed = results["shortcuts"].get_if<ShortcutList>();
    assert(listed != nullptr);
    assert(!listed->empty());
    assert(listed->front().first == "mute_mic");

    // 4. Activate signal
    bool activated_received = false;
    std::string activated_id;
    uint64_t activated_ts = 0;

    auto slot = client_bus->add_match(
        "type='signal',interface='org.freedesktop.impl.portal.GlobalShortcuts',member='Activated'",
        [&activated_received, &activated_id, &activated_ts](dbus::Message& msg) {
            activated_received = true;
            ObjectPath sp;
            msg.read_object_path(&sp);
            msg.read_string(&activated_id);
            msg.read_uint64(&activated_ts);
        });

    assert(slot.is_valid());

    uint64_t test_ts = 123456789;
    backend->global_shortcuts().activate_shortcut(sess_handle, "mute_mic", test_ts);

    for (int i = 0; i < 30; ++i) {
        client_bus->process();
        if (activated_received) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    assert(activated_received);
    assert(activated_id == "mute_mic");
    assert(activated_ts == test_ts);

    backend->stop();
    std::cout << "test_globalshortcuts PASSED\n";
    return 0;
}
