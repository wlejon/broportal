// Test concurrency, thread-safety, and non-blocking callback behavior in broportal.
// Verifies:
// 1. Signal-emitting setters called concurrently with background dispatch do not race sd-bus.
// 2. Callbacks swapped mid-dispatch do not race the dispatcher.
// 3. Slow/synchronous callbacks do not block the D-Bus connection or other requests.
#include "check.h"
#include "fixture.h"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace broportal;
using namespace std::chrono_literals;

namespace {

// Test 1: Concurrently call signal-emitting setters from multiple threads
// while PortalBackend is running in the background.
void test_concurrent_signal_setters() {
    bstest::PortalFixture f("concurrent_signal_setters");

    std::atomic<bool> stop{false};
    std::atomic<int> signals_sent{0};

    // Thread A: calls settings setters
    std::thread t_settings([&]() {
        uint32_t val = 0;
        while (!stop.load()) {
            f.backend->settings().set_color_scheme((val++) % 3);
            f.backend->settings().set_setting("org.example.test", "counter", Variant(val));
            signals_sent.fetch_add(2);
            std::this_thread::yield();
        }
    });

    // Thread B: calls inhibit state notifications
    std::thread t_inhibit([&]() {
        bool active = false;
        while (!stop.load()) {
            f.backend->inhibit().notify_state_changed(active, SessionState::Running);
            active = !active;
            signals_sent.fetch_add(1);
            std::this_thread::yield();
        }
    });

    // Thread C: calls global shortcut activation/deactivation
    std::thread t_shortcuts([&]() {
        uint64_t ts = 1000;
        while (!stop.load()) {
            f.backend->global_shortcuts().activate_shortcut(
                ObjectPath{"/org/freedesktop/portal/desktop/session/test"}, "shot1", ts++);
            f.backend->global_shortcuts().deactivate_shortcut(
                ObjectPath{"/org/freedesktop/portal/desktop/session/test"}, "shot1", ts++);
            signals_sent.fetch_add(2);
            std::this_thread::yield();
        }
    });

    // Let threads run concurrently for 200ms
    std::this_thread::sleep_for(200ms);
    stop.store(true);

    t_settings.join();
    t_inhibit.join();
    t_shortcuts.join();

    CHECK(signals_sent.load() > 50);
}

// Test 2: Swap callbacks repeatedly while requests are dispatched in flight.
void test_callbacks_swapped_mid_dispatch() {
    bstest::PortalFixture f("callbacks_swapped_mid_dispatch");

    std::atomic<bool> stop{false};
    std::atomic<int> successful_calls{0};

    // Thread 1: Rapidly swap the file picker callback
    std::thread t_swapper([&]() {
        int toggle = 0;
        while (!stop.load()) {
            if (toggle++ % 2 == 0) {
                f.backend->file_chooser().set_file_picker_callback(
                    [](const ObjectPath&, const std::string&, const std::string&,
                       const FileChooserOptions&, std::vector<std::string>& out, VariantMap&) {
                        out.push_back("file:///tmp/one.txt");
                        return ResponseCode::Success;
                    });
            } else {
                f.backend->file_chooser().set_file_picker_callback(
                    [](const ObjectPath&, const std::string&, const std::string&,
                       const FileChooserOptions&, std::vector<std::string>& out, VariantMap&) {
                        out.push_back("file:///tmp/two.txt");
                        return ResponseCode::Success;
                    });
            }
            std::this_thread::yield();
        }
    });

    // Thread 2: Make method calls continuously
    std::thread t_caller([&]() {
        int count = 0;
        while (!stop.load() && count < 50) {
            std::string err;
            uint32_t code = 999;
            VariantMap results;
            bool ok = f.call(
                "org.freedesktop.impl.portal.FileChooser", "OpenFile",
                [&](dbus::Message& msg) {
                    msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/call_" + std::to_string(count)});
                    msg.append_string("org.example.Caller");
                    msg.append_string("");
                    msg.append_string("Title");
                    msg.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&code);
                    reply.read_variant_map(&results);
                },
                &err);

            if (ok && code == static_cast<uint32_t>(ResponseCode::Success)) {
                successful_calls.fetch_add(1);
            }
            count++;
        }
    });

    t_caller.join();
    stop.store(true);
    t_swapper.join();

    CHECK_EQ(successful_calls.load(), 50);
}

// Test 3: Verify that a slow/synchronous callback does not block the connection
// or other requests.
void test_nonblocking_synchronous_callbacks() {
    bstest::PortalFixture f("nonblocking_synchronous_callbacks");

    // File chooser callback takes 150ms to simulate a modal dialog or user thinking
    f.backend->file_chooser().set_file_picker_callback(
        [](const ObjectPath&, const std::string&, const std::string&,
           const FileChooserOptions&, std::vector<std::string>& out, VariantMap&) {
            std::this_thread::sleep_for(150ms);
            out.push_back("file:///tmp/slow.txt");
            return ResponseCode::Success;
        });

    f.backend->settings().set_color_scheme(1);

    auto slow_start = std::chrono::steady_clock::now();

    // Launch slow OpenFile request in separate thread
    auto slow_future = std::async(std::launch::async, [&]() {
        std::string err;
        uint32_t code = 999;
        VariantMap results;
        bool ok = f.call(
            "org.freedesktop.impl.portal.FileChooser", "OpenFile",
            [&](dbus::Message& msg) {
                msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/slow_req"});
                msg.append_string("org.example.App");
                msg.append_string("");
                msg.append_string("Slow Dialog");
                msg.append_variant_map({});
            },
            [&](dbus::Message& reply) {
                reply.read_uint32(&code);
                reply.read_variant_map(&results);
            },
            &err);
        return ok && (code == static_cast<uint32_t>(ResponseCode::Success));
    });

    // Wait 25ms to ensure the slow request has arrived and is executing in the worker
    std::this_thread::sleep_for(25ms);

    // Now, create a second client connection and issue a Settings Read call
    std::string client2_err;
    auto client2 = dbus::Bus::open_address(f.bus.address, &client2_err);
    REQUIRE(client2 != nullptr);

    auto quick_start = std::chrono::steady_clock::now();
    Variant val;
    bool quick_ok = client2->call_method(
        f.config.bus_name, f.config.object_path,
        "org.freedesktop.impl.portal.Settings", "Read",
        [&](dbus::Message& msg) {
            msg.append_string("org.freedesktop.appearance");
            msg.append_string("color-scheme");
        },
        [&](dbus::Message& reply) {
            reply.read_variant(&val);
        });

    auto quick_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - quick_start).count();

    CHECK(quick_ok);
    CHECK(val.get_if<uint32_t>() != nullptr && *val.get_if<uint32_t>() == 1u);

    // Crucial assertion: the quick request MUST finish well before the slow request!
    // With 150ms sleep in the slow callback, quick should take < 50ms.
    CHECK(quick_duration < 80);

    // Wait for the slow request to complete
    bool slow_ok = slow_future.get();
    auto slow_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - slow_start).count();

    CHECK(slow_ok);
    CHECK(slow_duration >= 140);
}

}  // namespace

int main() {
    test_concurrent_signal_setters();
    test_callbacks_swapped_mid_dispatch();
    test_nonblocking_synchronous_callbacks();
    return 0;
}
