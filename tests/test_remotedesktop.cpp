#include "broportal/backend.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace broportal;

    std::string err;
    BackendConfig config;
    config.bus_name = "org.freedesktop.impl.portal.desktop.bro.test_rd";
    config.object_path = "/org/freedesktop/portal/desktop";

    auto backend = PortalBackend::create_on_user_bus(config, &err);
    if (!backend) {
        std::cout << "Skipping test_remotedesktop: user bus not available: " << err << "\n";
        return 77;
    }

    // Set up event listeners
    int32_t received_keycode = 0;
    uint32_t received_keystate = 0;
    double received_dx = 0.0, received_dy = 0.0;
    int32_t received_button = 0;
    uint32_t received_buttonstate = 0;

    RemoteDesktopInputListener listener;
    listener.on_keyboard_keycode = [&](const ObjectPath&, int32_t keycode, uint32_t state) {
        received_keycode = keycode;
        received_keystate = state;
    };
    listener.on_pointer_motion = [&](const ObjectPath&, double dx, double dy) {
        received_dx = dx;
        received_dy = dy;
    };
    listener.on_pointer_button = [&](const ObjectPath&, int32_t btn, uint32_t st) {
        received_button = btn;
        received_buttonstate = st;
    };
    backend->remote_desktop().set_input_listener(std::move(listener));

    bool ok = backend->start(&err);
    assert(ok);
    assert(backend->run_in_background());

    auto client_bus = dbus::Bus::open_user(&err);
    assert(client_bus);

    ObjectPath sess_handle{"/org/freedesktop/portal/desktop/session/rd_sess_1"};
    uint32_t resp_code = 999;
    VariantMap results;

    // 1. CreateSession
    bool called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.RemoteDesktop",
        "CreateSession",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/rd_req_1"});
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
    assert(results.contains("session"));

    // 2. SelectDevices
    resp_code = 999;
    results.clear();
    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.RemoteDesktop",
        "SelectDevices",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/rd_req_2"});
            msg.append_object_path(sess_handle);
            msg.append_string("org.example.App");
            VariantMap opts;
            opts["types"] = Variant(static_cast<uint32_t>(7));
            msg.append_variant_map(opts);
        },
        [&resp_code, &results](dbus::Message& reply) {
            reply.read_uint32(&resp_code);
            reply.read_variant_map(&results);
        },
        &err);

    assert(called);
    assert(resp_code == 0);

    // 3. Start
    resp_code = 999;
    results.clear();
    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.RemoteDesktop",
        "Start",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/rd_req_3"});
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
    assert(results.contains("devices"));

    // 4. Send NotifyKeyboardKeycode
    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.RemoteDesktop",
        "NotifyKeyboardKeycode",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(sess_handle);
            VariantMap opts;
            msg.append_variant_map(opts);
            msg.append_int32(42);
            msg.append_uint32(1);
        },
        nullptr,
        &err);
    assert(called);

    // 5. Send NotifyPointerMotion
    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.RemoteDesktop",
        "NotifyPointerMotion",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(sess_handle);
            VariantMap opts;
            msg.append_variant_map(opts);
            msg.append_double(12.5);
            msg.append_double(-8.25);
        },
        nullptr,
        &err);
    assert(called);

    // 6. Send NotifyPointerButton
    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.RemoteDesktop",
        "NotifyPointerButton",
        [&sess_handle](dbus::Message& msg) {
            msg.append_object_path(sess_handle);
            VariantMap opts;
            msg.append_variant_map(opts);
            msg.append_int32(272);
            msg.append_uint32(1);
        },
        nullptr,
        &err);
    assert(called);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    assert(received_keycode == 42);
    assert(received_keystate == 1);
    assert(received_dx > 12.4 && received_dx < 12.6);
    assert(received_dy < -8.2 && received_dy > -8.3);
    assert(received_button == 272);
    assert(received_buttonstate == 1);

    backend->stop();
    std::cout << "test_remotedesktop PASSED\n";
    return 0;
}
