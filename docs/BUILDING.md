# Building Lumen Photo Studio

Everything here is driven by the single top-level `CMakeLists.txt`. There are
no submodules, no code generation steps and no vendored dependencies.

> **This development machine currently has no toolchain.** Qt was uninstalled
> and `C:/Qt` no longer exists; there is no compiler and no CMake on `PATH`.
> Nothing in this repository can be configured, compiled or run locally until
> Qt 6 and a C++20 compiler are reinstalled. CI is the only place these
> instructions are actually exercised.

---

## Requirements

| Component | Minimum | Notes |
|---|---|---|
| CMake | 3.21 | `cmake_minimum_required(VERSION 3.21)` |
| C++ compiler | C++20 | MSVC 19.30+ (VS 2022), GCC 10+, Clang 13+, Apple Clang 13+ |
| Qt | 6.2 | Components: `Core`, `Gui`, `Widgets`, `Concurrent` |
| LibRaw | any | **Optional.** Without it RAW decoding is compiled out |
| pkg-config | any | Optional, only used to help locate LibRaw |

Two targets are produced:

- `lps_engine` — static library, all of `src/`
- `LumenPhotoStudio` — the Qt Widgets application, all of `ui/` + `resources/`

The CMake target name and the executable `OUTPUT_NAME` are both
`LumenPhotoStudio` on every platform.

---

## Where the binary lands

`CMAKE_RUNTIME_OUTPUT_DIRECTORY` is set to `${CMAKE_BINARY_DIR}/bin`.

| Platform | Generator | Path relative to the build directory |
|---|---|---|
| Linux / BSD | Ninja, Unix Makefiles | `bin/LumenPhotoStudio` |
| Windows | Ninja, NMake | `bin/LumenPhotoStudio.exe` |
| Windows | Visual Studio (multi-config) | `bin/<Config>/LumenPhotoStudio.exe` |
| macOS | Ninja, Unix Makefiles | `bin/LumenPhotoStudio.app/Contents/MacOS/LumenPhotoStudio` |
| macOS | Xcode (multi-config) | `bin/<Config>/LumenPhotoStudio.app/Contents/MacOS/LumenPhotoStudio` |

`<Config>` is `Debug`, `Release`, `RelWithDebInfo` or `MinSizeRel`.

Prefer Ninja in CI — it is single-config, so the artifact path has no
`<Config>` component and does not depend on the generator's layout.

---

## CMake options

| Option | Default | Purpose |
|---|---|---|
| `CMAKE_BUILD_TYPE` | *(empty)* | Set it. Single-config generators otherwise build unoptimised with no build type |
| `CMAKE_PREFIX_PATH` | — | Point at the Qt 6 prefix if Qt is not on the default search path |
| `CMAKE_INSTALL_PREFIX` | platform default | Staging root for `cmake --install` |
| `LPS_APP_NAME` | `Lumen Photo Studio` | Display name (plist, VERSIONINFO, `QCoreApplication::applicationName`) |
| `LPS_APP_VERSION` | `${PROJECT_VERSION}` (`1.1.0`) | Reported by `QCoreApplication::applicationVersion()` |
| `LPS_COMPANY_NAME` | `Lumen` | `QCoreApplication::organizationName()` |
| `LPS_APP_COPYRIGHT` | `Copyright (c) 2026 Lumen Photo Studio contributors.` | Embedded in plist + VERSIONINFO |
| `LPS_APP_ICON_PATH` | `resources/icons/lumen_logo_512.png` | Source PNG installed into the Linux hicolor theme |
| `LPS_INSTALL_DATA_DIR` | see below | Runtime data dir, relative to `CMAKE_INSTALL_PREFIX` |
| `LIBRAW_ROOT` | — | Extra hint for LibRaw when it lives in a non-standard prefix |

`LPS_INSTALL_DATA_DIR` defaults to:

- macOS: `LumenPhotoStudio.app/Contents/Resources` (bundle-relative — a `.app`
  is relocatable, so anything staged into a sibling `share/` tree is
  unreachable at runtime)
- everywhere else: `${CMAKE_INSTALL_DATADIR}/LumenPhotoStudio`, i.e.
  `share/LumenPhotoStudio`

The following are **not** options — they are fixed identity constants in
`CMakeLists.txt`, mirrored into `packaging/` and `ui/main.cpp`. Change all of
them together or none:

```
LPS_OUTPUT_NAME  = LumenPhotoStudio
LPS_BUNDLE_ID    = studio.lumen.photostudio
LPS_DESKTOP_ID   = studio.lumen.photostudio
LPS_ORG_DOMAIN   = lumen.studio
```

---

## Windows

### MSVC (recommended)

Install Visual Studio 2022 (or the standalone Build Tools) with the
"Desktop development with C++" workload, then Qt 6 via the online installer,
selecting *Qt 6.x.x > MSVC 2022 64-bit*.

From a *Developer PowerShell for VS 2022*:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64"
cmake --build build
```

Result: `build/bin/LumenPhotoStudio.exe`.

Multi-config alternative:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64"
cmake --build build --config Release
```

Result: `build/bin/Release/LumenPhotoStudio.exe`.

Optional LibRaw through vcpkg:

```
vcpkg install libraw:x64-windows
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64"
```

### MinGW

Install *Qt 6.x.x > MinGW 64-bit* plus the matching *Developer and Designer
Tools > MinGW* toolchain, then:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/mingw_64"
cmake --build build
```

`packaging/windows/app.rc.in` is compiled by `windres` under MinGW and by
`rc.exe` under MSVC. It uses raw numeric VERSIONINFO constants and no
`#include`, so it needs no Windows SDK headers. `enable_language(RC)` is
called inside the `if(WIN32)` block, and the warning flags are wrapped in
`$<$<COMPILE_LANGUAGE:CXX>:...>` so the resource compiler never receives C++
switches.

### Outstanding manual step — the Windows `.ico`

`resources/icons/lumen_logo_512.png` is the only icon asset in the repository.
Windows executables need a real multi-resolution `.ico`, and **no `.ico` has
been generated yet**. `CMakeLists.txt` therefore omits the `ICON` statement
from the generated `.rc` entirely, and the built `.exe` shows the generic
Windows application icon. The VERSIONINFO block is still emitted.

To fix it, produce `packaging/windows/lumen.ico` containing the 16, 24, 32, 48,
64, 128 and 256 px sizes and commit it. CMake picks it up automatically on the
next configure — no CMake edit is required. With ImageMagick:

```
magick resources/icons/lumen_logo_512.png -background none -define icon:auto-resize=256,128,64,48,32,24,16 packaging/windows/lumen.ico
```

---

## Linux (Ubuntu / Debian)

```
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config qt6-base-dev libgl1-mesa-dev libraw-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Result: `build/bin/LumenPhotoStudio`.

Notes:

- `qt6-base-dev` provides Core, Gui, Widgets and Concurrent. Ubuntu 22.04 ships
  Qt 6.2.4, which satisfies the `find_package(Qt6 6.2 ...)` minimum.
- `libgl1-mesa-dev` is required by Qt6Gui's CMake config even though the app
  itself is pure QWidgets.
- `libraw-dev` is optional. Without it, configure prints
  `LibRaw not found: RAW support disabled` and RAW loading is compiled out.
- Headless machines need the offscreen QPA plugin (shipped with the qt6-base
  runtime) or `xvfb` in order to run the binary at all.

Fedora equivalent: `qt6-qtbase-devel LibRaw-devel mesa-libGL-devel`.

---

## macOS (Homebrew)

```
brew install cmake ninja qt@6 libraw pkg-config

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)"
cmake --build build
```

Result: `build/bin/LumenPhotoStudio.app`.

`qt@6` is keg-only, hence the explicit `CMAKE_PREFIX_PATH`. LibRaw is found
through pkg-config; the explicit `/opt/homebrew`, `/usr/local` and `/opt/local`
search paths in `CMakeLists.txt` cover both Homebrew architectures and MacPorts
if pkg-config is unavailable.

### Outstanding manual step — the macOS `.icns`

As with Windows, only a PNG exists. `CFBundleIconFile` in the generated
`Info.plist` is left empty until `packaging/macos/LumenPhotoStudio.icns` is
committed. Once it is, CMake fills the key in and copies the file into
`Contents/Resources` automatically.

```
mkdir -p /tmp/lumen.iconset
for s in 16 32 64 128 256 512; do sips -z $s $s resources/icons/lumen_logo_512.png --out /tmp/lumen.iconset/icon_${s}x${s}.png; done
iconutil -c icns /tmp/lumen.iconset -o packaging/macos/LumenPhotoStudio.icns
```

A complete iconset also wants the `@2x` variants; the loop above is the minimum
`iconutil` will accept.

---

## Installing to a staging directory

```
cmake --install build --prefix dist/stage
```

See `docs/DEPLOYMENT.md` for the resulting layout on each platform and for the
`windeployqt` / `macdeployqt` steps.

---

## CI smoke test

The executable supports a headless self-check:

```
QT_QPA_PLATFORM=offscreen ./build/bin/LumenPhotoStudio --smoke-test
echo $?
```

PowerShell:

```
$env:QT_QPA_PLATFORM = "offscreen"
.\build\bin\LumenPhotoStudio.exe --smoke-test
$LASTEXITCODE
```

It constructs `MainWindow`, calls `show()`, runs one pass of the event loop and
exits 0. Any exception thrown during startup produces exit code 1. The quit
timer is armed *before* `MainWindow` is constructed, so the run can never be
blocked by the autosave-recovery dialog that `MainWindow`'s constructor
schedules.

On Windows the target is built with `WIN32_EXECUTABLE ON`, so the process has
no console attached and prints nothing. Check `$LASTEXITCODE`, not stdout.
