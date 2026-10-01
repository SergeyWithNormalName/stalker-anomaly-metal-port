# macOS App Bundle Integration (DEV & Future Release)

## 1. Overview & Architecture

To provide native macOS application lifecycle management, WindowServer integration, GameController support, and macOS Game Mode eligibility without degrading the rapid C++ development loop, the build system implements a lightweight, post-link DEV `.app` bundle synchronization.

```
edit source
    │
    ▼
ninja -C build AnomalyDX11
    │ (incremental compile object files)
    ▼
Link AnomalyDX11 (ld64)
    │
    ▼ [POST_BUILD custom command]
packaging/macos/sync_dev_bundle.sh (< 3 ms)
    ├── Copies configured Info.plist (if changed)
    ├── Ensures Contents/{MacOS,Resources,Frameworks}
    └── Hard-links AnomalyDX11 -> Anomaly.app/Contents/MacOS/AnomalyDX11
    │
    ▼
run-gamma-metal.command
    ├── Default: open -n -W Anomaly.app --args ... (LaunchServices / Game Mode)
    └── --raw:   direct execution of AnomalyDX11 in terminal (debugger / probes)
```

### Why CMake `POST_BUILD` Instead of `MACOSX_BUNDLE`
1. **Preserves the Raw Binary Workflow:** Setting `set_target_properties(Anomaly.DX11 PROPERTIES MACOSX_BUNDLE TRUE)` would alter `$<TARGET_FILE:Anomaly.DX11>`, placing the executable deep inside `Anomaly.app/Contents/MacOS/`. This would break external diagnostic scripts, probes (`AnomalyMetalBootstrap`), debuggers configured for raw paths, and CMake PDB directory rules.
2. **Minimal Link-Time Overhead:** Attaching `add_custom_command(TARGET ${NAME} POST_BUILD ...)` ensures the bundle synchronization executes **only** when `AnomalyDX11` is actually linked. When compiling individual translation units, the packaging step never runs. When running `ninja` with no changes, Ninja reports `ninja: no work to do` and executes 0 commands.
3. **Zero-Copy Synchronization:** The script uses filesystem hard links (`ln -f`) between the compiled binary and `Anomaly.app/Contents/MacOS/AnomalyDX11`. Both point to the exact same inode. Creation takes less than 1 millisecond and consumes 0 additional disk bytes. Fallback to `cp -pf` is provided if hard-linking fails.

---

## 2. DEV Bundle Layout

Location: `build/Binaries/RelWithDebInfo/Anomaly.app` (alongside raw `AnomalyDX11`).

```
build/Binaries/RelWithDebInfo/
├── AnomalyDX11                        <-- Raw Mach-O executable
└── Anomaly.app/                       <-- Lightweight DEV bundle
    └── Contents/
        ├── Info.plist                 <-- Generated from packaging/macos/Info.plist.in
        ├── PkgInfo                    <-- APPL????
        ├── MacOS/
        │   └── AnomalyDX11            <-- Hard link to ../../AnomalyDX11
        ├── Resources/                 <-- AppIcon placeholder
        └── Frameworks/                <-- Empty in DEV; staging target for E10
```

### Inode Policy: Hard Link (DEV) vs Distinct Copy (RELEASE)

#### DEV (Current Implementation)
- **Hard Link Permitted:** `Anomaly.app/Contents/MacOS/AnomalyDX11` is an instantaneous hard link (`ln -f`) to `build/Binaries/RelWithDebInfo/AnomalyDX11` (sharing inode).
- **Minimal Cost:** < 1 ms link time, zero disk space consumed (zero-copy).
- **No Developer ID Signing / Notarization:** Development runs under ad-hoc linker signatures without notarization overhead.

#### RELEASE (Phase E10 Staging Rule)
- **Copy-Before-Sign Required:** The executable **must** be copied (`cp -p`) into a separate staging `.app` directory so it has an independent inode.
- **Never Share Inode with Raw Binary:** The release bundle must never share an inode with the working development binary; mutating install names or signatures must never alter the developer's raw binary.
- **Frameworks Staging:** Third-party dylibs are copied into `Contents/Frameworks/`.
- **Relinking:** Install names and `@rpath` entries are updated inside the staging tree via `install_name_tool`.
- **Inside-Out Signing:** Nested libraries and frameworks are signed before signing the main binary and outer bundle.
- **Developer ID & Notarization:** Developer ID Application signing and notarization execute strictly on the isolated release staging tree.

---

## 3. Metadata & Info.plist (`packaging/macos/Info.plist.in`)

Metadata is maintained through CMake substitution without duplicated hardcoded strings:
- **`CFBundleIdentifier`**: `com.stalker-anomaly.game`
- **`CFBundleName`**: `Anomaly`
- **`CFBundleDisplayName`**: `S.T.A.L.K.E.R. Anomaly`
- **`CFBundleExecutable`**: `AnomalyDX11`
- **`CFBundlePackageType`**: `APPL`
- **`CFBundleShortVersionString`**: `${PROJECT_VERSION}` (synchronized with CMake `TODAY_DOTS`)
- **`CFBundleVersion`**: `${TODAY_DOTS}`
- **`LSMinimumSystemVersion`**: Unified with Mach-O deployment target (`${CMAKE_OSX_DEPLOYMENT_TARGET}` if specified, else active SDK via `xcrun --sdk macosx --show-sdk-version`, matching Mach-O `LC_BUILD_VERSION minos` exactly).
- **`LSApplicationCategoryType`**: `public.app-category.games`
- **`LSSupportsGameMode`**: `<true/>`
- **`GCSupportsControllerUserInteraction`**: `<true/>`
- **`GCSupportedGameControllers`**:
  ```xml
  <array>
      <dict>
          <key>ProfileName</key>
          <string>ExtendedGamepad</string>
      </dict>
  </array>
  ```
- **Spatial Audio (Future Preparation):** Commented placeholder for `NSMotionUsageDescription` (required for `CMHeadphoneMotionManager` AirPods head tracking). Kept inactive in DEV so macOS does not prompt users for motion sensor permissions prior to code integration.
- **Icons:** Configured for `AppIcon` (`Resources/AppIcon.icns`).

### Deployment Target & Minimum macOS Resolution
1. **Single Source of Truth:** `LSMinimumSystemVersion` must never be hardcoded independently from the binary's actual Mach-O deployment target.
2. **Current Toolchain State:** When `CMAKE_OSX_DEPLOYMENT_TARGET` is unset, AppleClang defaults to targeting the active SDK version (`minos 27.0` in Mach-O `LC_BUILD_VERSION`). To avoid dyld discrepancy, CMake queries `xcrun --sdk macosx --show-sdk-version` so both binary and plist record `27.0`.
3. **Explicit Deployment Target:** To target an earlier macOS version across the entire project, set `-DCMAKE_OSX_DEPLOYMENT_TARGET=<version>` at configure time. Both the compiler (`-mmacosx-version-min=<version>`) and the generated `Info.plist` (`LSMinimumSystemVersion = <version>`) will automatically use that identical value.
4. **No Artificial Version Inflation:** Optional capabilities (Game Mode in 14.0+, MetalFX, AirPods head tracking) do not raise the base deployment target of the game. They are queried at runtime via macOS capability and availability checks.

---

## 4. Game Mode & LaunchServices

### macOS Game Mode Behavior
On macOS 14.0+ (Sonoma, Sequoia, and later), Game Mode is managed by the OS daemon (`gamepolicyd`) and WindowServer.
1. **Launch Mechanism:** Game Mode **only** activates when an application bundle is launched through LaunchServices (`open` command or Finder). Launching the raw binary from Terminal treats the process as a sub-task of Terminal/zsh, which disables Game Mode recognition.
2. **Bundle Declaration:** The bundle must declare `LSApplicationCategoryType = public.app-category.games` and `LSSupportsGameMode = <true/>`.
3. **Fullscreen Requirement:** Game Mode automatically engages when the game window enters macOS Fullscreen. In windowed mode, Game Mode remains on standby.
4. **Hardware State:** When active, macOS prioritizes CPU and GPU cores for the game process while lowering Bluetooth polling latency for wireless gamepads and AirPods.

---

## 5. Working Directory & Path Resolution

### Discovery of LaunchServices Working Directory
When an app is launched via LaunchServices (`open Anomaly.app`), macOS sets `cwd` to `/`.
In X-Ray (`src/xrCore/LocatorAPI.cpp:searchForFsltx`), the engine looks for `fsgame.ltx` in:
1. `cwd` (`/`) — fails when launched via LaunchServices.
2. `../` — fails.
3. `ApplicationPath` (`Contents/Resources/`) — fails in DEV because assets are external.

### Solution
The launcher explicitly passes `-fsltx "$RUNTIME_ROOT/fsgame.ltx"` as an argument to `Anomaly.app`. `CLocatorAPI` resolves this path, sets `$fs_root$` to `$RUNTIME_ROOT`, and anchors all game assets (`appdata`, `gamedata`, `db`, `savedgames`, `logs`) relative to `$fs_root$`.

---

## 6. Dual Launch Modes in `run-gamma-metal.command`

### 1. Default (DEV `.app` via LaunchServices)
```bash
./run-gamma-metal.command [--load <save>] [--capture-ready]
```
- Launches `build/Binaries/RelWithDebInfo/Anomaly.app` using `open -n -W -a`.
- Inherits exported environment variables (`XRAY_METAL_*`, `MTL_CAPTURE_ENABLED`).
- Redirects stdout/stderr to `$RUNTIME_ROOT/appdata/logs/launcher_stdout.log` and `launcher_stderr.log`.
- Enables macOS application lifecycle and Game Mode eligibility upon fullscreen.

### 2. Raw Executable (Low-Level Diagnostic Mode)
```bash
./run-gamma-metal.command --raw [--load <save>] [--capture-ready]
```
- Executes `build/Binaries/RelWithDebInfo/AnomalyDX11` directly in the terminal via `exec`.
- Keeps interactive stdout/stderr live in terminal.
- Intended for LLDB debugging, command-line probes, profiling without LaunchServices.

---

## 7. Dependency Audit (`otool -L AnomalyDX11`)

| Dependency | Current Linked Path | Type | Future Release Action (Phase E10) |
|---|---|---|---|
| `libSDL2-2.0.0.dylib` | `/opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib` | Homebrew | Copy to `Contents/Frameworks/`, change id to `@rpath/libSDL2-2.0.0.dylib` |
| `libopenal.1.dylib` | `/opt/homebrew/opt/openal-soft/lib/libopenal.1.dylib` | Homebrew | Copy to `Contents/Frameworks/`, change id to `@rpath/libopenal.1.dylib` |
| `libcrypto.3.dylib` | `/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite dependency paths |
| `libgif.dylib` | `/opt/homebrew/opt/giflib/lib/libgif.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite id to `@rpath` |
| `libjpeg.8.dylib` | `/opt/homebrew/opt/jpeg-turbo/lib/libjpeg.8.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite id to `@rpath` |
| `libtheora.1.dylib` | `/opt/homebrew/opt/theora/lib/libtheora.1.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite id to `@rpath` |
| `libtheoradec.2.dylib` | `/opt/homebrew/opt/theora/lib/libtheoradec.2.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite id to `@rpath` |
| `libogg.0.dylib` | `/opt/homebrew/opt/libogg/lib/libogg.0.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite id to `@rpath` |
| `libvorbis.0.dylib` | `/opt/homebrew/opt/libvorbis/lib/libvorbis.0.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite id to `@rpath` |
| `libvorbisfile.3.dylib` | `/opt/homebrew/opt/libvorbis/lib/libvorbisfile.3.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite id to `@rpath` |
| `libvorbisenc.2.dylib` | `/opt/homebrew/opt/libvorbis/lib/libvorbisenc.2.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite id to `@rpath` |
| `libluajit-5.1.2.dylib` | `/opt/homebrew/opt/luajit/lib/libluajit-5.1.2.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite id to `@rpath` |
| `libtbb.12.dylib` | `/opt/homebrew/opt/tbb/lib/libtbb.12.dylib` | Homebrew | Copy to `Contents/Frameworks/`, rewrite id to `@rpath` |
| `libdxvk_d3d11.0.dylib` | `@rpath/libdxvk_d3d11.0.dylib` | Vendored / Fallback | Bundle into `Contents/Frameworks/` or build-in if DXVK Native retained |
| `libdxvk_dxgi.0.dylib` | `@rpath/libdxvk_dxgi.0.dylib` | Vendored / Fallback | Bundle into `Contents/Frameworks/` or build-in if DXVK Native retained |
| System Frameworks (`AppKit`, `Foundation`, `Metal`, `GameController`, `CoreAudio`, `CoreHaptics`, etc.) | `/System/Library/Frameworks/...` | System | Standard macOS runtime link; no bundling needed |
| `libc++.1.dylib`, `libSystem.B.dylib`, `libiconv.2.dylib` | `/usr/lib/...` | System | Standard macOS runtime link; no bundling needed |

### Current Embedded LC_RPATHs in `AnomalyDX11`
- `/opt/homebrew/lib` (Homebrew fallback)
- `<PRIVATE_LOCAL_PATH>` (Absolute developer path)
- `<PRIVATE_LOCAL_PATH>` (Absolute developer path)

*Release Note (E10):* In Phase E10, all absolute developer paths and `/opt/homebrew/lib` references must be removed. The executable LC_RPATH must use `@executable_path/../Frameworks` with `install_name_tool` relinking.

---

## 8. Runtime & Assets Manifest for Future Standalone Release

A standalone consumer release bundle will require packaging or mounting:
1. **Metal Shader Library:**
   Precompiled `.metallib` archives currently pointed to by `xray_metal_shader_env.sh` (from `build/metal-runtime-shaders/`). In a release bundle, these belong in `Contents/Resources/Shaders/`.
2. **Game Data Archives:**
   Core database archives (`db/*.xdb*`), configs, and scripts (`gamedata/`).
3. **Writable User Data:**
   Saves, screenshots, logs, and `user.ltx` must point to `~/Library/Application Support/S.T.A.L.K.E.R. Anomaly/` or an isolated portable profile path rather than inside the read-only application bundle.
4. **Third-Party Frameworks:**
   All 13 Homebrew dylibs staged into `Contents/Frameworks/`.
5. **MO2 Launcher Decoupling:**
   The game bundle does not bake in a single static mod profile. A release frontend/launcher will offer:
   - **Play:** Launches the main game bundle with active mod profile arguments.
   - **Mod Configuration:** Launches the MO2 profile manager.
   - **Diagnostics:** Runs integrity probes, clears shader caches, or collects logs.

---

## 9. Signing & Distribution Strategy (Phase E10)

1. **Local Development (Current):**
   - Uses automatic ad-hoc linker signatures created by ld64 on Apple Silicon (`flags=0x20002(adhoc,linker-signed)`).
   - No paid Apple Developer account or manual `codesign` command is required during normal compilation.
2. **Release Distribution (Phase E10):**
   - **Strict Signing Order (Inside-Out):**
     1. Sign individual dynamic libraries in `Contents/Frameworks/` with Developer ID Application + Hardened Runtime (`--options runtime`).
     2. Sign helper tools / standalone probes.
     3. Sign the main executable `Contents/MacOS/AnomalyDX11`.
     4. Sign the outer `Anomaly.app` bundle.
   - Do **NOT** use `codesign --deep` for release distribution, as Apple deprecates `--deep` for release verification.
   - Submit for Apple Notarization (`xcrun notarytool submit`).
   - Staple the notarization ticket (`xcrun stapler staple Anomaly.app`).
