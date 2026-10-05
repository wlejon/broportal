#include "broportal/backend.h"
#include "broportal/request.h"
#include "broportal/session.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace broportal;

    std::string err;
    auto bus = dbus::Bus::open_user(&err);
    if (!bus) {
        std::cout << "Skipping test_request_session: user bus not available: " << err << "\n";
        return 77;
    }

    auto client_bus = dbus::Bus::open_user(&err);
    assert(client_bus);

    const std::string bus_name = "org.freedesktop.impl.portal.desktop.bro.test_req_sess";
    bus->request_name(bus_name, SD_BUS_NAME_REPLACE_EXISTING);

    std::atomic<bool> server_stop{false};
    std::thread server_thread([&bus, &server_stop]() {
        while (!server_stop.load()) {
            while (bus->process() > 0) {}
            bus->wait(20000);
        }
    });

    // 1. Test Request Close()
    bool request_closed = false;
    ObjectPath req1_path{"/org/freedesktop/portal/desktop/request/test_req_1"};
    auto req1 = std::make_unique<Request>(*bus, req1_path, "test.app", [&request_closed](Request&) {
        request_closed = true;
    });

    // Client calls Close() on the Request
    bool call_ok = client_bus->call_method(
        bus_name,
        req1_path.path,
        "org.freedesktop.impl.portal.Request",
        "Close",
        nullptr,
        nullptr,
        &err);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    assert(call_ok);
    assert(request_closed);
    assert(req1->is_closed());

    // 2. Test Request complete() and Response signal
    bool received_response_signal = false;
    uint32_t signal_code = 999;
    std::string signal_custom_val;

    auto match_slot = client_bus->add_match(
        "type='signal',interface='org.freedesktop.impl.portal.Request',member='Response'",
        [&received_response_signal, &signal_code, &signal_custom_val](dbus::Message& msg) {
            received_response_signal = true;
            msg.read_uint32(&signal_code);
            VariantMap results;
            msg.read_variant_map(&results);
            signal_custom_val = get_string(results, "result_key").value_or("");
        });

    ObjectPath req2_path{"/org/freedesktop/portal/desktop/request/test_req_2"};
    auto req2 = std::make_unique<Request>(*bus, req2_path, "test.app");

    VariantMap test_results;
    test_results["result_key"] = Variant("result_val_success");
    req2->complete(ResponseCode::Success, test_results);

    // Process bus messages
    for (int i = 0; i < 20; ++i) {
        bus->process();
        client_bus->process();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    assert(received_response_signal);
    assert(signal_code == 0);
    assert(signal_custom_val == "result_val_success");

    // 3. Test Session Close() and Closed signal
    bool session_closed_callback = false;
    bool received_closed_signal = false;

    auto session_match = client_bus->add_match(
        "type='signal',interface='org.freedesktop.impl.portal.Session',member='Closed'",
        [&received_closed_signal](dbus::Message&) {
            received_closed_signal = true;
        });

    ObjectPath sess_path{"/org/freedesktop/portal/desktop/session/test_sess_1"};
    auto sess = std::make_unique<Session>(
        *bus, sess_path, "test.app", SessionType::Custom,
        [&session_closed_callback](Session&) {
            session_closed_callback = true;
        });

    // Client calls Close() on the Session
    call_ok = client_bus->call_method(
        bus_name,
        sess_path.path,
        "org.freedesktop.impl.portal.Session",
        "Close",
        nullptr,
        nullptr,
        &err);

    for (int i = 0; i < 20; ++i) {
        bus->process();
        client_bus->process();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    assert(call_ok);
    assert(session_closed_callback);
    assert(received_closed_signal);
    assert(sess->is_closed());

    server_stop.store(true);
    bus->flush();
    if (server_thread.joinable()) {
        server_thread.join();
    }
    bus->release_name(bus_name);

    std::cout << "test_request_session PASSED\n";
    return 0;
}
