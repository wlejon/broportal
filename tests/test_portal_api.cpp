#include "../src/api/api.h"
#include "embed/embed.h"
#include "eval/eval.h"
#include "check.h"
#if defined(__linux__)
#include "fixture.h"
#endif

#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <vector>

using namespace broportal;

#if defined(__linux__)
namespace {

struct Reply {
    bool called = false;
    uint32_t code = 999;
    VariantMap results;
};

template <typename F>
auto callWithPump(F&& fn) {
    auto future = std::async(std::launch::async, std::forward<F>(fn));
    while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
        broportal::api::tickPortalAsync();
    }
    return future.get();
}

} // namespace
#endif

#if !defined(__linux__)

// Off Linux there is no portal backend: bro.portal says so, with a reason, and
// every entry point refuses rather than pretending to register a handler.
int main() {
    namespace ev = bronze::embed;
    using namespace bronze::eval;

    std::cout << "Starting broportal JavaScript API tests (unavailable platform)..." << std::endl;

    broportal::api::installPortal();

    auto g = ev::globalValue("bro");
    CHECK(g.found);
    ev::Persistent portal(ev::getProperty(g.value, "portal"));
    CHECK(ev::isObject(portal.get()));

    ev::Persistent pAvail(ev::getProperty(portal.get(), "available"));
    CHECK(ev::isBool(pAvail.get()) && !ev::toBool(pAvail.get()));

    {
        auto r = evalScript("typeof bro.portal.reason === 'string' && bro.portal.reason.length > 0");
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && ev::toBool(r.value));
    }
    for (const char* call : {"bro.portal.start()", "bro.portal.isRunning()", "bro.portal.stop()"}) {
        auto r = evalScript(call);
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && !ev::toBool(r.value));
    }
    for (const char* on : {"onFileChooser", "onScreenshot", "onScreencast", "onOpenUri"}) {
        auto r = evalScript(std::string("bro.portal.") + on + "(() => {})");
        CHECK(r.thrown);
    }

    broportal::api::tickPortalAsync();
    broportal::api::shutdownPortalAsync();

    std::cout << "All broportal JavaScript API tests PASSED!" << std::endl;
    return 0;
}

#else

int main() {
    namespace ev = bronze::embed;
    using namespace bronze::eval;

    std::cout << "Starting broportal JavaScript API tests..." << std::endl;

    // 1. Install bro.portal into Bronze realm
    broportal::api::installPortal();

    auto g = ev::globalValue("bro");
    CHECK(g.found);
    CHECK(ev::isObject(g.value));

    ev::Persistent portal(ev::getProperty(g.value, "portal"));
    CHECK(ev::isObject(portal.get()));
    std::cout << "  Mounted bro.portal successfully." << std::endl;

    ev::Persistent pAvail(ev::getProperty(portal.get(), "available"));
    CHECK(ev::isBool(pAvail.get()) && ev::toBool(pAvail.get()));

    // Verify required methods
    const char* methods[] = {
        "start", "stop", "isRunning",
        "onFileChooser", "offFileChooser",
        "onScreenshot", "offScreenshot",
        "onPickColor", "offPickColor",
        "onScreencast", "offScreencast",
        "onOpenUri", "offOpenUri"
    };
    for (const char* m : methods) {
        auto fn = ev::getProperty(portal.get(), m);
        CHECK(ev::isFunction(fn));
        std::cout << "  Found bro.portal." << m << std::endl;
    }

    // 2. Initial state: isRunning is false
    {
        auto r = evalScript("bro.portal.isRunning()");
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && !ev::toBool(r.value));
        std::cout << "  Initial isRunning() === false [PASS]" << std::endl;
    }

    // 3. Set up fixture on private D-Bus daemon
    bstest::PortalFixture f("test_portal_api", false);
    broportal::api::setBackend(f.backend.get());

    // Call bro.portal.start()
    {
        auto r = evalScript("bro.portal.start()");
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && ev::toBool(r.value));
        CHECK(f.backend->is_running());

        auto rRunning = evalScript("bro.portal.isRunning()");
        CHECK(!rRunning.thrown);
        CHECK(ev::isBool(rRunning.value) && ev::toBool(rRunning.value));
        std::cout << "  bro.portal.start() -> running [PASS]" << std::endl;
    }

    // 4. Test FileChooser
    std::cout << "Testing FileChooser..." << std::endl;
    {
        // 4a. Synchronous respond()
        auto rReg1 = evalScript(
            "globalThis._fc1 = bro.portal.onFileChooser((req) => {\n"
            "  req.respond({ uris: ['file:///tmp/sync_picked.txt'] });\n"
            "});\n"
        );
        CHECK(!rReg1.thrown);

        auto rep1 = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.FileChooser", "OpenFile",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/fc_1"});
                    m.append_string("org.example.App");
                    m.append_string("");
                    m.append_string("Pick file");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(rep1.code, 0u);
        auto itUris = rep1.results.find("uris");
        CHECK(itUris != rep1.results.end());
        const auto* uris1 = itUris->second.get_if<std::vector<std::string>>();
        REQUIRE(uris1 != nullptr);
        REQUIRE(!uris1->empty());
        CHECK_EQ((*uris1)[0], std::string("file:///tmp/sync_picked.txt"));
        std::cout << "  OpenFile sync respond() [PASS]" << std::endl;

        evalScript("globalThis._fc1.dispose()");

        // 4b. Return object directly
        auto rReg2 = evalScript(
            "globalThis._fc2 = bro.portal.onFileChooser((req) => {\n"
            "  return { uris: ['file:///tmp/direct_return.txt'] };\n"
            "});\n"
        );
        CHECK(!rReg2.thrown);

        auto rep2 = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.FileChooser", "OpenFile",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/fc_2"});
                    m.append_string("org.example.App");
                    m.append_string("");
                    m.append_string("Pick file 2");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(rep2.code, 0u);
        const auto* uris2 = rep2.results["uris"].get_if<std::vector<std::string>>();
        REQUIRE(uris2 != nullptr);
        REQUIRE(!uris2->empty());
        CHECK_EQ((*uris2)[0], std::string("file:///tmp/direct_return.txt"));
        std::cout << "  OpenFile direct return [PASS]" << std::endl;

        evalScript("globalThis._fc2.dispose()");

        // 4c. Async / Promise handler
        auto rReg3 = evalScript(
            "globalThis._fc3 = bro.portal.onFileChooser(async (req) => {\n"
            "  return { uris: ['file:///tmp/async_picked.txt'] };\n"
            "});\n"
        );
        CHECK(!rReg3.thrown);

        auto rep3 = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.FileChooser", "OpenFile",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/fc_3"});
                    m.append_string("org.example.App");
                    m.append_string("");
                    m.append_string("Pick file async");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(rep3.code, 0u);
        const auto* uris3 = rep3.results["uris"].get_if<std::vector<std::string>>();
        REQUIRE(uris3 != nullptr);
        REQUIRE(!uris3->empty());
        CHECK_EQ((*uris3)[0], std::string("file:///tmp/async_picked.txt"));
        std::cout << "  OpenFile async/Promise handler [PASS]" << std::endl;

        evalScript("globalThis._fc3.dispose()");

        // 4d. Cancelled
        auto rReg4 = evalScript(
            "globalThis._fc4 = bro.portal.onFileChooser((req) => {\n"
            "  req.cancel('user cancelled');\n"
            "});\n"
        );
        CHECK(!rReg4.thrown);

        auto rep4 = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.FileChooser", "OpenFile",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/fc_4"});
                    m.append_string("org.example.App");
                    m.append_string("");
                    m.append_string("Pick cancel");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(rep4.code, 1u); // Cancelled
        std::cout << "  OpenFile req.cancel() [PASS]" << std::endl;

        evalScript("globalThis._fc4.dispose()");
    }

    // 5. Test Screenshot & PickColor
    std::cout << "Testing Screenshot & PickColor..." << std::endl;
    {
        // 5a. Screenshot sync
        auto rRegSS1 = evalScript(
            "globalThis._ss1 = bro.portal.onScreenshot((req) => {\n"
            "  return { uri: 'file:///tmp/test_shot.png' };\n"
            "});\n"
        );
        CHECK(!rRegSS1.thrown);

        auto repSS1 = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.Screenshot", "Screenshot",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/ss_1"});
                    m.append_string("org.example.App");
                    m.append_string("");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(repSS1.code, 0u);
        const auto* uriSS1 = repSS1.results["uri"].get_if<std::string>();
        REQUIRE(uriSS1 != nullptr);
        CHECK_EQ(*uriSS1, std::string("file:///tmp/test_shot.png"));
        std::cout << "  Screenshot sync return [PASS]" << std::endl;

        evalScript("globalThis._ss1.dispose()");

        // 5b. Screenshot async cancelled
        auto rRegSS2 = evalScript(
            "globalThis._ss2 = bro.portal.onScreenshot(async (req) => {\n"
            "  return { cancelled: true };\n"
            "});\n"
        );
        CHECK(!rRegSS2.thrown);

        auto repSS2 = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.Screenshot", "Screenshot",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/ss_2"});
                    m.append_string("org.example.App");
                    m.append_string("");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(repSS2.code, 1u); // Cancelled
        std::cout << "  Screenshot async cancelled [PASS]" << std::endl;

        evalScript("globalThis._ss2.dispose()");

        // 5c. PickColor
        auto rRegPC = evalScript(
            "globalThis._pc = bro.portal.onPickColor((req) => {\n"
            "  return { color: { r: 0.1, g: 0.2, b: 0.3 } };\n"
            "});\n"
        );
        CHECK(!rRegPC.thrown);

        auto repPC = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.Screenshot", "PickColor",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/pc_1"});
                    m.append_string("org.example.App");
                    m.append_string("");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(repPC.code, 0u);
        const auto* col = repPC.results["color"].get_if<RgbColor>();
        REQUIRE(col != nullptr);
        CHECK(std::abs(col->r - 0.1) < 1e-4);
        CHECK(std::abs(col->g - 0.2) < 1e-4);
        CHECK(std::abs(col->b - 0.3) < 1e-4);
        std::cout << "  PickColor [PASS]" << std::endl;

        evalScript("globalThis._pc.dispose()");
    }

    // 6. Test ScreenCast
    std::cout << "Testing ScreenCast..." << std::endl;
    {
        auto rRegSC = evalScript(
            "globalThis._sc = bro.portal.onScreencast((req) => {\n"
            "  req.respond({ streams: [{ nodeId: 777 }] });\n"
            "});\n"
        );
        CHECK(!rRegSC.thrown);

        // CreateSession
        auto repCS = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.ScreenCast", "CreateSession",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/sc_cs"});
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/session/sc_sess"});
                    m.append_string("org.example.App");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(repCS.code, 0u);

        // SelectSources
        auto repSS = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.ScreenCast", "SelectSources",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/sc_sel"});
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/session/sc_sess"});
                    m.append_string("org.example.App");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(repSS.code, 0u);

        // Start
        auto repStart = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.ScreenCast", "Start",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/sc_start"});
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/session/sc_sess"});
                    m.append_string("org.example.App");
                    m.append_string("");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(repStart.code, 0u);
        const auto* sl = repStart.results["streams"].get_if<StreamList>();
        REQUIRE(sl != nullptr);
        REQUIRE(!sl->empty());
        CHECK_EQ((*sl)[0].first, 777u);
        std::cout << "  ScreenCast negotiation [PASS]" << std::endl;

        evalScript("globalThis._sc.dispose()");
    }

    // 7. Test OpenURI
    std::cout << "Testing OpenURI..." << std::endl;
    {
        // 7a. OpenURI success
        auto rRegUri = evalScript(
            "globalThis._uri = bro.portal.onOpenUri((req) => {\n"
            "  return { success: true };\n"
            "});\n"
        );
        CHECK(!rRegUri.thrown);

        auto repUri = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.OpenURI", "OpenURI",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/uri_1"});
                    m.append_string("org.example.App");
                    m.append_string("");
                    m.append_string("https://bro.example.com");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(repUri.code, 0u);
        std::cout << "  OpenURI success [PASS]" << std::endl;

        evalScript("globalThis._uri.dispose()");

        // 7b. OpenURI cancelled
        auto rRegUri2 = evalScript(
            "globalThis._uri2 = bro.portal.onOpenUri((req) => {\n"
            "  return { cancelled: true };\n"
            "});\n"
        );
        CHECK(!rRegUri2.thrown);

        auto repUri2 = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.OpenURI", "OpenURI",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/uri_2"});
                    m.append_string("org.example.App");
                    m.append_string("");
                    m.append_string("https://cancel.example.com");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) {
                    reply.read_uint32(&r.code);
                    reply.read_variant_map(&r.results);
                });
            return r;
        });
        CHECK_EQ(repUri2.code, 1u); // Cancelled
        std::cout << "  OpenURI cancelled [PASS]" << std::endl;

        evalScript("globalThis._uri2.dispose()");
    }

    // 8. Test stop() and shutdownPortalAsync()
    std::cout << "Testing stop() and shutdown..." << std::endl;
    {
        auto rStop = evalScript("bro.portal.stop()");
        CHECK(!rStop.thrown);
        CHECK(ev::isBool(rStop.value) && ev::toBool(rStop.value));

        auto rRunning = evalScript("bro.portal.isRunning()");
        CHECK(!rRunning.thrown);
        CHECK(ev::isBool(rRunning.value) && !ev::toBool(rRunning.value));
        std::cout << "  bro.portal.stop() -> stopped [PASS]" << std::endl;
    }

    broportal::api::shutdownPortalAsync();

    std::cout << "All broportal JavaScript API tests PASSED!" << std::endl;
    return 0;
}

#endif
