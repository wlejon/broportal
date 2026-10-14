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
        "start", "stop", "isRunning", "getStatus",
        "onFileChooser", "offFileChooser",
        "onScreenshot", "offScreenshot",
        "onPickColor", "offPickColor",
        "onScreencast", "offScreencast",
        "onOpenUri", "offOpenUri",
        "setColorScheme", "getColorScheme",
        "setAccentColor", "getAccentColor",
        "setContrast", "getContrast",
        "setReducedMotion", "getReducedMotion",
        "setSetting", "getSetting", "getSettings",
        "onSettingChanged", "offSettingChanged",
        "onInhibit", "offInhibit",
        "onQueryEndResponse", "offQueryEndResponse", "queryEndResponse",
        "onStateChanged", "offStateChanged", "notifyStateChanged",
        "getActiveInhibitions", "isInhibited",
        "onBindShortcuts", "offBindShortcuts",
        "onListShortcuts", "offListShortcuts",
        "onConfigureShortcuts", "offConfigureShortcuts",
        "triggerShortcutActivated", "triggerShortcutDeactivated", "notifyShortcutsChanged",
        "onRemoteDesktop", "offRemoteDesktop",
        "onRemoteDesktopInput", "offRemoteDesktopInput"
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

        auto rStatus = evalScript(
            "(() => {\n"
            "  const st = bro.portal.getStatus();\n"
            "  return st.available === true && st.running === true && st.interfaces.length === 8;\n"
            "})()"
        );
        CHECK(!rStatus.thrown);
        CHECK(ev::isBool(rStatus.value) && ev::toBool(rStatus.value));
        std::cout << "  bro.portal.start() & getStatus() -> running [PASS]" << std::endl;
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

    // 8. Test Settings
    std::cout << "Testing Settings..." << std::endl;
    {
        std::vector<std::pair<std::string, std::string>> dbusSettingSignals;
        auto settingSlot = f.client->add_match(
            "type='signal',interface='org.freedesktop.impl.portal.Settings',member='SettingChanged'",
            [&](dbus::Message& m) {
                std::string ns, key;
                m.read_string(&ns);
                m.read_string(&key);
                dbusSettingSignals.emplace_back(ns, key);
            });
        REQUIRE(settingSlot.is_valid());
        while (f.client->process() > 0) {}

        auto rObs = evalScript(
            "globalThis._settingEvents = [];\n"
            "globalThis._setObs = bro.portal.onSettingChanged((ev) => {\n"
            "  globalThis._settingEvents.push(ev.namespace + ':' + ev.key);\n"
            "});\n"
        );
        CHECK(!rObs.thrown);

        // setColorScheme: string ('dark', 'light', 'default') and numeric (0, 1, 2)
        CHECK(!evalScript("bro.portal.setColorScheme('dark')").thrown);
        CHECK_EQ(ev::toDouble(evalScript("bro.portal.getColorScheme()").value), 1.0);
        CHECK(!evalScript("bro.portal.setColorScheme('light')").thrown);
        CHECK_EQ(ev::toDouble(evalScript("bro.portal.getColorScheme()").value), 2.0);
        CHECK(!evalScript("bro.portal.setColorScheme(1)").thrown);
        CHECK_EQ(ev::toDouble(evalScript("bro.portal.getColorScheme()").value), 1.0);

        // Verify via D-Bus Settings.Read
        Variant dbusScheme;
        CHECK(f.call("org.freedesktop.impl.portal.Settings", "Read",
            [](dbus::Message& m) {
                m.append_string("org.freedesktop.appearance");
                m.append_string("color-scheme");
            },
            [&](dbus::Message& reply) {
                reply.read_variant(&dbusScheme);
            }));
        CHECK_EQ(dbusScheme.get_value_or<uint32_t>(99), 1u);

        // setAccentColor: [r, g, b] array and { r, g, b } object
        auto rAcc = evalScript(
            "bro.portal.setAccentColor([0.25, 0.5, 0.75]);\n"
            "const ac = bro.portal.getAccentColor();\n"
            "Math.abs(ac[0] - 0.25) < 1e-4 && Math.abs(ac[1] - 0.5) < 1e-4 && Math.abs(ac[2] - 0.75) < 1e-4 &&\n"
            "Math.abs(ac.r - 0.25) < 1e-4 && Math.abs(ac.g - 0.5) < 1e-4 && Math.abs(ac.b - 0.75) < 1e-4\n"
        );
        CHECK(!rAcc.thrown);
        CHECK(ev::isBool(rAcc.value) && ev::toBool(rAcc.value));

        Variant dbusAccent;
        CHECK(f.call("org.freedesktop.impl.portal.Settings", "Read",
            [](dbus::Message& m) {
                m.append_string("org.freedesktop.appearance");
                m.append_string("accent-color");
            },
            [&](dbus::Message& reply) {
                reply.read_variant(&dbusAccent);
            }));
        const auto* rgb = dbusAccent.get_if<RgbColor>();
        REQUIRE(rgb != nullptr);
        CHECK(std::abs(rgb->r - 0.25) < 1e-4);
        CHECK(std::abs(rgb->g - 0.5) < 1e-4);
        CHECK(std::abs(rgb->b - 0.75) < 1e-4);

        // setContrast: 'high', 'normal', 1, 0
        CHECK(!evalScript("bro.portal.setContrast('high')").thrown);
        CHECK_EQ(ev::toDouble(evalScript("bro.portal.getContrast()").value), 1.0);
        CHECK(!evalScript("bro.portal.setContrast('normal')").thrown);
        CHECK_EQ(ev::toDouble(evalScript("bro.portal.getContrast()").value), 0.0);
        CHECK(!evalScript("bro.portal.setContrast(1)").thrown);

        // setSetting & getSettings
        auto rCustomSet = evalScript(
            "bro.portal.setSetting('org.gnome.desktop.interface', 'gtk-theme', 'Adwaita-dark');\n"
            "const all = bro.portal.getSettings();\n"
            "all['org.freedesktop.appearance']['color-scheme'] === 1 &&\n"
            "all['org.freedesktop.appearance']['contrast'] === 1 &&\n"
            "all['org.gnome.desktop.interface']['gtk-theme'] === 'Adwaita-dark' &&\n"
            "bro.portal.getSetting('org.gnome.desktop.interface', 'gtk-theme') === 'Adwaita-dark'\n"
        );
        CHECK(!rCustomSet.thrown);
        CHECK(ev::isBool(rCustomSet.value) && ev::toBool(rCustomSet.value));

        // Pump D-Bus client and JS observer
        broportal::api::tickPortalAsync();
        for (int i = 0; i < 40 && dbusSettingSignals.size() < 4; ++i) {
            f.client->wait(10000);
            while (f.client->process() > 0) {}
        }
        CHECK(dbusSettingSignals.size() >= 4);

        auto rObsCount = evalScript("globalThis._settingEvents.length >= 4");
        CHECK(!rObsCount.thrown);
        CHECK(ev::isBool(rObsCount.value) && ev::toBool(rObsCount.value));
        evalScript("globalThis._setObs.dispose()");
        std::cout << "  Settings (setColorScheme, setAccentColor, setContrast, setSetting, getSettings, SettingChanged) [PASS]" << std::endl;
    }

    // 9. Test Inhibit
    std::cout << "Testing Inhibit..." << std::endl;
    {
        auto rInhReg = evalScript(
            "globalThis._inhEvents = [];\n"
            "globalThis._qeEvents = [];\n"
            "globalThis._stEvents = [];\n"
            "globalThis._inhH = bro.portal.onInhibit((ev) => {\n"
            "  globalThis._inhEvents.push({ added: ev.added, appId: ev.appId, flags: ev.flags, reason: ev.reason, suspend: ev.suspend, idle: ev.idle });\n"
            "});\n"
            "globalThis._qeH = bro.portal.onQueryEndResponse((ev) => {\n"
            "  globalThis._qeEvents.push(ev.sessionHandle);\n"
            "});\n"
            "globalThis._stH = bro.portal.onStateChanged((ev) => {\n"
            "  globalThis._stEvents.push(ev.sessionState);\n"
            "});\n"
        );
        CHECK(!rInhReg.thrown);

        const std::string inhReq = "/org/freedesktop/portal/desktop/request/api_inh_1";
        CHECK(f.call("org.freedesktop.impl.portal.Inhibit", "Inhibit",
            [&](dbus::Message& m) {
                m.append_object_path(ObjectPath{inhReq});
                m.append_string("org.example.VideoPlayer");
                m.append_string("main_win");
                m.append_uint32(12u); // Suspend (4) | Idle (8)
                VariantMap opts;
                opts["reason"] = Variant(std::string("Playing movie"));
                m.append_variant_map(opts);
            },
            nullptr));

        broportal::api::tickPortalAsync();

        auto rInhCheck = evalScript(
            "globalThis._inhEvents.length === 1 &&\n"
            "globalThis._inhEvents[0].added === true &&\n"
            "globalThis._inhEvents[0].appId === 'org.example.VideoPlayer' &&\n"
            "globalThis._inhEvents[0].flags === 12 &&\n"
            "globalThis._inhEvents[0].reason === 'Playing movie' &&\n"
            "globalThis._inhEvents[0].suspend === true &&\n"
            "globalThis._inhEvents[0].idle === true &&\n"
            "bro.portal.isInhibited('suspend') === true &&\n"
            "bro.portal.isInhibited('idle') === true &&\n"
            "bro.portal.isInhibited('logout') === false &&\n"
            "bro.portal.getActiveInhibitions().length === 1\n"
        );
        CHECK(!rInhCheck.thrown);
        CHECK(ev::isBool(rInhCheck.value) && ev::toBool(rInhCheck.value));

        // Close inhibition request via D-Bus Request.Close
        CHECK(f.client->call_method(f.config.bus_name, inhReq, "org.freedesktop.impl.portal.Request", "Close",
                                    nullptr, nullptr));
        for (int i = 0; i < 20; ++i) {
            broportal::api::tickPortalAsync();
            if (!f.backend->inhibit().is_inhibited(InhibitFlag::Idle)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        broportal::api::tickPortalAsync();

        auto rInhRemoved = evalScript(
            "globalThis._inhEvents.length === 2 &&\n"
            "globalThis._inhEvents[1].added === false &&\n"
            "bro.portal.isInhibited() === false\n"
        );
        CHECK(!rInhRemoved.thrown);
        CHECK(ev::isBool(rInhRemoved.value) && ev::toBool(rInhRemoved.value));

        // CreateMonitor, notifyStateChanged, QueryEndResponse
        const std::string monSess = "/org/freedesktop/portal/desktop/session/api_inh_mon_1";
        uint32_t monCode = 999;
        CHECK(f.call("org.freedesktop.impl.portal.Inhibit", "CreateMonitor",
            [&](dbus::Message& m) {
                m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/api_inh_mon_req"});
                m.append_object_path(ObjectPath{monSess});
                m.append_string("org.example.VideoPlayer");
                m.append_string("");
            },
            [&](dbus::Message& reply) {
                reply.read_uint32(&monCode);
            }));
        CHECK_EQ(monCode, 0u);

        uint32_t seenSessionState = 0;
        bool seenScreensaver = false;
        auto stateSlot = f.client->add_match(
            "type='signal',interface='org.freedesktop.impl.portal.Inhibit',member='StateChanged'",
            [&](dbus::Message& m) {
                ObjectPath p;
                VariantMap st;
                m.read_object_path(&p);
                m.read_variant_map(&st);
                seenScreensaver = get_bool_or(st, "screensaver-active", false);
                seenSessionState = get_uint32_or(st, "session-state", 0);
            });
        REQUIRE(stateSlot.is_valid());
        while (f.client->process() > 0) {}

        CHECK(!evalScript("bro.portal.notifyStateChanged(true, 'query-end')").thrown);
        broportal::api::tickPortalAsync();
        for (int i = 0; i < 40 && seenSessionState == 0; ++i) {
            f.client->wait(10000);
            while (f.client->process() > 0) {}
        }
        CHECK(seenScreensaver);
        CHECK_EQ(seenSessionState, static_cast<uint32_t>(SessionState::QueryEnd));

        CHECK(f.call("org.freedesktop.impl.portal.Inhibit", "QueryEndResponse",
            [&](dbus::Message& m) {
                m.append_object_path(ObjectPath{monSess});
            },
            nullptr));
        broportal::api::tickPortalAsync();

        auto rQeCheck = evalScript(
            "globalThis._qeEvents.length === 1 && globalThis._qeEvents[0] === '" + monSess + "' &&\n"
            "globalThis._stEvents.length === 1 && globalThis._stEvents[0] === 2\n"
        );
        CHECK(!rQeCheck.thrown);
        CHECK(ev::isBool(rQeCheck.value) && ev::toBool(rQeCheck.value));

        evalScript("globalThis._inhH.dispose(); globalThis._qeH.dispose(); globalThis._stH.dispose();");
        std::cout << "  Inhibit (onInhibit, notifyStateChanged, onStateChanged, onQueryEndResponse) [PASS]" << std::endl;
    }

    // 10. Test GlobalShortcuts
    std::cout << "Testing GlobalShortcuts..." << std::endl;
    {
        auto rGsReg = evalScript(
            "globalThis._gsBind = bro.portal.onBindShortcuts((req) => ({\n"
            "  shortcuts: req.shortcuts.map((s) => ({ id: s.id, description: s.description, triggerDescription: 'Ctrl+Shift+' + (s.preferredTrigger || 'K') }))\n"
            "}));\n"
        );
        CHECK(!rGsReg.thrown);

        const std::string gsSess = "/org/freedesktop/portal/desktop/session/api_gs_sess_1";
        auto repGsCreate = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.GlobalShortcuts", "CreateSession",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/api_gs_cs"});
                    m.append_object_path(ObjectPath{gsSess});
                    m.append_string("org.example.ShortcutsApp");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) { reply.read_uint32(&r.code); reply.read_variant_map(&r.results); });
            return r;
        });
        CHECK_EQ(repGsCreate.code, 0u);

        std::vector<std::string> gsSignals;
        auto gsSlot = f.client->add_match(
            "type='signal',interface='org.freedesktop.impl.portal.GlobalShortcuts'",
            [&](dbus::Message& m) {
                ObjectPath p;
                m.read_object_path(&p);
                std::string member = sd_bus_message_get_member(m.raw());
                if (member == "Activated" || member == "Deactivated") {
                    std::string id;
                    uint64_t ts = 0;
                    m.read_string(&id);
                    m.read_uint64(&ts);
                    gsSignals.push_back(member + ":" + id + ":" + std::to_string(ts));
                }
            });
        REQUIRE(gsSlot.is_valid());
        while (f.client->process() > 0) {}

        auto repGsBind = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.GlobalShortcuts", "BindShortcuts",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/api_gs_bind"});
                    m.append_object_path(ObjectPath{gsSess});
                    ShortcutList wanted;
                    VariantMap p;
                    p["description"] = Variant(std::string("Mute Mic"));
                    p["preferred_trigger"] = Variant(std::string("M"));
                    wanted.emplace_back("mute_mic", p);
                    m.append_shortcut_list(wanted);
                    m.append_string("");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) { reply.read_uint32(&r.code); reply.read_variant_map(&r.results); });
            return r;
        });
        CHECK_EQ(repGsBind.code, 0u);
        const auto* boundList = repGsBind.results["shortcuts"].get_if<ShortcutList>();
        REQUIRE(boundList != nullptr && boundList->size() == 1);
        CHECK_EQ((*boundList)[0].first, std::string("mute_mic"));
        CHECK_EQ(get_string((*boundList)[0].second, "trigger_description").value_or(""), std::string("Ctrl+Shift+M"));

        auto rGsListReg = evalScript("globalThis._gsList = bro.portal.onListShortcuts((req) => ({ shortcuts: req.shortcuts }));");
        CHECK(!rGsListReg.thrown);

        auto repGsList = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.GlobalShortcuts", "ListShortcuts",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/api_gs_list"});
                    m.append_object_path(ObjectPath{gsSess});
                },
                [&](dbus::Message& reply) { reply.read_uint32(&r.code); reply.read_variant_map(&r.results); });
            return r;
        });
        CHECK_EQ(repGsList.code, 0u);
        const auto* listed = repGsList.results["shortcuts"].get_if<ShortcutList>();
        REQUIRE(listed != nullptr && listed->size() == 1);
        CHECK_EQ((*listed)[0].first, std::string("mute_mic"));

        CHECK(!evalScript("bro.portal.triggerShortcutActivated('" + gsSess + "', 'mute_mic', 555000)").thrown);
        CHECK(!evalScript("bro.portal.triggerShortcutDeactivated('" + gsSess + "', 'mute_mic', 556000)").thrown);

        for (int i = 0; i < 40 && gsSignals.size() < 2; ++i) {
            f.client->wait(10000);
            while (f.client->process() > 0) {}
        }
        REQUIRE(gsSignals.size() >= 2);
        CHECK_EQ(gsSignals[0], std::string("Activated:mute_mic:555000"));
        CHECK_EQ(gsSignals[1], std::string("Deactivated:mute_mic:556000"));

        evalScript("globalThis._gsBind.dispose(); globalThis._gsList.dispose();");
        std::cout << "  GlobalShortcuts (onBindShortcuts, onListShortcuts, triggerShortcutActivated/Deactivated) [PASS]" << std::endl;
    }

    // 11. Test RemoteDesktop
    std::cout << "Testing RemoteDesktop..." << std::endl;
    {
        auto rRdReg = evalScript(
            "globalThis._rdInputs = [];\n"
            "globalThis._rdStart = bro.portal.onRemoteDesktop((req) => ({ devices: req.devices, clipboardEnabled: true }));\n"
            "globalThis._rdIn = bro.portal.onRemoteDesktopInput((ev) => { globalThis._rdInputs.push(ev); });\n"
        );
        CHECK(!rRdReg.thrown);

        const std::string rdSess = "/org/freedesktop/portal/desktop/session/api_rd_sess_1";
        auto repRdCreate = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.RemoteDesktop", "CreateSession",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/api_rd_cs"});
                    m.append_object_path(ObjectPath{rdSess});
                    m.append_string("org.example.RemoteApp");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) { reply.read_uint32(&r.code); reply.read_variant_map(&r.results); });
            return r;
        });
        CHECK_EQ(repRdCreate.code, 0u);

        auto repRdSelect = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.RemoteDesktop", "SelectDevices",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/api_rd_sel"});
                    m.append_object_path(ObjectPath{rdSess});
                    m.append_string("org.example.RemoteApp");
                    VariantMap opts;
                    opts["types"] = Variant(static_cast<uint32_t>(3)); // Keyboard (1) | Pointer (2)
                    m.append_variant_map(opts);
                },
                [&](dbus::Message& reply) { reply.read_uint32(&r.code); reply.read_variant_map(&r.results); });
            return r;
        });
        CHECK_EQ(repRdSelect.code, 0u);

        auto repRdStart = callWithPump([&]() {
            Reply r;
            f.call("org.freedesktop.impl.portal.RemoteDesktop", "Start",
                [&](dbus::Message& m) {
                    m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/api_rd_start"});
                    m.append_object_path(ObjectPath{rdSess});
                    m.append_string("org.example.RemoteApp");
                    m.append_string("");
                    m.append_variant_map({});
                },
                [&](dbus::Message& reply) { reply.read_uint32(&r.code); reply.read_variant_map(&r.results); });
            return r;
        });
        CHECK_EQ(repRdStart.code, 0u);
        CHECK_EQ(get_uint32_or(repRdStart.results, "devices", 0), 3u);
        CHECK(get_bool_or(repRdStart.results, "clipboard_enabled", false));

        CHECK(f.call("org.freedesktop.impl.portal.RemoteDesktop", "NotifyKeyboardKeycode",
            [&](dbus::Message& m) { m.append_object_path(ObjectPath{rdSess}); m.append_variant_map({}); m.append_int32(30); m.append_uint32(1); }, nullptr));
        CHECK(f.call("org.freedesktop.impl.portal.RemoteDesktop", "NotifyPointerMotion",
            [&](dbus::Message& m) { m.append_object_path(ObjectPath{rdSess}); m.append_variant_map({}); m.append_double(15.5); m.append_double(-6.25); }, nullptr));

        broportal::api::tickPortalAsync();

        auto rRdInCheck = evalScript(
            "globalThis._rdInputs.length === 2 &&\n"
            "globalThis._rdInputs[0].type === 'keyboard_keycode' && globalThis._rdInputs[0].keycode === 30 && globalThis._rdInputs[0].state === 1 &&\n"
            "globalThis._rdInputs[1].type === 'pointer_motion' && globalThis._rdInputs[1].dx === 15.5 && globalThis._rdInputs[1].dy === -6.25\n"
        );
        CHECK(!rRdInCheck.thrown);
        CHECK(ev::isBool(rRdInCheck.value) && ev::toBool(rRdInCheck.value));

        evalScript("globalThis._rdStart.dispose(); globalThis._rdIn.dispose();");
        std::cout << "  RemoteDesktop (onRemoteDesktop, onRemoteDesktopInput) [PASS]" << std::endl;
    }

    // 12. Test stop() and shutdownPortalAsync()
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
