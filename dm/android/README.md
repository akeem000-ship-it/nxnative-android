# Android

The Android port runs the same host as the Mac and iOS builds: the game's own arm64 code, rewritten where the platform requires it, on a host that answers Horizon's SVCs and the SDK's services. This folder holds what Android needs in place of Apple's pieces. Upstream sources build unchanged wherever they can, so the port follows the main line.

**Status:** the host builds for Android 12 and later (arm64), and an APK packs from an NSP folder. **There is no graphics backend yet.** `gfx/nvn_null.c` is a null device: the game runs its frame loop with GPU work completing at once, and nothing is drawn. The Vulkan backend is the next step (see [Roadmap](#roadmap)). No game has been run on a device yet: everything below is checked by building and by the tests in `tests/` (under qemu-aarch64), not on a phone.

## Building an APK

Needs: Python 3.12+ with `lz4 capstone fonttools` (and `backports.zstd` before 3.14), the .NET SDK 10 (for `nx_extract`), `zstd`, `cmake`, `ninja`, a JDK (`keytool`), and the Android NDK r27+ and SDK (`build-tools;35.0.0`, `platforms;android-35`) through `ANDROID_HOME` / `ANDROID_NDK_HOME`. It runs on Linux or a Mac.

```
python3 scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform android [--push]
```

This runs the NSP script's steps up to the rewrite, with the modules rewritten for Android (`DM_TARGET=android`, into `<id>_build/patched_<version>_android`). It then builds the host for the game's profile with the NDK, packs `<game>.apk` in the NSP folder, and signs it with the Android debug key (`~/.android/debug.keystore`; Android installs only signed APKs). `--push` installs it on the device connected through `adb` and copies the RomFS and add-on content to it. Without `--push`, install the APK with `adb install` and copy the RomFS as the script's last line says.

`tools/build_apk.py` also runs alone on modules that are already rewritten: `build_apk.py --game <title ID> --patched <dir> --romfs <RomFS> -o <out.apk> [--push]`.

## On the device

| What | Where |
|---|---|
| The rewritten modules, tables and fonts | in the APK (`assets/dm`), copied to the app's internal storage at the first start after each install |
| The game's RomFS | `/sdcard/Android/data/<package>/files/romfs.img` (a RomFS image) or `.../files/romfs/` (a folder); an APK holds at most 4 GB, so it is not packed |
| Add-on content | `.../files/aoc/<index>/` |
| Saves, temporary files | internal storage (`/data/data/<package>/files/save`, `temp`) |
| The log | `.../files/log.txt`, and logcat (`adb logcat -s nxnative`) |
| Environment switches | `.../files/env.txt`, one `KEY=VALUE` per line, as the iOS app's `Documents/env.txt` |

Controllers map by position, as on the Mac and iPhone: the right face button is A, the bottom one B. Touches go to the touchscreen.

## What replaces what

| Apple | Android | File |
|---|---|---|
| `mrs tpidrro_el0` (pthread TSD) for the rewritten code's thread slots | `mrs tpidr_el0; ldur [,#-8]` (bionic_tls, TLS slot -1); the loader measures where a key's value lies in it | `dm/tools/patch_nso.py` `emit_tsd_base`, `dm/host/loader.c`, `compat/darwin_compat.c` `dm_bionic_key_offset` |
| Mach, libdispatch semaphores, QoS, malloc zones, CommonCrypto, CoreFoundation dictionary, `clonefile` / `renamex_np` / xattrs | Linux and bionic, behind stand-ins for the Apple headers | `compat/` |
| Mach-O assembly | converted to ELF at build time | `tools/asm_elf.py` |
| Mach-O registry sections (`__DATA,swhle` …) | ELF sections found through `__start_` / `__stop_` | `dm/host/dm.h` `DM_SECTION`, `compat/darwin_compat.c` `getsectiondata` |
| Darwin signal contexts | Linux sigcontext (same integer layout), FPSIMD and ESR records | `dm/host/dm.h` `DM_UC_*` |
| CoreAudio AudioUnit | AAudio | `au_aaudio.c` |
| VideoToolbox (H.264), AudioToolbox (AAC) | MediaCodec | `movie_avc_mediacodec.c`, `movie_aac_mediacodec.c` |
| Metal (NVN, Vulkan and GL layers) | none yet: the null device | `gfx/nvn_null.c` |
| UIKit / AppKit front end | NativeActivity | `front_android.c` |

## Tests

`tests/run.sh` builds two static arm64 test programs with the NDK and runs them on a device through `adb`, or under `qemu-aarch64` when no device is connected:

- `t_compat`: the compat layer on bionic. It covers thread slots read through the rewritten code's own instruction sequence on nine threads, thread suspension and register state, fault-safe memory reads and regions, signal context records, SHA-256, the dictionary, semaphores, `clonefile` / `renamex_np` / xattrs, `F_GETPATH` and registry sections.
- `t_rewrite`: a module rewritten with `DM_TARGET=android`, mapped as the loader maps it and run on five threads through `dm/host/entry.S`. Each thread's x18 and thread register accesses reach its own slots, and its `svc` reaches the gate.

`dm/tools/tests/test_rewrite_target.py` checks the rewriter's output for both targets.

## Known gaps

- **Graphics:** nothing is drawn (above). A compute shader's buffer results are not produced either, which a game may wait on.
- **Text input** (the software keyboard) reports no window, and the progress screen only logs.
- **Rumble, motion controls, the frame-rate display and the settings launcher** are not there yet.
- **Real-time scheduling:** the present thread gets no real-time policy (Android gives apps none); ADPF performance hints are the Android tool for it.

## Roadmap

1. **Vulkan backend.** It takes the null device's place behind the same interface as `nvn_mtl.m` (`dm/gfx/nvn/nvn_mtl.h`, `nvn_obj.h`). The shader chain already ends in SPIR-V (Ryujinx's translator), which Vulkan takes without SPIRV-Cross's Metal step. Most phones read no BC textures, so those are transcoded to ASTC or RGBA at upload. Presents go to the `ANativeWindow` (`front_metal_layer`) through a swapchain, paced by `AChoreographer`.
2. **A Java launcher** for settings, text input, rumble and picking the RomFS without `adb`.
