# broportal

[![CI](https://github.com/wlejon/broportal/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/broportal/actions/workflows/ci.yml)

An [xdg-desktop-portal](https://flatpak.github.io/xdg-desktop-portal/) backend
in C++20: the `org.freedesktop.impl.portal.*` objects a desktop shell or
Wayland compositor serves so that the portal frontend can route file pickers,
screenshots, screen casts, remote input, settings, inhibitors and global
shortcuts to it. A standalone library with its own CMake and ctest: on Linux
it links `brodbus` and `sd-bus` (`libsystemd`); on Windows and macOS it provides
clean non-Linux stubs.

broportal is the D-Bus half. The pixels, dialogs and key grabs belong to the
host: each portal that needs one asks the host through a callback, and
**without the callback the answer is an error, never a made-up result**. No
temporary file is passed off as the user's choice, no synthetic screenshot or
empty PipeWire node is offered, and no shortcut or remote input is granted
that nobody approved.

## Where it sits

Part of the **[bro](https://github.com/wlejon/bro)** desktop ecosystem (see the
[ecosystem architecture](https://github.com/wlejon/bro/blob/main/docs/ecosystem.md)).
Within the desktop stack, `broportal` implements the backend for
`xdg-desktop-portal`, allowing sandboxed (Flatpak/Snap) and native desktop
applications to interact with host desktop services (file dialogs, screencasting,
screenshots, remote desktop, and shortcut bindings). It sits beside `broseat`,
`brocred`, and `brosys`, sharing the unified `brodbus` layer on Linux.

## Platform support

Portals exist only on a Linux D-Bus session bus. broportal still configures
and builds on **Windows and macOS** with no external packages, so a cross-platform
host can link it unconditionally: there it provides the value types (`types.h`)
plus `broportal::available(&why)`, which returns false and explains why. The
backend headers `#error` off Linux rather than offering objects that cannot
work.

| Interface | Served by the backend | Needs from the host | Without the host |
|---|---|---|---|
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

## Building & Dependencies

### Prerequisites

- **CMake 3.24+** and a **C++20** compiler (MSVC 2022+, GCC 12+, Clang 15+, Apple Clang).
- **Linux**: `libsystemd` (sd-bus >= 246) and `pkg-config` (`libsystemd-dev` on Debian/Ubuntu, `systemd-libs` on Arch).
  The Linux tests also use `dbus-daemon`, `dbus-send`, `gdbus` (`libglib2.0-bin`), and `xdg-desktop-portal` when present.
- **Windows / macOS**: No external packages required.

### Resolving brodbus on Linux

On Linux, `broportal` links `brodbus` for unified D-Bus connectivity, message
serialization, and private bus test fixtures. There are no submodules: brodbus
(and bronze, for the JavaScript API) is a `bro_dependency()` pin in
`CMakeLists.txt`, resolved through `cmake/bro_deps.cmake` in this order:

1. **Existing target**: If `brodbus` is already added by a parent project.
2. **Working tree**: `../brodbus` beside the top-level project, or `-DFETCHCONTENT_SOURCE_DIR_BRODBUS=<path>`.
3. **Pinned commit**: fetched from GitHub at configure, so a plain `git clone` builds.

### Standalone build

```bash
# Linux / macOS
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure

# Windows (MSVC)
cmake -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

CMake options:
- `BROPORTAL_BUILD_TESTS`: Build tests (default `ON` when top-level, `OFF` when included via `add_subdirectory`).
- `BROPORTAL_COVERAGE`: Instrument the build for gcov coverage (GCC/Clang).
- `BROPORTAL_ENABLE_API`: Build the standalone Bronze JavaScript API (default `ON` when top-level; bronze, with brass, from `../bronze` or the head of its main branch).

### Consuming broportal

Downstream projects consume the `broportal::broportal` CMake target. Ecosystem
consumers declare it with `bro_dependency()` (`cmake/bro_deps.cmake`): a target the
outer project already added wins, else a `../broportal` working tree beside the
top-level project, else the head of its main branch, fetched at configure
(`-DFETCHCONTENT_SOURCE_DIR_BROPORTAL=<path>` points at another tree):

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bro_deps.cmake)
bro_dependency(broportal)

target_link_libraries(your_target PRIVATE broportal::broportal)
```

## API overview

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

### Thread safety and non-blocking execution

- **Thread-safe connection and setters**: Signal-emitting setters (`set_setting`,
  `notify_state_changed`, `activate_shortcut`, `complete`, `close`, ...) are
  thread-safe and can be called from any thread while `run_in_background()`
  dispatches on its worker thread. `PortalBackend` serializes access to sd-bus
  with an internal recursive mutex and eventfd wake mechanism.
- **Thread-safe callback swapping**: Handlers and listeners on all portal
  interfaces are protected by internal mutexes; swapping them mid-dispatch is
  safe and race-free.
- **Non-blocking callback execution**: Portal method requests (file pickers,
  screenshots, screencasting, remote desktop, URI opening) are offloaded to
  a background worker pool, allowing slow or modal UI dialogs to run without
  blocking the D-Bus connection or delaying other portal requests.

## Tests

Test assertions use `tests/check.h` (active in every configuration, no `assert()`).
A test that cannot run in the current environment exits code 77 with the reason
printed, and ctest reports it as skipped. Every Linux test starts its own
`dbus-daemon` fixture (via `brodbus::PrivateBus`), so tests never touch the
desktop session bus.

The test suite contains 16 test suites covering all backend interfaces and integrations:

| Test | Platform | Target / Environment | Oracle |
|---|---|---|---|
| `test_types` | everywhere | in-process | Variant types and their D-Bus signatures |
| `test_unavailable` | Windows, macOS | in-process | `available()` is false with explanatory reason |
| `test_dbus_helpers` | Linux | private `dbus-daemon` | Bus daemon queries (`GetNameOwner`, `NameHasOwner`), `dbus-send` signals |
| `test_request_session` | Linux | private `dbus-daemon` + `gdbus` | `gdbus` closing Request and Session objects; Response and Closed signals |
| `test_filechooser` | Linux | private `dbus-daemon` + client | Separate client connection and `gdbus`: errors without host, host answers relayed, option parsing |
| `test_screenshot` | Linux | private `dbus-daemon` + client | Screenshot and PickColor callbacks, URI verification, default color handling |
| `test_screencast` | Linux | private `dbus-daemon` + client | Session negotiation, source selection, stream relays with PipeWire node IDs |
| `test_remotedesktop` | Linux | private `dbus-daemon` + client | Session devices, start callback, input events, access rejection without grant |
| `test_settings` | Linux | private `dbus-daemon` + `gdbus` | `Read`, `ReadAll` namespace queries, `SettingChanged` signals |
| `test_inhibit` | Linux | private `dbus-daemon` + client | Inhibit tracking, `CreateMonitor`, `StateChanged`, `QueryEndResponse` |
| `test_openuri` | Linux | private `dbus-daemon` + client | URI opening and file opening with file descriptor passing (same inode verification) |
| `test_globalshortcuts` | Linux | private `dbus-daemon` + client | Shortcut binding sessions, triggers, activation/deactivation signals |
| `test_full_backend` | Linux | private `dbus-daemon` + introspection | Introspection of all eight interfaces, version properties, name release on `stop()` |
| `test_concurrency` | Linux | private `dbus-daemon` + worker threads | Multithreaded signal emission, mid-dispatch callback swapping, and non-blocking asynchronous modal dialog responses |
| `test_xdp_frontend` | Linux | private `dbus-daemon` + real `xdg-desktop-portal` | Distribution's `xdg-desktop-portal` frontend in front of backend: `Settings.ReadOne` and `FileChooser.OpenFile` answered through it |
| `broportal_test_api` | Linux / Windows (when API enabled) | Bronze runtime | Bronze JavaScript bindings (`broportal_api`) and garbage collection stress testing |

### Test fixtures & CI skipping

- **Private bus fixture**: All Linux tests run against an isolated private
  `dbus-daemon` instance, preventing interference with host services.
- **CI skipping**: Tests check for required tools (`dbus-daemon`, `gdbus`,
  `dbus-send`, and `xdg-desktop-portal`). If a tool is missing, the corresponding
  test exits 77 (skipped) with the reason logged.

## License

MIT, see [LICENSE](LICENSE).
