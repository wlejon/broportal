#include "broportal/backend.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace broportal;

    std::string err;
    BackendConfig config;
    config.bus_name = "org.freedesktop.impl.portal.desktop.bro.test_inhibit";
    config.object_path = "/org/freedesktop/portal/desktop";

    auto backend = PortalBackend::create_on_user_bus(config, &err);
    if (!backend) {
        std::cout << "Skipping test_inhibit: user bus not available: " << err << "\n";
        return 77;
    }

    bool ok = backend->start(&err);
    assert(ok);
    assert(backend->run_in_background());

    auto client_bus = dbus::Bus::open_user(&err);
    assert(client_bus);

    // 1. Inhibit
    ObjectPath req_handle{"/org/freedesktop/portal/desktop/request/inhibit_req_1"};
    bool called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.Inhibit",
        "Inhibit",
        [&req_handle](dbus::Message& msg) {
            msg.append_object_path(req_handle);
            msg.append_string("org.example.App");
            msg.append_string("main_window");
            msg.append_uint32(static_cast<uint32_t>(InhibitFlag::Idle) | static_cast<uint32_t>(InhibitFlag::Suspend));
            VariantMap opts;
            opts["reason"] = Variant("Playing a video");
            msg.append_variant_map(opts);
        },
        nullptr,
        &err);

    assert(called);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    assert(backend->inhibit().is_inhibited(InhibitFlag::Idle));
    assert(backend->inhibit().is_inhibited(InhibitFlag::Suspend));
    assert(!backend->inhibit().is_inhibited(InhibitFlag::Logout));

    auto inhibitions = backend->inhibit().active_inhibitions();
    assert(inhibitions.size() == 1);
    assert(inhibitions.front().reason == "Playing a video");

    // 2. Client cancels inhibition by calling Request.Close()
    called = client_bus->call_method(
        config.bus_name,
        req_handle.path,
        "org.freedesktop.impl.portal.Request",
        "Close",
        nullptr,
        nullptr,
        &err);
    assert(called);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!backend->inhibit().is_inhibited(InhibitFlag::Idle));
    assert(backend->inhibit().active_inhibitions().empty());
    std::cout << "Inhibition cleared after Request.Close()\n";

    // 3. CreateMonitor & StateChanged
    bool state_changed_received = false;
    uint32_t session_state = 0;

    auto slot = client_bus->add_match(
        "type='signal',interface='org.freedesktop.impl.portal.Inhibit',member='StateChanged'",
        [&state_changed_received, &session_state](dbus::Message& msg) {
            state_changed_received = true;
            ObjectPath mon_path;
            msg.read_object_path(&mon_path);
            VariantMap state_map;
            msg.read_variant_map(&state_map);
            session_state = get_uint32_or(state_map, "session-state", 0);
        });

    assert(slot.is_valid());

    ObjectPath mon_sess{"/org/freedesktop/portal/desktop/session/inhibit_mon_1"};
    uint32_t resp_code = 999;
    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.Inhibit",
        "CreateMonitor",
        [&mon_sess](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/inhibit_mon_req"});
            msg.append_object_path(mon_sess);
            msg.append_string("org.example.App");
            msg.append_string("main_window");
        },
        [&resp_code](dbus::Message& reply) {
            reply.read_uint32(&resp_code);
        },
        &err);

    assert(called);
    assert(resp_code == 0);

    backend->inhibit().notify_state_changed(false, SessionState::QueryEnd);

    for (int i = 0; i < 30; ++i) {
        client_bus->process();
        if (state_changed_received) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    assert(state_changed_received);
    assert(session_state == static_cast<uint32_t>(SessionState::QueryEnd));

    // Acknowledge query end
    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.Inhibit",
        "QueryEndResponse",
        [&mon_sess](dbus::Message& msg) {
            msg.append_object_path(mon_sess);
        },
        nullptr,
        &err);
    assert(called);

    backend->stop();
    std::cout << "test_inhibit PASSED\n";
    return 0;
}
