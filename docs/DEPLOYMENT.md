# Lumen Photo Studio Deployment

## App Metadata

- App name: Lumen Photo Studio
- Version: 1.1.0
- Company name: Lumen
- Icon path: `resources/icons/lumen_logo_512.png`
- Executable target: `LumenPhotoUI`

## CMake Install

Configure and build:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Install into a staging folder:

```powershell
cmake --install build --config Release --prefix dist/stage
```

The install step stages:

- `LumenPhotoUI`
- `plugins/` placeholder
- `presets/` placeholder
- deployment documentation

Qt resources are compiled into the executable through `resources/resources.qrc`.

## Windows

Target output folder:

```text
dist/windows/
```

Typical release flow from a Qt-enabled developer shell:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --config Release --prefix dist/windows
windeployqt dist/windows/bin/LumenPhotoUI.exe
```

Notes:

- If using a multi-config generator, keep `--config Release`.
- Copy optional runtime files such as sample presets, LUT packs, and plugin manifests after `windeployqt`.
- If LibRaw is enabled, ensure the LibRaw runtime DLL is present beside the executable or in the deployed runtime path.

## macOS

Target output folder:

```text
dist/macos/
```

Placeholder flow:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --config Release --prefix dist/macos
macdeployqt "dist/macos/LumenPhotoUI.app"
```

Future packaging work:

- Set bundle identifier.
- Add signed `.icns` app icon.
- Add code signing and notarization.
- Verify plugin/preset writable data location for sandboxed builds.

## Linux

Target output folder:

```text
dist/linux/
```

Placeholder options:

- AppImage for portable desktop builds.
- `.deb` package for Debian/Ubuntu installs.

Notes:

- Bundle or declare Qt 6 runtime dependencies.
- Include desktop entry metadata.
- Add icon installation under the appropriate hicolor icon theme path.
- Verify LibRaw runtime dependency when RAW support is enabled.
