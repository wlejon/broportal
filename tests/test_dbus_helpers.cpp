#include "broportal/dbus_helpers.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace broportal;
    using namespace broportal::dbus;

    std::string err;
    auto bus = Bus::open_user(&err);
    if (!bus) {
        std::cout << "Skipping test_dbus_helpers: user bus not available: " << err << "\n";
        return 77;
    }

    assert(bus->is_valid());
    assert(bus->get_fd() >= 0);

    // Test move semantics
    Bus moved_bus = std::move(*bus);
    assert(moved_bus.is_valid());
    assert(!bus->is_valid());

    // Test message creation and encoding
    sd_bus_message* raw_msg = nullptr;
    int r = sd_bus_message_new_method_call(
        moved_bus.raw(),
        &raw_msg,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "GetNameOwner");
    assert(r >= 0);

    Message msg(raw_msg, true);
    assert(msg.is_valid());
    assert(msg.append_string("org.freedesktop.DBus"));

    // Test call
    sd_bus_error sdbus_err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    r = sd_bus_call(moved_bus.raw(), msg.raw(), 3000000, &sdbus_err, &reply);
    assert(r >= 0);
    assert(reply != nullptr);

    Message reply_msg(reply, true);
    std::string owner;
    assert(reply_msg.read_string(&owner));
    assert(!owner.empty());
    std::cout << "org.freedesktop.DBus owner: " << owner << "\n";

    // Test Match Slot
    int signal_count = 0;
    auto slot = moved_bus.add_match(
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
        [&signal_count](Message&) {
            signal_count++;
        });
    assert(slot.is_valid());

    // Test Slot move semantics
    Slot moved_slot = std::move(slot);
    assert(moved_slot.is_valid());
    assert(!slot.is_valid());

    moved_slot.reset();
    assert(!moved_slot.is_valid());

    std::cout << "test_dbus_helpers PASSED\n";
    return 0;
}
