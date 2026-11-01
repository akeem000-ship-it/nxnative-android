# Android

The Android port runs the same host as the Mac and iOS builds: the game's own arm64 code, rewritten where the platform requires it, on a host that answers Horizon's SVCs and the SDK's services. This folder holds what Android needs in place of Apple's pieces. Upstream sources build unchanged wherever they can, so the port follows the main line.

**Status:** the host builds for Android 12 and later (arm64), and an APK packs from an NSP folder. The Vulkan backend (`gfx/nvn_vk.c`, `gfx/nvn_vk_draw.inc`) executes textures, copies, clears, draws and compute dispatches and presents to the screen, from the translator's SPIR-V. **Getting the shaders into an APK is not automated yet** ([Shaders](#shaders)), and storage images, geometry and tessellation shaders, transform feedback and render conditions are not executed yet. `-DGFX=null` builds the null device instead (`gfx/nvn_null.c`: nothing drawn). No game has been run on a device yet: everything below is checked by building and by the tests in `tests/` (under qemu-aarch64), not on a phone.

## Building an APK

Needs: Python 3.12+ with `lz4 capstone fonttools` (and `backports.zstd` before 3.14), the .NET SDK 10 (for `nx_extract`), `zstd`, `cmake`, `ninja`, a JDK (`keytool`), and the Android NDK r27+ and SDK (`build-tools;35.0.0`, `platforms;android-35`) through `ANDROID_HOME` / `ANDROID_NDK_HOME`. It runs on Linux or a Mac.

```
python3 scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform android [--push]
```

This runs the NSP script's steps up to the rewrite, with the modules rewritten for Android (`DM_TARGET=android`, into `<id>_build/patched_<version>_android`). It then builds the host for the game's profile with the NDK, packs `<game>.apk` in the NSP folder, and signs it with the Android debug key (`~/.android/debug.keystore`; Android installs only signed APKs). `--push` installs it on the device connected through `adb` and copies the RomFS and add-on content to it. Without `--push`, install the APK with `adb install` and copy the RomFS as the script's last line says.

`tools/build_apk.py` also runs alone on modules that are already rewritten: `build_apk.py --game <title ID> --patched <dir> --romfs <RomFS> -o <out.apk> [--push]`.

## Shaders

The draws use the same Switch-shader translator as the Mac and iOS builds (`dm/gfx/tools/dmsh_xlate`, Ryujinx's), which already produces Vulkan SPIR-V; it runs on Linux with the .NET 10 runtime. Each shader's SPIR-V, resource list and constants are kept and made ready for phones:

1. Run the shader build with the translator wrapper: `XLATE=dm/android/tools/xlate_keep.py DMSH_SPV_OUT=<dir> python3 dm/gfx/tools/dmsh_build.py ...` (as the NSP script runs it). The wrapper keeps `<key>.spv`, `<key>.res.json` and `<key>.c1` in `<dir>`, and leaves the versioned chain tools unchanged. **Not run on a real game yet:** `dmsh_build.py` also compiles Metal after translating, which fails off a Mac after the files are kept.
2. `python3 dm/android/tools/nvn_vk_shaders.py <dir> <vk dir>` writes the backend's files. It uses `spv_mobile.py` to drop the optional capabilities phones lack, give each stage its own descriptor set and add the GL-depth variant of each vertex shader. Each `.res` line pairs a binding's Vulkan descriptor type with its NVN role.
3. `build_apk.py --shaders <vk dir>` packs them into `assets/dm/vk`, and the app points the backend there (`DM_VK_SHADERS`).

The NSP script's `--platform android` does not run these steps yet.

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
| Metal (the NVN layer's execution) | Vulkan 1.1: textures, copies, clears, draws, dispatches, present | `gfx/nvn_vk.c`, `gfx/nvn_vk_draw.inc`, `gfx/nvn_fmt_vk.c` |
| The Metal shader build (SPIR-V → MSL → metallib) | the translator's SPIR-V kept and prepared for phones | `tools/xlate_keep.py`, `tools/nvn_vk_shaders.py`, `tools/spv_mobile.py` |
| Metal (the Vulkan and GL layers of Hades and other Vulkan / GL games) | none yet | |
| UIKit / AppKit front end | NativeActivity | `front_android.c` |

## Tests

`tests/run.sh` builds two static arm64 test programs with the NDK and runs them on a device through `adb`, or under `qemu-aarch64` when no device is connected:

- `t_compat`: the compat layer on bionic. It covers thread slots read through the rewritten code's own instruction sequence on nine threads, thread suspension and register state, fault-safe memory reads and regions, signal context records, SHA-256, the dictionary, semaphores, `clonefile` / `renamex_np` / xattrs, `F_GETPATH` and registry sections.
- `t_rewrite`: a module rewritten with `DM_TARGET=android`, mapped as the loader maps it and run on five threads through `dm/host/entry.S`. Each thread's x18 and thread register accesses reach its own slots, and its `svc` reaches the gate.

`tests/run_vk.sh` builds the Vulkan backend for the desktop and runs `t_vk.c` on any Vulkan 1.1 device (Mesa's lavapipe without a GPU), with the validation layer when installed, twice: BC textures sampled by the device and decoded on the CPU. It records command lists as `hle_nvn.c` does and checks:

- scissored, layered and depth clears;
- buffer ↔ texture round trips with row strides;
- deswizzled packaged textures;
- copies and mirrored blits;
- BC1 decoding;
- events, counters and fences in order;
- the presented frame (flipped, cropped).

It also checks draws, with shaders laid out as the translator lays them out (`tests/shaders`): quads, both window origins, the game's uniform buffers, textures through the registered pools, 8-bit indexed draws with a base vertex, culling, depth testing with the GL depth variant, alpha blending, and a compute dispatch writing a storage buffer back to guest memory.

`dm/tools/tests/test_rewrite_target.py` checks the rewriter's output for both targets.

## Known gaps

- **Graphics, not executed yet:** storage images (a draw or dispatch that uses one is skipped and logged), geometry and tessellation shaders, transform feedback, render conditions, logic ops, advanced blend modes, instance divisors above 1, and the Metal build's shader fixes that the raw SPIR-V lacks (outputs starting as (0,0,0,1), Maxwell's NaN rules for min / max, integer pixel outputs).
- **Graphics, simple rather than fast:** buffer contents are copied into staging for every draw that reads them, a submission completes before its call returns (the CPU waits for the GPU), a draw that writes a storage buffer ends the submission, and every image stays in layout `GENERAL`.
- **Text input** (the software keyboard) reports no window, and the progress screen only logs.
- **Rumble, motion controls, the frame-rate display and the settings launcher** are not there yet.
- **Real-time scheduling:** the present thread gets no real-time policy (Android gives apps none); ADPF performance hints are the Android tool for it.

## Roadmap

1. **Shaders into the APK automatically:** the NSP script running the translation with `xlate_keep.py` and `nvn_vk_shaders.py` on Linux, without the Metal steps.
2. **Running a real game on a phone:** fixing what that shows.
3. **Speed:** guest memory mirrored instead of copied per draw, asynchronous submission, per-image layouts, presents paced by `AChoreographer`.
4. **The missing pieces above,** storage images first.
2. **A Java launcher** for settings, text input, rumble and picking the RomFS without `adb`.
