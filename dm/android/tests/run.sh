#!/bin/bash
# Build the compat tests (t_compat.c) as a static arm64 Android executable and run them: on a device through adb when one is connected, else under qemu-aarch64(-static).
# NDK: $ANDROID_NDK_HOME, $ANDROID_NDK or the newest under $ANDROID_HOME/ndk. Exit code = failures
set -e
here=$(cd "$(dirname "$0")" && pwd)
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK:-$(ls -d "${ANDROID_HOME:-/nonexistent}"/ndk/* 2>/dev/null | sort -V | tail -1)}}
[ -d "$ndk" ] || { echo "no NDK: set ANDROID_NDK_HOME" >&2; exit 2; }
cc=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android31-clang | head -1)
out=$(mktemp -d)
"$cc" -static -O1 -g -std=gnu11 -D_GNU_SOURCE -Wall -I"$here/../compat/include" -include "$here/../compat/darwin_compat.h" \
    "$here/t_compat.c" "$here/../compat/darwin_compat.c" -o "$out/t_compat"
set +e
if command -v adb >/dev/null && [ "$(adb get-state 2>/dev/null)" = device ]; then
    adb push "$out/t_compat" /data/local/tmp/t_compat >/dev/null && adb shell "rm -rf /data/local/tmp/t_compat_files; mkdir /data/local/tmp/t_compat_files; /data/local/tmp/t_compat /data/local/tmp/t_compat_files"
else
    qemu=$(command -v qemu-aarch64-static || command -v qemu-aarch64) || { echo "no device and no qemu-aarch64" >&2; exit 2; }
    mkdir "$out/files" && DM_TEST_QEMU=1 "$qemu" "$out/t_compat" "$out/files"
fi
rc=$?
rm -rf "$out"
exit $rc
