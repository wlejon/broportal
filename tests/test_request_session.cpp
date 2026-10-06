// Request and Session objects on a private bus, driven from outside by gdbus:
// Close() runs the close callback and marks the object closed, complete()
// emits Response with the results, and a closed Session emits Closed. One
// thread dispatches every connection. Linux.
#include "check.h"
#include "fixture.h"
#include "broportal/request.h"
#include "broportal/session.h"

#include <chrono>

using namespace broportal;

namespace {

void pump(dbus::Bus& a, dbus::Bus& b) {
    for (int i = 0; i < 3; ++i) {
        while (a.process() > 0) {
        }
        b.wait(5000);
        while (b.process() > 0) {
        }
    }
}

}  // namespace

int main() {
    bstest::PrivateBus daemon;
    if (!daemon.ok()) bstest::skip("test_request_session", "dbus-daemon could not be started");
    if (!bstest::have_gdbus()) bstest::skip("test_request_session", "gdbus (the outside client) is not installed");

    std::string err;
    auto bus = dbus::Bus::open_address(daemon.address, &err);
    REQUIRE(bus);
    auto client = dbus::Bus::open_address(daemon.address, &err);
    REQUIRE(client);
    const std::string bus_name = "org.freedesktop.impl.portal.desktop.bro.test_req_sess";
    REQUIRE(bus->request_name(bus_name, 0, &err));
    auto gdbus_close = [&](const std::string& path, const std::string& iface) {
        return bstest::run_while_dispatching(*bus, "gdbus call --address '" + daemon.address + "' --dest " +
                                                       bus_name + " --object-path " + path + " --method " +
                                                       iface + ".Close");
    };

    // 1. Request.Close from a client.
    bool request_closed = false;
    ObjectPath req1_path{"/org/freedesktop/portal/desktop/request/test_req_1"};
    auto req1 = std::make_unique<Request>(*bus, req1_path, "test.app", [&](Request&) { request_closed = true; });
    auto r1 = gdbus_close(req1_path.path, "org.freedesktop.impl.portal.Request");
    CHECK_EQ(r1.status, 0);
    CHECK(request_closed);
    CHECK(req1->is_closed());

    // 2. complete() emits Response(code, results).
    bool got_response = false;
    uint32_t code = 999;
    std::string value;
    auto response_slot = client->add_match(
        "type='signal',interface='org.freedesktop.impl.portal.Request',member='Response'",
        [&](dbus::Message& msg) {
            got_response = true;
            msg.read_uint32(&code);
            VariantMap results;
            msg.read_variant_map(&results);
            value = get_string(results, "result_key").value_or("");
        });
    REQUIRE(response_slot.is_valid());
    pump(*bus, *client);

    ObjectPath req2_path{"/org/freedesktop/portal/desktop/request/test_req_2"};
    auto req2 = std::make_unique<Request>(*bus, req2_path, "test.app");
    VariantMap results;
    results["result_key"] = Variant("result_val_success");
    req2->complete(ResponseCode::Success, results);
    bus->flush();
    CHECK(bstest::wait_until([&] { pump(*bus, *client); return got_response; }, std::chrono::seconds(5)));
    CHECK_EQ(code, 0u);
    CHECK_EQ(value, std::string("result_val_success"));

    // 3. Session.Close from a client: callback, Closed signal, closed state.
    bool session_closed = false;
    bool got_closed = false;
    auto closed_slot = client->add_match(
        "type='signal',interface='org.freedesktop.impl.portal.Session',member='Closed'",
        [&](dbus::Message&) { got_closed = true; });
    REQUIRE(closed_slot.is_valid());
    pump(*bus, *client);

    ObjectPath sess_path{"/org/freedesktop/portal/desktop/session/test_sess_1"};
    auto sess = std::make_unique<Session>(*bus, sess_path, "test.app", SessionType::Custom,
                                          [&](Session&) { session_closed = true; });
    auto r3 = gdbus_close(sess_path.path, "org.freedesktop.impl.portal.Session");
    CHECK_EQ(r3.status, 0);
    CHECK(session_closed);
    CHECK(sess->is_closed());
    CHECK(bstest::wait_until([&] { pump(*bus, *client); return got_closed; }, std::chrono::seconds(5)));

    return bstest::finish("test_request_session");
}
