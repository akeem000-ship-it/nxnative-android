#!/usr/bin/env python3
# Pack an Android APK for one game: the host built for its profile with the NDK (dm/android/CMakeLists.txt → libnxhost.so), the rewritten modules and the profile's tables as assets
# (assets/dm, copied to internal storage at the first start, front_android.c), a NativeActivity manifest, the game's icon. The RomFS does not go into the APK (an APK holds at most 4 GB
# and games are larger): --push installs the APK and copies the RomFS (folder or RomFS image) and the add-on content to the app's external files folder through adb.
# scripts/build_ipa_from_nsp.py --platform android calls this after its rewrite step; it also runs alone:
#   build_apk.py --game <title ID> --patched <rewritten modules> --romfs <RomFS folder or .img> -o <out.apk> [--name ...] [--push]
# The modules must have been rewritten with DM_TARGET=android (their fixups say so; the host refuses others). Signed with the Android debug key (~/.android/debug.keystore,
# made with keytool when missing): Android installs only signed APKs, and updates install over an earlier build only with the same key.
# Needs: the NDK ($ANDROID_NDK_HOME, or the newest in $ANDROID_HOME/ndk), the SDK's build-tools and a platform ($ANDROID_HOME), cmake, ninja, keytool; adb for --push
import argparse, hashlib, os, re, shutil, subprocess, sys, tempfile, time, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DM = HERE.parents[1]
MIN_SDK, TARGET_SDK = 31, 35   # Android 12: AAudio, MediaCodec and Vulkan 1.1 everywhere; arm64 only

class BuildError(Exception): pass

def say(msg): print(msg, flush=True)

def run(cmd, log=None, env=None, cwd=None):
    r = subprocess.run([str(c) for c in cmd], env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if log: Path(log).parent.mkdir(parents=True, exist_ok=True); Path(log).write_text(r.stdout)
    if r.returncode: raise BuildError(f'{Path(str(cmd[0])).name} failed (exit {r.returncode}){f", see {log}" if log else ""}:\n' + '\n'.join(r.stdout.splitlines()[-25:]))
    return r.stdout

def sdk_paths():
    sdk = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or Path.home() / 'Android/Sdk')
    ndk = os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK')
    if not ndk:
        cands = sorted((sdk / 'ndk').glob('*'), key=lambda p: [int(x) if x.isdigit() else 0 for x in re.split(r'[.-]', p.name)]) if (sdk / 'ndk').is_dir() else []
        ndk = str(cands[-1]) if cands else ''
    if not ndk or not Path(ndk, 'build/cmake/android.toolchain.cmake').is_file(): raise BuildError('no Android NDK: set ANDROID_NDK_HOME (r27 or newer)')
    bts = sorted((sdk / 'build-tools').glob('*'), key=lambda p: [int(x) if x.isdigit() else 0 for x in re.split(r'[.-]', p.name)]) if (sdk / 'build-tools').is_dir() else []
    if not bts: raise BuildError(f'no SDK build-tools under {sdk}: set ANDROID_HOME, then sdkmanager "build-tools;35.0.0" "platforms;android-35"')
    jar = sdk / f'platforms/android-{TARGET_SDK}/android.jar'
    if not jar.is_file():
        plats = sorted(sdk.glob('platforms/android-*/android.jar'))
        if not plats: raise BuildError(f'no SDK platform under {sdk}: sdkmanager "platforms;android-{TARGET_SDK}"')
        jar = plats[-1]
    return Path(ndk), bts[-1], jar

def build_host(game, out, ndk, env, logs):
    """libnxhost.so for the game profile (a provisional or derived profile is found through DM_GAMES_DIR / DM_GAMES_FIRST in env, as the Makefile's)"""
    bd = out / 'host'
    for t in ('cmake', 'ninja'):
        if not shutil.which(t): raise BuildError(f'{t} not found')
    run(['cmake', '-S', DM / 'android', '-B', bd, '-G', 'Ninja', f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake', '-DANDROID_ABI=arm64-v8a',
         f'-DANDROID_PLATFORM=android-{MIN_SDK}', f'-DGAME={game}', f'-DPython3_EXECUTABLE={sys.executable}', '-DCMAKE_BUILD_TYPE=RelWithDebInfo'], logs / 'android_cmake.log', env)
    run(['ninja', '-C', bd], logs / 'android_host.log', env)
    so = bd / 'libnxhost.so'
    stripped = out / 'libnxhost.so'
    strip = next((ndk / 'toolchains/llvm/prebuilt').glob('*/bin/llvm-strip'))
    run([strip, '--strip-debug', '-o', stripped, so])   # symbols stay (crash backtraces name host functions), debug info goes
    return stripped

def stage_assets(game_dir, patched, modules, out, env_file, std_font, logs, shaders=None):
    """assets/dm: what the iOS app embeds (dm/ios/tools/build_ipa.sh) — the rewritten modules and their tables, the NRO modules, the profile's shader tables, the fonts — and files.txt
    listing them under a stamp of their contents (the app copies them again when the stamp changes)"""
    a = out / 'assets/dm'; shutil.rmtree(out / 'assets', ignore_errors=True); a.mkdir(parents=True)
    fx = []
    for m in modules:
        for ext in ('patched.bin', 'fixups.tsv', 'gates.tsv', 'cuts.tsv', 'obs.tsv'):
            p = patched / f'{m}.{ext}'
            if not p.is_file(): raise BuildError(f'no rewritten module file {p} (run dm/tools/patch_nso.py with DM_TARGET=android)')
            fx.append(p)
        t = (patched / f'{m}.fixups.tsv').read_text()
        if '# TARGET\tandroid' not in t: raise BuildError(f'{patched / (m + ".fixups.tsv")} was rewritten for {"Apple" if "# TARGET" not in t else t.split("# TARGET")[1].split()[0]}: rewrite with DM_TARGET=android')
    nro = patched / 'nro.tsv'
    if nro.is_file():   # NRO modules the game loads with nn::ro: their rewritten files sit beside the table (loader.c reads them from the module folder off iOS)
        fx.append(nro)
        for line in nro.read_text().splitlines():
            if line and not line.startswith('#'):
                n = line.split('\t')[0]
                fx += [p for p in patched.glob(f'{glob_escape(n)}.*') if p.suffix in ('.bin', '.tsv')]
    for p in dict.fromkeys(fx): shutil.copy2(p, a / p.name)
    for f in ('shader_map.tsv', 'pso_manifest.tsv', 'c1_full.tsv', 'cb_extent.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv'):
        if (game_dir / f).is_file(): shutil.copy2(game_dir / f, a / f)
    if not (a / 'shader_map.tsv').exists(): (a / 'shader_map.tsv').write_text('')   # the null graphics device draws nothing: an empty table is enough
    (a / 'font').mkdir()
    if std_font: shutil.copy2(std_font, a / 'font/standard.ttf')
    # the button icons' letters: macOS's Arial Rounded Bold (make_ext_font.py's default) where it exists, else a bold sans of this machine
    letters = next((f for f in ('/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
                                '/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf', '/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf',
                                'C:/Windows/Fonts/arialbd.ttf') if Path(f).is_file()), None)
    try: run([sys.executable, DM / 'tools/make_ext_font.py', a / 'font/nintendo_ext.ttf'] + ([letters] if letters else []), logs / 'android_font.log')
    except BuildError as e: say(f'[RED] no Nintendo extension font (button icons in text will be missing): {str(e).splitlines()[0]} (pip install fonttools)')
    if env_file: shutil.copy2(env_file, a / 'env.txt')
    if shaders:   # the Vulkan shaders (nvn_vk_shaders.py output): assets/dm/vk
        (a / 'vk').mkdir()
        for p in sorted(Path(shaders).iterdir()):
            if p.suffix in ('.spv', '.res', '.c1'): shutil.copy2(p, a / 'vk' / p.name)
    files = sorted(str(p.relative_to(a)) for p in a.rglob('*') if p.is_file())
    h = hashlib.sha256()
    for f in files: h.update(f.encode() + b'\0'); h.update((a / f).read_bytes())
    stamp = f'{time.strftime("%Y-%m-%d %H:%M:%S")} {h.hexdigest()[:16]}'
    (a / 'files.txt').write_text(stamp + '\n' + '\n'.join(files) + '\n')
    return out / 'assets', sum((a / f).stat().st_size for f in files)

def glob_escape(s): return re.sub(r'([*?\[])', r'[\1]', s)

MANIFEST = '''<?xml version="1.0" encoding="utf-8"?>
<!-- generated by dm/android/tools/build_apk.py -->
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{package}" android:versionCode="{code}" android:versionName="{version}">
    <uses-sdk android:minSdkVersion="{min_sdk}" android:targetSdkVersion="{target_sdk}"/>
    <uses-feature android:glEsVersion="0x00030002" android:required="false"/>
    <uses-feature android:name="android.hardware.vulkan.version" android:version="0x401000" android:required="false"/>
    <uses-feature android:name="android.hardware.gamepad" android:required="false"/>
    <uses-feature android:name="android.hardware.touchscreen" android:required="false"/>
    <uses-permission android:name="android.permission.VIBRATE"/>
    <application android:label="{label}" {icon}android:hasCode="false" android:extractNativeLibs="false" android:isGame="true" android:appCategory="game"
                 android:allowBackup="false" android:largeHeap="true" android:resizeableActivity="false"
                 android:theme="@android:style/Theme.DeviceDefault.NoActionBar.Fullscreen">
        <activity android:name="android.app.NativeActivity" android:exported="true" android:launchMode="singleTask"
                  android:screenOrientation="sensorLandscape" android:configChanges="orientation|screenSize|screenLayout|keyboard|keyboardHidden|navigation|uiMode|density|smallestScreenSize"
                  android:resizeableActivity="false" android:preferMinimalPostProcessing="true">
            <meta-data android:name="android.app.lib_name" android:value="nxhost"/>
            <intent-filter>
                <action android:name="android.intent.action.MAIN"/>
                <category android:name="android.intent.category.LAUNCHER"/>
            </intent-filter>
        </activity>
    </application>
</manifest>
'''

def xml_attr(s): return s.replace('&', '&amp;').replace('"', '&quot;').replace('<', '&lt;').replace('>', '&gt;').replace("'", "\\'")

def debug_keystore():
    ks = Path.home() / '.android/debug.keystore'
    if not ks.exists():
        ks.parent.mkdir(parents=True, exist_ok=True)
        run(['keytool', '-genkeypair', '-keystore', ks, '-storepass', 'android', '-alias', 'androiddebugkey', '-keypass', 'android', '-keyalg', 'RSA', '-keysize', '2048',
             '-validity', '10000', '-dname', 'CN=Android Debug,O=Android,C=US'])
        say(f'made the Android debug key {ks}')
    return ks

def pack(out_apk, package, label, version, code, icon, assets, so, bt, jar, work, logs):
    t = work / 'apk'; shutil.rmtree(t, ignore_errors=True); t.mkdir(parents=True)
    icon_attr, res_args = '', []
    if icon and Path(icon).is_file():
        rd = t / 'res/mipmap'; rd.mkdir(parents=True)
        shutil.copy2(icon, rd / f'ic_launcher{Path(icon).suffix.lower()}')
        run([bt / 'aapt2', 'compile', '-o', t / 'res.zip', rd / f'ic_launcher{Path(icon).suffix.lower()}'], logs / 'android_aapt2_compile.log')
        icon_attr, res_args = 'android:icon="@mipmap/ic_launcher" ', [t / 'res.zip']
    (t / 'AndroidManifest.xml').write_text(MANIFEST.format(package=package, code=code, version=xml_attr(version), min_sdk=MIN_SDK, target_sdk=TARGET_SDK, label=xml_attr(label), icon=icon_attr))
    base = t / 'base.apk'
    run([bt / 'aapt2', 'link', '-o', base, '-I', jar, '--manifest', t / 'AndroidManifest.xml', '-A', assets, '--auto-add-overlay', '-0', 'bin'] + res_args, logs / 'android_aapt2_link.log')
    # the host library: stored (not deflated), so that zipalign -P 16 can page-align it and Android maps it straight from the APK (extractNativeLibs false)
    with zipfile.ZipFile(base, 'a') as z:
        zi = zipfile.ZipInfo('lib/arm64-v8a/libnxhost.so', date_time=time.localtime()[:6]); zi.compress_type = zipfile.ZIP_STORED; zi.external_attr = 0o644 << 16
        with open(so, 'rb') as f, z.open(zi, 'w', force_zip64=False) as w: shutil.copyfileobj(f, w, 1 << 20)
    aligned = t / 'aligned.apk'
    run([bt / 'zipalign', '-f', '-P', '16', '4', base, aligned], logs / 'android_zipalign.log')
    ks = debug_keystore()
    out_apk.parent.mkdir(parents=True, exist_ok=True)
    run([bt / 'apksigner', 'sign', '--ks', ks, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--ks-key-alias', 'androiddebugkey', '--out', out_apk, aligned], logs / 'android_apksigner.log')
    run([bt / 'apksigner', 'verify', out_apk], logs / 'android_apksigner_verify.log')
    shutil.rmtree(t, ignore_errors=True)

def push(apk, package, romfs, aoc, sdk_bt):
    adb = shutil.which('adb') or str(Path(sdk_bt).parents[1] / 'platform-tools/adb')
    if not Path(adb).exists() and not shutil.which(adb): raise BuildError('adb not found (sdkmanager "platform-tools")')
    st = subprocess.run([adb, 'get-state'], capture_output=True, text=True)
    if st.stdout.strip() != 'device': raise BuildError(f'no Android device on adb ({st.stderr.strip() or st.stdout.strip()}): connect one with USB debugging on')
    say('installing the APK'); run([adb, 'install', '-r', '-g', apk])
    dst = f'/sdcard/Android/data/{package}/files'
    run([adb, 'shell', f'mkdir -p {dst}'])
    if Path(romfs).is_file():
        say(f'copying the RomFS image to the device ({Path(romfs).stat().st_size / 2**30:.1f} GB)'); run([adb, 'shell', f'rm -rf {dst}/romfs']); run([adb, 'push', romfs, f'{dst}/romfs.img'])
    else:
        say('copying the RomFS folder to the device'); run([adb, 'shell', f'rm -rf {dst}/romfs {dst}/romfs.img']); run([adb, 'push', f'{romfs}/.', f'{dst}/romfs/'])
    if aoc and Path(aoc).is_dir() and any(Path(aoc).iterdir()):
        say('copying the add-on content'); run([adb, 'shell', f'rm -rf {dst}/aoc']); run([adb, 'push', f'{aoc}/.', f'{dst}/aoc/'])
    say(f'done: the game is installed; its log is {dst}/log.txt (adb pull {dst}/log.txt, or adb logcat -s nxnative)')

def build(game, patched, romfs, out_apk, name=None, version='1.0', package=None, icon=None, aoc=None, env_file=None, std_font=None, work=None, do_push=False, env=None, shaders=None):
    """the APK; returns its path. env: the environment the host build runs in (a provisional / derived profile's DM_GAMES_DIR / DM_GAMES_FIRST)"""
    env = dict(env or os.environ)
    sys.path.insert(0, str(DM / 'games')); import game as gamemod
    g = gamemod.load(game)
    ndk, bt, jar = sdk_paths()
    work = Path(work or tempfile.mkdtemp(prefix='nxnative_apk_')); logs = work / 'logs'; work.mkdir(parents=True, exist_ok=True)
    package = package or f'org.nxnative.g{game.lower()}'
    name = name or g.get('name') or game
    say(f'[android] host for {name} ({game}) with the NDK {ndk.name}')
    so = build_host(game, work, ndk, env, logs)
    say('[android] game files into the APK')
    if not shaders: say('[RED] no Vulkan shaders (--shaders): the game\'s draws are skipped, only clears and copies show')
    assets, nbytes = stage_assets(Path(g.dir), Path(patched), g.words('modules'), work, env_file, std_font, logs, shaders)
    code = int(time.time() // 60) & 0x7fffffff   # a later build is always an upgrade
    say(f'[android] packing {out_apk} ({nbytes / 2**20:.0f} MB of game files)')
    pack(Path(out_apk), package, name, version or '1.0', code, icon, assets, so, bt, jar, work, logs)
    say(f'[android] APK: {out_apk} ({Path(out_apk).stat().st_size / 2**20:.0f} MB), package {package}')
    if do_push: push(out_apk, package, romfs, aoc, bt)
    elif romfs: say(f'[android] the RomFS goes to the device separately: adb push {romfs}{"" if Path(romfs).is_file() else "/."} /sdcard/Android/data/{package}/files/{"romfs.img" if Path(romfs).is_file() else "romfs/"} (or run again with --push)')
    return Path(out_apk)

def main():
    ap = argparse.ArgumentParser(description='Pack an Android APK for one game from its rewritten modules (dm/tools/patch_nso.py with DM_TARGET=android)')
    ap.add_argument('--game', required=True, help='base game title ID (the profile dm/games/<id>)')
    ap.add_argument('--patched', required=True, help='folder of the rewritten modules (<module>.patched.bin and tables)')
    ap.add_argument('--romfs', help='the RomFS folder or image (pushed with --push; never packed into the APK)')
    ap.add_argument('--aoc', help='folder of extracted add-on content (pushed with --push)')
    ap.add_argument('-o', '--out', required=True, help='output .apk')
    ap.add_argument('--name', help='app name (default: the profile\'s game name)')
    ap.add_argument('--version', default='1.0', help='version name')
    ap.add_argument('--package', help='Android package name (default org.nxnative.g<title ID>)')
    ap.add_argument('--icon', help='app icon (PNG or JPEG)')
    ap.add_argument('--env-file', help='environment switches packed as env.txt')
    ap.add_argument('--standard-font', help='a TTF packed as the standard shared font (default: an Android system font at run time)')
    ap.add_argument('--shaders', help='the Vulkan shaders: the output folder of dm/android/tools/nvn_vk_shaders.py')
    ap.add_argument('--work', help='work folder (default: a temporary one)')
    ap.add_argument('--push', action='store_true', help='install on the connected device and copy the RomFS and add-on content to it (adb)')
    a = ap.parse_args()
    try: build(a.game, a.patched, a.romfs, a.out, a.name, a.version, a.package, a.icon, a.aoc, a.env_file, a.standard_font, a.work, a.push, shaders=a.shaders)
    except BuildError as e: print(f'error: {e}', file=sys.stderr); sys.exit(1)

if __name__ == '__main__':
    main()
