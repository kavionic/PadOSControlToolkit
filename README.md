# PadOS Control Toolkit

A Qt-based toolkit for building host applications for PadOS devices, with reusable
components for serial communication, log viewing, firmware updates, and SD-card
management, plus a minimal reference application.

## Components

- `DeviceSession` owns a serial connection, device probing, application/bootloader
  state, time synchronization, and the optional shell and mounted-filesystem integrations.
- `SerialHandler` provides typed message transmission and packet-handler registration,
  including application-specific messages defined outside the toolkit.
- `LogView`, `FileBrowser`, `FirmwareUpdater`, and `SDCardSync` are ordinary Qt widgets.
  Each is attached to a `DeviceSession` with `SetDeviceSession()`.
- `ConnectionToolbar` is a reusable `QToolBar` constructed with a `DeviceSession`.
  It provides port selection, refresh/reconnect actions, and connection status, and
  remembers the selected port in application settings. Add it
  to a `QMainWindow` and use its `toggleViewAction()` for a checkable menu entry.
- The asynchronous filesystem helpers and SD synchronization states are also available
  independently of the standard window.
- `ControlWindow` assembles the standard tools in dock widgets, with connection controls,
  a View menu for panel and toolbar visibility, layout persistence, and Reset Layout.
  Closing a dock hides its panel;
  it does not destroy it or cancel an operation.
- `Applications/PadOSControl/main.cpp` is the complete reference application.

The initial build produces one static library. Core, widget, and integration sources
are organized separately without requiring consumers to coordinate multiple libraries.

## Build on Windows

The initial supported configuration is Visual Studio 2022, C++23, Qt 6.8.1 MSVC x64,
and Qt Visual Studio Tools / Qt MSBuild. Both Debug and Release configurations are supplied.
Clone with `--recurse-submodules`, or initialize an existing checkout with:

```powershell
git submodule update --init --recursive
```

The first build automatically bootstraps the bundled vcpkg and installs libssh, its
OpenSSL dependency, and the WinFsp SDK into `Build/vcpkg_installed/`. It downloads the
required sources and supporting build tools as needed and reuses cached packages when
available. The vcpkg submodule and `vcpkg.json` pin the tool and dependency versions.
`vcpkg-configuration.json` selects the toolkit's WinFsp package recipe under
`MSBuild/VcpkgPorts/winfsp/`. That recipe extracts the SDK from the checksum-pinned
official WinFsp release without running its installer.

Set `PadOSRoot` explicitly for this workspace in `MSBuild/Local.props` (example below)
or pass it to MSBuild. Select the PadOS checkout or installation whose protocol headers
match the device; multiple versions can coexist without changing a global environment variable.
The selected root must contain `Include/SerialConsole/BootloaderMessages.h`.

Open `PadOSControlToolkit.sln` after configuring `MSBuild/Local.props`, or use an MSBuild-enabled shell:

```powershell
msbuild PadOSControlToolkit.sln /m /p:Configuration=Debug /p:Platform=x64 /p:PadOSRoot=C:\Projects\PadOS\
```

The solution builds the library, reference application, and `ToolkitSmoke` test. Outputs are placed in
`Build/x64/Debug` or `Build/x64/Release`. Run from a Qt-enabled environment or deploy
the Qt runtime beside the executable with `windeployqt` before launching it elsewhere.
Libssh runtime DLLs are copied beside application outputs automatically.

Dependencies and integration options are defined in `MSBuild/PadOSControlToolkit.props`.
Paths must include their trailing directory separator. Override them using MSBuild
properties or a local, ignored `MSBuild/Local.props` file:

| Property | Purpose | Default |
| --- | --- | --- |
| `PadOSRoot` | PadOS checkout or installation containing the shared serial and bootloader protocol headers. | Required; no default. |
| `LibsshRoot` | Installed libssh headers, libraries, and runtime DLLs. | `Build/vcpkg_installed/x64-windows/` |
| `WinFspRoot` | WinFsp SDK headers and import library. | `Build/vcpkg_installed/x64-windows/` |
| `ToolkitWithSsh` | Build and start the SSH shell bridge. | `true` |
| `ToolkitWithWinFsp` | Build Windows mounted-filesystem support. | `true` |

`LibsshRoot` and `WinFspRoot` can point to external installations. WinFsp headers may
be under `include/` (vcpkg) or `inc/` (the WinFsp installer), with libraries under `lib/`.
Automatic restoration installs the manifest's dependencies when either enabled
integration uses its default dependency location.

For example:

```xml
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <PadOSRoot>C:\Projects\PadOS\</PadOSRoot>
    <FirmwareSourceRoot>C:\Projects\DeviceFirmware\Firmware\Source\</FirmwareSourceRoot>
  </PropertyGroup>
</Project>
```

Mounting requires a separate [WinFsp runtime installation](https://winfsp.dev/rel/)
on the machine running the application. The build restores only SDK headers and import
libraries; it does not install the driver or copy WinFsp runtime DLLs beside the executable.
The application also runs without the runtime, with mounting unavailable. Keep integration
settings consistent between the library and consumers because they affect public class layouts.

## Using the toolkit

Reference `PadOSControlToolkit.vcxproj` from your application and import
`MSBuild/PadOSControlToolkit.props` after Qt's property sheets and
`MSBuild/PadOSControlToolkit.targets` after Qt's build targets. Include public headers
with the `PadOSControl/` prefix followed by `Core/`, `Widgets/`, or `Integrations/`.
The paths under `Include/PadOSControl/` mirror those under `Source/`; for example,
`Widgets/LogView/LogView.h` corresponds to `Widgets/LogView/LogView.cpp`. The private
precompiled header remains in `Source/`. Set `PadOSControlToolkitRoot` to use another checkout
location, including a submodule.

For a standard application, follow `Applications/PadOSControl/main.cpp`. To extend it,
construct a `ControlWindow`, create your own widgets using its `GetDeviceSession()`,
and register them with `AddPanel()` before calling `Start()`. Give each dock a stable,
unique identifier so saved layouts can be restored. Panels share one dock area; arrange
rows and columns with `splitDockWidget()` and groups of tabs with `tabifyDockWidget()`.
This keeps the dividers resizable in windows without a central widget. A custom central
widget can also be installed with the normal `QMainWindow` API. Use `GetFileBrowserDock()`,
`GetFirmwareUpdaterDock()`, `GetSDCardSyncDock()`, and `GetLogViewDock()` with Qt
docking functions to arrange the standard tools alongside application panels.

For a custom layout, create a `DeviceSession`, attach the toolkit widgets, and call
`Start()` after all packet handlers and signal connections are registered. Call
`Stop()` before shutting down, and destroy the panels before the session. Use one
binding per widget instance. Session and widget calls belong on the Qt GUI thread;
the filesystem and SSH integrations marshal their own worker-thread traffic.

Pass `DeviceSessionOptions` to `ControlWindow` or to a standalone `DeviceSession` to
configure filesystem and temporary-directory names. Options are copied at construction
and shared by the session's filesystem integration and attached panels.

```cpp
DeviceSessionOptions options;
options.FilesystemName = "MyDevice";
options.FilesystemPrefix = "\\MyDevice\\sdcard";
options.TemporaryDirectoryName = "MyDevice";
ControlWindow window(options);
```

The defaults are `PadOS` for the filesystem name and volume label, `\PadOS\sdcard`
for the share prefix, and `PadOS` for temporary-directory naming. `FileBrowser` places
preview downloads under `<system-temp>/PadOS/`, and `SDCardSync` creates temporary
comparison folders named `PadOS_SDCardDiff_XXXXXX`. `TemporaryDirectoryName` must be
a single directory name, not a full path. WinFsp limits the filesystem name to 15 UTF-16
code units and the share prefix to 191; mounting fails if either limit is exceeded.

## Verification

Run `Build/x64/Debug/ToolkitSmoke.exe` from a Qt-enabled environment after building.
The smoke test uses an offscreen window and temporary settings. It checks the preserved
bootloader wire IDs and packet sizes, standard and custom docks, layout persistence,
session-to-panel logging, and standalone widget construction/destruction. It does not
start a device connection, SSH listener, mount, or firmware update. An optional first
argument writes a PNG rendering of the window.

Live serial communication, firmware flashing, SSH bridging, and mounted-file operations
still require verification against hardware.
