# broportal

[![CI](https://github.com/wlejon/broportal/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/broportal/actions/workflows/ci.yml)

An [xdg-desktop-portal](https://flatpak.github.io/xdg-desktop-portal/) backend
in C++20: the `org.freedesktop.impl.portal.*` objects a desktop shell or
Wayland compositor serves so that the portal frontend can route file pickers,
screenshots, screen casts, remote input, settings, inhibitors and global
shortcuts to it. A standalone library with its own CMake and ctest: no
dependency on bro or bronze, no siblings, nothing vendored; sd-bus (libsystemd)
is its one dependency.

broportal is the D-Bus half. The pixels, dialogs and key grabs belong to the
host: each portal that needs one asks the host through a callback, and
**without the callback the answer is an error, never a made-up result**. No
temporary file is passed off as the user's choice, no synthetic screenshot or
empty PipeWire node is offered, and no shortcut or remote input is granted
that nobody approved.

## Platform support

Portals exist only on a Linux D-Bus session bus. broportal still configures
and builds on **Windows and macOS** with no packages, so a cross-platform
host can link it unconditionally: there it is the value types (`types.h`)
plus `broportal::available(&why)`, which returns false and says why. The
other headers `#error` off Linux rather than offering objects that cannot
work.

| Interface | Served by the backend | Needs from the host | Without the host |
|-----------|-----------------------|---------------------|------------------|
| FileChooser | `OpenFile`, `SaveFile`, `SaveFiles`; options parsed (multiple, directory, current_name/folder/file, choices) | `set_file_picker_callback`, or preselected `set_default_selected_files` | `OtherError` |
| Screenshot | `Screenshot`, `PickColor` | `set_screenshot_callback` (a URI), `set_pick_color_callback` or `set_default_color` | `OtherError` |
| ScreenCast | sessions, `SelectSources` (types, multiple, cursor and persist modes), `Start` relaying streams as `a(ua{sv})` | `set_negotiate_callback`: the PipeWire node ids and their properties | `Start` answers `OtherError` |
| RemoteDesktop | sessions, `SelectDevices`, `Start`, all ten `Notify*` input methods | `set_start_callback` (grant, possibly narrowed), `set_input_listener` | `Start` answers `OtherError`; `Notify*` for a device the session was not granted is `AccessDenied` |
| Settings | `Read`, `ReadAll` (namespace globs), `SettingChanged` | `set_setting`, `set_color_scheme`, ... | no preference (`color-scheme` 0); no accent color |
| Inhibit | `Inhibit` (held until `Request.Close`), `CreateMonitor`, `StateChanged`, `QueryEndResponse` | `set_inhibit_change_listener`, `notify_state_changed`, `set_query_end_listener` | inhibitors are tracked and queryable |
| GlobalShortcuts | sessions, `BindShortcuts`, `ListShortcuts`, `Activated`/`Deactivated`/`ShortcutsChanged` | `set_bind_shortcuts_callback` (what was really grabbed, with `trigger_description`), `set_configure_callback` | `BindShortcuts` answers `OtherError`; `ConfigureShortcuts` is `NotSupported` |
| OpenURI | `OpenURI`, `OpenFile` (fd passing) | `set_open_uri_callback`, `set_open_file_callback` | `OtherError` |

`org.freedesktop.impl.portal.OpenURI` is **not part of the xdg-desktop-portal
specification**: the stock frontend implements OpenURI itself and never calls
a backend for it. broportal serves it for hosts that route URI opening
through their own bus clients.

## Use

```cpp
#include <broportal/broportal.h>

std::string err;
auto backend = broportal::PortalBackend::create_on_user_bus({}, &err);
backend->file_chooser().set_file_picker_callback(
    [](auto& handle, auto& app_id, auto& title, const broportal::FileChooserOptions& opts,
       std::vector<std::string>& uris, broportal::VariantMap& results) {
        // show the compositor's picker; fill uris, or answer Cancelled
        return broportal::ResponseCode::Cancelled;
    });
backend->settings().set_color_scheme(1);  // prefer dark
backend->start(&err);                     // owns org.freedesktop.impl.portal.desktop.bro
backend->run_in_background();             // or fold backend->bus().get_fd() into your loop
```

The frontend finds the backend through a portal file and `portals.conf`:

```ini
# /usr/share/xdg-desktop-portal/portals/bro.portal
[portal]
DBusName=org.freedesktop.impl.portal.desktop.bro
Interfaces=org.freedesktop.impl.portal.FileChooser;org.freedesktop.impl.portal.Settings;...

# ~/.config/xdg-desktop-portal/<desktop>-portals.conf
[preferred]
default=bro
```

Two things to know:

- **One thread drives the connection.** sd-bus connections are not
  thread-safe. With `run_in_background()`, the setters that emit signals
  (`set_setting`, `notify_state_changed`, `activate_shortcut`, ...) race the
  dispatch thread; call them from the thread that dispatches, or dispatch
  from your own loop (`process()` / `wait()`). Set callbacks before
  `start()`, or at least never while a request is being dispatched.
- **Callbacks answer synchronously.** A callback that shows a dialog holds the
  reply, and the connection, until it returns.

## Building

Linux needs `pkg-config` and `libsystemd` (`libsystemd-dev` on
Debian/Ubuntu, `systemd-libs` on Arch). Windows and macOS need nothing beyond
the compiler. The Linux tests also use `dbus-daemon`, `dbus-send`, `gdbus`
(`libglib2.0-bin`) and `xdg-desktop-portal` when present.

```bash
# Linux / macOS
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure

# Windows (MSVC, Visual Studio generator)
cmake -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`-DBROPORTAL_COVERAGE=ON` instruments a GCC/Clang build for gcov.

## Tests

Real ctests: no `assert()` (`tests/check.h` counts failures in every
configuration). Exit 77 is a skip, used only when a tool is absent, and the
test prints the reason. Every Linux test starts its own `dbus-daemon`, so
nothing touches the session bus of the machine running them.

| Test | Where | Oracle |
|------|-------|--------|
| test_types | everywhere | the variant types and their D-Bus signatures |
| test_unavailable | Windows, macOS | `available()` is false with a reason |
| test_dbus_helpers | Linux | the bus daemon itself (`GetNameOwner`, `NameHasOwner`), `dbus-send` signals |
| test_request_session | Linux | `gdbus` closing Request and Session objects; Response and Closed signals |
| test_filechooser, test_screenshot, test_screencast, test_remotedesktop, test_openuri | Linux | a separate client connection and `gdbus`: the errors without a host, the host's answers relayed, options parsed, fds passed (same inode), input refused without a grant |
| test_settings, test_inhibit, test_globalshortcuts | Linux | `gdbus` with one thread dispatching the backend; signals received on a separate connection |
| test_full_backend | Linux | introspection of all eight interfaces, `version` properties, the name released on `stop()` |
| test_xdp_frontend | Linux | the distribution's **xdg-desktop-portal** in front of the backend: `org.freedesktop.portal.Settings.ReadOne` and a `FileChooser.OpenFile` request answered through it |
