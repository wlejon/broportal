// The sd-bus layer on a private dbus-daemon: connection, method calls to the
// bus driver, name ownership, signal emission and matches (checked from a
// second connection and with dbus-send as an outside sender), and slot/bus
// move semantics. Linux.
#include "check.h"
#include "fixture.h"
#include "broportal/dbus_helpers.h"

#include <string>

using namespace broportal;
using namespace broportal::dbus;

namespace {

void pump(Bus& bus, int rounds = 20) {
    for (int i = 0; i < rounds; ++i) {
        bus.wait(20000);
        while (bus.process() > 0) {
        }
    }
}

}  // namespace

int main() {
    bstest::PrivateBus daemon;
    if (!daemon.ok()) bstest::skip("test_dbus_helpers", "dbus-daemon could not be started");

    std::string err;
    auto bus = Bus::open_address(daemon.address, &err);
    REQUIRE(bus);
    CHECK(bus->is_valid());
    CHECK(bus->get_fd() >= 0);

    // A method call through Message, answered by the bus driver.
    std::string owner;
    CHECK(bus->call_method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                           "GetNameOwner", [](Message& m) { m.append_string("org.freedesktop.DBus"); },
                           [&](Message& r) { r.read_string(&owner); }, &err));
    CHECK_EQ(owner, std::string("org.freedesktop.DBus"));

    // Name ownership, visible to another connection.
    CHECK(bus->request_name("org.bro.PortalTest", 0, &err));
    auto other = Bus::open_address(daemon.address, &err);
    REQUIRE(other);
    bool has_owner = false;
    CHECK(other->call_method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                             "NameHasOwner", [](Message& m) { m.append_string("org.bro.PortalTest"); },
                             [&](Message& r) { r.read_bool(&has_owner); }, &err));
    CHECK(has_owner);
    // A second owner is refused.
    CHECK(!other->request_name("org.bro.PortalTest", 0, &err));
    CHECK(bus->release_name("org.bro.PortalTest", &err));

    // Signals: emitted by one connection, matched on another.
    int hits = 0;
    std::string payload;
    auto slot = other->add_match("type='signal',interface='org.bro.Test',member='Ping'",
                                 [&](Message& m) {
                                     m.read_string(&payload);
                                     ++hits;
                                 },
                                 &err);
    REQUIRE(slot.is_valid());
    pump(*other, 2);
    CHECK(bus->emit_signal("/org/bro/test", "org.bro.Test", "Ping",
                           [](Message& m) { m.append_string("from-broportal"); }, &err));
    bus->flush();
    CHECK(bstest::wait_until([&] { pump(*other, 1); return hits == 1; }, std::chrono::seconds(5)));
    CHECK_EQ(payload, std::string("from-broportal"));

    // ...and from an outside sender.
    // --bus registers with the daemon (Hello); --address/--peer would not,
    // and the daemon routes nothing from an unregistered connection.
    std::string send = "dbus-send --bus='" + daemon.address +
                       "' --type=signal /x org.bro.Test.Ping string:from-dbus-send >/dev/null 2>&1";
    if (std::system(send.c_str()) == 0) {
        CHECK(bstest::wait_until([&] { pump(*other, 1); return hits == 2; }, std::chrono::seconds(5)));
        CHECK_EQ(payload, std::string("from-dbus-send"));
    } else {
        std::printf("Note: dbus-send is not installed; the outside-sender check did not run\n");
    }

    // Slot moves carry the match; reset ends it.
    Slot moved = std::move(slot);
    CHECK(moved.is_valid());
    CHECK(!slot.is_valid());
    moved.reset();
    CHECK(!moved.is_valid());
    int before = hits;
    bus->emit_signal("/org/bro/test", "org.bro.Test", "Ping", [](Message& m) { m.append_string("late"); });
    bus->flush();
    pump(*other);
    CHECK_EQ(hits, before);

    // Bus moves carry the connection.
    Bus moved_bus = std::move(*bus);
    CHECK(moved_bus.is_valid());
    CHECK(!bus->is_valid());

    // A call to a name nobody owns fails with an error.
    err.clear();
    CHECK(!moved_bus.call_method("org.bro.Nobody", "/", "org.bro.Nobody", "X", nullptr, nullptr, &err, 1000000));
    CHECK(!err.empty());

    return bstest::finish("test_dbus_helpers");
}
