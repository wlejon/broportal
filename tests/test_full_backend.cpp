#include "broportal/backend.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace broportal;

    std::string err;
    BackendConfig config;
    config.bus_name = "org.freedesktop.impl.portal.desktop.bro.test_full";
    config.object_path = "/org/freedesktop/portal/desktop";

    auto backend = PortalBackend::create_on_user_bus(config, &err);
    if (!backend) {
        std::cout << "Skipping test_full_backend: user bus not available: " << err << "\n";
        return 77;
    }

    assert(backend->start(&err));
    assert(backend->is_running());
    assert(backend->run_in_background());

    auto client_bus = dbus::Bus::open_user(&err);
    assert(client_bus);

    // Call Introspectable on /org/freedesktop/portal/desktop
    bool called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.DBus.Introspectable",
        "Introspect",
        nullptr,
        [](dbus::Message& reply) {
            std::string xml;
            assert(reply.read_string(&xml));
            assert(xml.find("org.freedesktop.impl.portal.FileChooser") != std::string::npos);
            assert(xml.find("org.freedesktop.impl.portal.Screenshot") != std::string::npos);
            assert(xml.find("org.freedesktop.impl.portal.ScreenCast") != std::string::npos);
            assert(xml.find("org.freedesktop.impl.portal.RemoteDesktop") != std::string::npos);
            assert(xml.find("org.freedesktop.impl.portal.Settings") != std::string::npos);
            assert(xml.find("org.freedesktop.impl.portal.Inhibit") != std::string::npos);
            assert(xml.find("org.freedesktop.impl.portal.OpenURI") != std::string::npos);
            assert(xml.find("org.freedesktop.impl.portal.GlobalShortcuts") != std::string::npos);
            std::cout << "Introspect verified all 8 interfaces registered on object path!\n";
        },
        &err);

    assert(called);

    // Test Request and Session lifecycle management from backend
    ObjectPath test_req_path{"/org/freedesktop/portal/desktop/request/full_test_req"};
    auto req = backend->create_request(test_req_path, "org.test.App");
    assert(req != nullptr);
    assert(backend->get_request(test_req_path) == req);
    backend->remove_request(test_req_path);
    assert(backend->get_request(test_req_path) == nullptr);

    ObjectPath test_sess_path{"/org/freedesktop/portal/desktop/session/full_test_sess"};
    auto sess = backend->create_session(test_sess_path, "org.test.App", SessionType::Custom);
    assert(sess != nullptr);
    assert(backend->get_session(test_sess_path) == sess);
    backend->remove_session(test_sess_path);
    assert(backend->get_session(test_sess_path) == nullptr);

    backend->stop();
    assert(!backend->is_running());

    std::cout << "test_full_backend PASSED\n";
    return 0;
}
