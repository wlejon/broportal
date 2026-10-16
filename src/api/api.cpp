#include "api.h"
#include "host_portal_internal.h"
#include "broportal/availability.h"

#include <mutex>

namespace broportal::api {

namespace {

std::mutex g_backend_mu;
#if defined(__linux__)
std::shared_ptr<broportal::PortalBackend> g_custom_backend;
broportal::PortalBackend* g_custom_backend_raw = nullptr;
std::shared_ptr<broportal::PortalBackend> g_default_backend;
#endif

} // namespace

#if defined(__linux__)
std::shared_ptr<broportal::PortalBackend> activeBackend() {
    std::lock_guard lock(g_backend_mu);
    if (g_custom_backend) return g_custom_backend;
    if (g_custom_backend_raw) {
        return std::shared_ptr<broportal::PortalBackend>(
            std::shared_ptr<void>{}, g_custom_backend_raw);
    }
    if (!g_default_backend) {
        std::string err;
        g_default_backend = broportal::PortalBackend::create_on_user_bus({}, &err);
        if (g_default_backend) {
            attachBackendHandlers(*g_default_backend);
        }
    }
    return g_default_backend;
}

void attachBackendHandlers(broportal::PortalBackend& backend) {
    backend.set_file_chooser_handler(handleNativeFilePicker);
    backend.set_screenshot_handler(handleNativeScreenshot);
    backend.screenshot().set_pick_color_callback(handleNativePickColor);
    backend.set_screencast_handler(handleNativeScreenCast);
    backend.set_open_uri_handler(handleNativeOpenUri);
    backend.open_uri().set_open_file_callback(handleNativeOpenFile);
    attachSettingsBackend(backend);
    attachInhibitBackend(backend);
    attachGlobalShortcutsBackend(backend);
    attachRemoteDesktopBackend(backend);
}

void setBackend(std::shared_ptr<broportal::PortalBackend> backend) {
    std::lock_guard lock(g_backend_mu);
    g_custom_backend = std::move(backend);
    g_custom_backend_raw = nullptr;
    if (g_custom_backend) {
        attachBackendHandlers(*g_custom_backend);
    }
}

void setBackend(broportal::PortalBackend* backend) {
    std::lock_guard lock(g_backend_mu);
    g_custom_backend.reset();
    g_custom_backend_raw = backend;
    if (g_custom_backend_raw) {
        attachBackendHandlers(*g_custom_backend_raw);
    }
}

std::shared_ptr<broportal::PortalBackend> getBackend() {
    return activeBackend();
}
#else
std::shared_ptr<broportal::PortalBackend> getBackend() {
    return nullptr;
}

void setBackend(std::shared_ptr<broportal::PortalBackend>) {}
void setBackend(broportal::PortalBackend*) {}
#endif

Value ensureBroPortal() {
    ev::Persistent globalThisVal;
    auto gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        globalThisVal.set(gt.value);
    }

    ev::Persistent broP;
    auto bro = ev::globalValue("bro");
    if (bro.found && ev::isObject(bro.value)) broP.set(bro.value);
    if (!ev::isObject(broP.get()) && ev::isObject(globalThisVal.get())) {
        Value candidate = ev::getProperty(globalThisVal.get(), "bro");
        if (ev::isObject(candidate)) broP.set(candidate);
    }
    if (!ev::isObject(broP.get())) {
        broP.set(ev::createObject());
        ev::registerGlobal("bro", broP.get());
        if (ev::isObject(globalThisVal.get())) {
            globalThisVal.set(ev::setProperty(globalThisVal.get(), "bro", broP.get()));
        }
    }

    ev::Persistent portalP(ev::getProperty(broP.get(), "portal"));
    if (!ev::isObject(portalP.get())) {
        portalP.set(ev::createObject());
        broP.set(ev::setProperty(broP.get(), "portal", portalP.get()));
    }
    return portalP.get();
}

void installPortal() {
    ev::Persistent portalObj(ensureBroPortal());
    ObjectBuilder portal(portalObj.get());

    std::string why;
    bool is_avail = broportal::available(&why);
    portal.set("available", is_avail);

    if (!is_avail) {
        portal.set("reason", why);
        portal.set("interfaces", ev::makeArray(0));
        portal.def("start", 0, [](Value, std::span<const Value>) -> Value { return ev::fromBool(false); });
        portal.def("stop", 0, [](Value, std::span<const Value>) -> Value { return ev::fromBool(false); });
        portal.def("isRunning", 0, [](Value, std::span<const Value>) -> Value { return ev::fromBool(false); });
        portal.def("getStatus", 0, [why](Value, std::span<const Value>) -> Value {
            ObjectBuilder st;
            st.set("available", false);
            st.set("running", false);
            st.set("reason", why);
            st.set("interfaces", ev::makeArray(0));
            return st.get();
        });
        const char* unavailMethods[] = {
            "onFileChooser", "onScreenshot", "onPickColor", "onScreencast", "onOpenUri",
            "onRemoteDesktop", "onRemoteDesktopStart", "onRemoteDesktopInput",
            "onBindShortcuts", "onListShortcuts", "onConfigureShortcuts",
            "triggerShortcutActivated", "triggerShortcutDeactivated", "notifyShortcutsChanged",
            "onInhibit", "onQueryEndResponse", "queryEndResponse", "onStateChanged", "notifyStateChanged",
            "setColorScheme", "setAccentColor", "setContrast", "setReducedMotion", "setSetting", "getSetting", "getSettings", "onSettingChanged"
        };
        for (const char* m : unavailMethods) {
            portal.def(m, 1, [](Value, std::span<const Value>) -> Value {
                return ev::throwError("Portal service is unavailable on this platform");
            });
        }
        return;
    }

#if defined(__linux__)
    auto makeInterfacesArray = []() -> Value {
        static constexpr const char* kIfaces[] = {
            "org.freedesktop.impl.portal.FileChooser",
            "org.freedesktop.impl.portal.Screenshot",
            "org.freedesktop.impl.portal.ScreenCast",
            "org.freedesktop.impl.portal.RemoteDesktop",
            "org.freedesktop.impl.portal.Settings",
            "org.freedesktop.impl.portal.Inhibit",
            "org.freedesktop.impl.portal.GlobalShortcuts",
            "org.freedesktop.impl.portal.OpenURI"
        };
        ev::Persistent arr(ev::makeArray(8));
        for (uint32_t i = 0; i < 8; ++i) {
            ev::Persistent elem(ev::fromUtf8(kIfaces[i]));
            arr.set(ev::setElement(arr.get(), i, elem.get()));
        }
        return arr.get();
    };
    portal.set("interfaces", makeInterfacesArray());

    // bro.portal.start() -> boolean
    portal.def("start", 0, [](Value, std::span<const Value>) -> Value {
        auto b = activeBackend();
        if (!b) return ev::fromBool(false);
        if (b->is_running()) return ev::fromBool(true);

        std::string err;
        if (!b->start(&err)) {
            return ev::fromBool(false);
        }
        attachBackendHandlers(*b);
        b->run_in_background();
        return ev::fromBool(true);
    });

    // bro.portal.stop() -> boolean
    portal.def("stop", 0, [](Value, std::span<const Value>) -> Value {
        shutdownPortalAsync();
        return ev::fromBool(true);
    });

    // bro.portal.isRunning() -> boolean
    portal.def("isRunning", 0, [](Value, std::span<const Value>) -> Value {
        auto b = activeBackend();
        return ev::fromBool(b && b->is_running());
    });

    // bro.portal.getStatus() -> object
    portal.def("getStatus", 0, [makeInterfacesArray](Value, std::span<const Value>) -> Value {
        auto b = activeBackend();
        ObjectBuilder st;
        st.set("available", true);
        st.set("running", static_cast<bool>(b && b->is_running()));
        st.set("busName", b ? b->config().bus_name : "");
        st.set("bus_name", b ? b->config().bus_name : "");
        st.set("objectPath", b ? b->config().object_path : "");
        st.set("object_path", b ? b->config().object_path : "");
        st.set("interfaces", makeInterfacesArray());
        return st.get();
    });

    installFileChooserOnto(portal.get());
    installScreenshotOnto(portal.get());
    installScreenCastOnto(portal.get());
    installOpenUriOnto(portal.get());
    installSettingsOnto(portal.get());
    installInhibitOnto(portal.get());
    installGlobalShortcutsOnto(portal.get());
    installRemoteDesktopOnto(portal.get());
#endif
}

void tickPortalAsync() {
#if defined(__linux__)
    // The backend that exists, if any: the tick never makes one (a session
    // bus connection and a worker pool), only bro.portal's calls do.
    std::shared_ptr<broportal::PortalBackend> b;
    {
        std::lock_guard lock(g_backend_mu);
        if (g_custom_backend) b = g_custom_backend;
        else if (g_custom_backend_raw)
            b = std::shared_ptr<broportal::PortalBackend>(std::shared_ptr<void>{}, g_custom_backend_raw);
        else b = g_default_backend;
    }
    if (b && b->is_running()) {
        b->process();
    }

    drainFileChooser();
    drainScreenshot();
    drainScreenCast();
    drainOpenUri();
    drainSettings();
    drainInhibit();
    drainGlobalShortcuts();
    drainRemoteDesktop();
#endif

    ev::drainMicrotasks();
}

void shutdownPortalAsync() {
#if defined(__linux__)
    shutdownFileChooser();
    shutdownScreenshot();
    shutdownScreenCast();
    shutdownOpenUri();
    shutdownSettings();
    shutdownInhibit();
    shutdownGlobalShortcuts();
    shutdownRemoteDesktop();

    auto b = activeBackend();
    if (b && b->is_running()) {
        b->stop_background();
        b->stop();
    }
#endif
}

} // namespace broportal::api
