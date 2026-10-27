#!/bin/bash
# Build the Android tests as static arm64 executables and run them: on a device through adb when one is connected, else under qemu-aarch64(-static).
#   t_compat   dm/android/compat (see t_compat.c)
#   t_rewrite  a module rewritten with DM_TARGET=android, run through dm/host/entry.S (see t_rewrite.c); needs a Python with capstone and lz4 ($PYTHON, default python3)
# NDK: $ANDROID_NDK_HOME, $ANDROID_NDK or the newest under $ANDROID_HOME/ndk. Exit code = failures (2 = could not run)
set -e
here=$(cd "$(dirname "$0")" && pwd); dm=$(cd "$here/../.." && pwd)
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK:-$(ls -d "${ANDROID_HOME:-/nonexistent}"/ndk/* 2>/dev/null | sort -V | tail -1)}}
[ -d "$ndk" ] || { echo "no NDK: set ANDROID_NDK_HOME" >&2; exit 2; }
cc=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android31-clang | head -1)
py=${PYTHON:-python3}
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
flags=(-static -O1 -g -std=gnu11 -D_GNU_SOURCE -Wall -I"$here/../compat/include" -include "$here/../compat/darwin_compat.h")
"$cc" "${flags[@]}" "$here/t_compat.c" "$here/../compat/darwin_compat.c" -o "$out/t_compat"
mkdir "$out/mod"
"$py" -c "import sys; sys.path.insert(0, '$dm/tools/tests'); from test_rewrite_target import nso; open('$out/t.nso', 'wb').write(nso())"
DM_TARGET=android "$py" "$dm/tools/patch_nso.py" "$out/t.nso" "$out/mod" main >/dev/null
"$py" "$here/../tools/asm_elf.py" "$dm/host/entry.S" "$out/entry.S"
"$cc" -c "$out/entry.S" -o "$out/entry.o"   # assembly: without the C flags (the forced include)
"$cc" "${flags[@]}" -ffixed-x18 "$here/t_rewrite.c" "$here/../compat/darwin_compat.c" "$out/entry.o" -o "$out/t_rewrite"
set +e
fails=0
if command -v adb >/dev/null && [ "$(adb get-state 2>/dev/null)" = device ]; then
    d=/data/local/tmp/nxnative_tests
    adb shell "rm -rf $d; mkdir -p $d/files" && adb push "$out/t_compat" "$out/t_rewrite" "$out/mod" $d/ >/dev/null
    adb shell "$d/t_compat $d/files"; fails=$((fails + $?))
    adb shell "$d/t_rewrite $d/mod"; fails=$((fails + $?))
else
    qemu=$(command -v qemu-aarch64-static || command -v qemu-aarch64) || { echo "no device and no qemu-aarch64" >&2; exit 2; }
    mkdir "$out/files"
    echo "== t_compat"; DM_TEST_QEMU=1 "$qemu" "$out/t_compat" "$out/files"; fails=$((fails + $?))
    echo "== t_rewrite"; "$qemu" "$out/t_rewrite" "$out/mod"; fails=$((fails + $?))
fi
exit $fails
