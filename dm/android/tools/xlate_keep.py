#!/usr/bin/env python3
# The shader translator as dmsh_build.py runs it (XLATE=<this>), keeping what the Android Vulkan backend needs: it runs the in-repo translator
# (dm/gfx/tools/dmsh_xlate_bin, through dotnet) on the job file, then copies each translated shader's <key>.spv and <key>.res.json, and its c1 constant segment from the
# job, into $DMSH_SPV_OUT, from where dm/android/tools/nvn_vk_shaders.py makes the Vulkan shaders. dmsh_build.py itself is left as it is (it is one of the shader chain's
# versioned tools). Usage (by dmsh_build.py): xlate_keep.py <job.json> <output dir>
import json, os, shutil, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
dll = os.path.join(here, '../../gfx/tools/dmsh_xlate_bin/dmsh_xlate.dll')
job, outdir = sys.argv[1], sys.argv[2]
r = subprocess.run([os.environ.get('DOTNET', 'dotnet'), dll, job, outdir])
keep = os.environ.get('DMSH_SPV_OUT')
if keep:
    os.makedirs(keep, exist_ok=True)
    for j in json.load(open(job)):
        k = j['key']
        if j.get('stage') not in ('vs', 'ps', 'cs') or not os.path.exists(os.path.join(outdir, k + '.spv')): continue
        for ext in ('.spv', '.res.json'): shutil.copyfile(os.path.join(outdir, k + ext), os.path.join(keep, k + ext))
        open(os.path.join(keep, k + '.c1'), 'wb').write(bytes.fromhex(j.get('c1', '')))
sys.exit(r.returncode)
