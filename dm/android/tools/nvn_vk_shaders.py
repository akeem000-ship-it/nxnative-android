#!/usr/bin/env python3
# The translator's shaders made ready for the Android Vulkan backend (dm/android/gfx/nvn_vk.c): from the translation directory dmsh_build.py keeps with DMSH_SPV_OUT
# (<key>.spv, <key>.res.json, <key>.c1 per shader) to the files the backend loads from its shader directory:
#   <key>.spv      the shader for phones (spv_mobile.py: unused optional capabilities dropped), its resources in the stage's descriptor set (vertex and compute 0, pixel 1)
#   <key>.gl.spv   vertex shaders: the same with NVN's GL depth convention (z = (z + w) / 2), for a game that keeps the default depth mode
#   <key>.c1       the shader's own constant segment (c1), as given
#   <key>.res      what the backend binds, one line each, from res.json (the NVN meaning) and the SPIR-V (the Vulkan descriptor type, count and interface types):
#                    stage vs|ps|cs
#                    local <x> <y> <z>                                  (compute: the workgroup size)
#                    b <binding> <vk descriptor type> <count> <role ...>   role: support | cbuf <slot> | sbuf <cb_slot> <cb_offset> <flags>
#                                                                        | tex <type> <handle> <cbuf_slot> | img <type> <handle> <cbuf_slot>
#                    in <location> <f|i|u> <components>                 (vertex inputs, by NVN attribute number)
#                    out <location> <f|i|u>                             (pixel outputs, by render target)
# Usage: nvn_vk_shaders.py <translation dir> <output dir> [key ...]   (no keys = every <key>.spv there)
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spv_mobile as sm

SET_OF = {'vs': 0, 'ps': 1, 'cs': 0}
# VkDescriptorType values
UNIFORM_BUFFER, STORAGE_BUFFER, COMBINED, SAMPLED_IMAGE, SAMPLER, STORAGE_IMAGE, UNIFORM_TEXEL, STORAGE_TEXEL = 6, 7, 1, 2, 0, 3, 4, 5
OP_NAME, OP_MEMBER_NAME = 5, 6

def reflect(spv):
    """per binding: (descriptor type, count); inputs / outputs: location → (f|i|u, components); the compute workgroup size"""
    head, insns = sm._insns(spv)
    ty, binding, location, vars_, local = {}, {}, {}, [], None
    for ins in insns:
        op = ins[0] & 0xffff
        if op in (sm.OP_TYPE_INT, sm.OP_TYPE_FLOAT, sm.OP_TYPE_VECTOR, sm.OP_TYPE_POINTER, sm.OP_TYPE_STRUCT, 25, 26, 27, 28, 29): ty[ins[1]] = ins   # also Image, Sampler, SampledImage, Array, RuntimeArray
        elif op == sm.OP_DECORATE and ins[2] == 33: binding[ins[1]] = ins[3]   # Binding
        elif op == sm.OP_DECORATE and ins[2] == 30: location[ins[1]] = ins[3]  # Location
        elif op == sm.OP_VARIABLE: vars_.append(ins)
        elif op == sm.OP_EXECUTION_MODE and ins[2] == 17: local = ins[3:6]    # LocalSize
        elif op == 43: ty[ins[2]] = ('const', ins[3])                          # OpConstant (array lengths)
    bufblocks = {ins[1] for ins in insns if (ins[0] & 0xffff) == sm.OP_DECORATE and ins[2] == 3}     # BufferBlock (older storage buffers)
    def strip_array(t):
        n = 1
        while isinstance(ty.get(t), list) and (ty[t][0] & 0xffff) in (28, 29):
            if (ty[t][0] & 0xffff) == 28: c = ty.get(ty[t][3]); n *= c[1] if isinstance(c, tuple) else 1
            else: n = 0   # runtime array: the backend binds what the shader can index (QueryTextureArrayLength: 32)
            t = ty[t][2]
        return t, n
    def scalar_kind(t):
        e = ty.get(t)
        while isinstance(e, list) and (e[0] & 0xffff) == sm.OP_TYPE_VECTOR: e = ty.get(e[2])
        if not isinstance(e, list): return 'f', 1
        return ('f' if (e[0] & 0xffff) == sm.OP_TYPE_FLOAT else 'i' if e[3] else 'u')
    def comps(t): e = ty.get(t); return e[3] if isinstance(e, list) and (e[0] & 0xffff) == sm.OP_TYPE_VECTOR else 1
    res, ins_, outs = {}, {}, {}
    for v in vars_:
        pt = ty.get(v[1]); sc = v[3]
        if not isinstance(pt, list): continue
        t, n = strip_array(pt[3])
        e = ty.get(t)
        if v[2] in binding and isinstance(e, list):
            k = e[0] & 0xffff
            if k == sm.OP_TYPE_STRUCT: d = STORAGE_BUFFER if sc == 12 or t in bufblocks else UNIFORM_BUFFER   # StorageClass StorageBuffer
            elif k == 27: d = COMBINED                                                                     # OpTypeSampledImage
            elif k == 26: d = SAMPLER
            elif k == 25:                                                                                  # OpTypeImage: Dim 5 = Buffer, Sampled 1 / 2
                d = (UNIFORM_TEXEL if e[7] == 1 else STORAGE_TEXEL) if e[3] == 5 else (SAMPLED_IMAGE if e[7] == 1 else STORAGE_IMAGE)
            else: continue
            res[binding[v[2]]] = (d, n)
        elif v[2] in location and sc in (1, 3):   # Input / Output
            (ins_ if sc == 1 else outs)[location[v[2]]] = (scalar_kind(t), comps(t))
    return res, ins_, outs, local

def convert(src_dir, out_dir, key):
    spv = open(os.path.join(src_dir, key + '.spv'), 'rb').read()
    r = json.load(open(os.path.join(src_dir, key + '.res.json')))
    stage = r['stage']
    if stage not in SET_OF: return f'{key}: stage {stage} is not drawn by the Vulkan backend yet'
    mobile, _dropped = sm.strip(spv)
    mobile = sm.set_descriptor_set(mobile, SET_OF[stage])
    open(os.path.join(out_dir, key + '.spv'), 'wb').write(mobile)
    if stage == 'vs':
        gl = sm.gl_depth(mobile)
        if gl: open(os.path.join(out_dir, key + '.gl.spv'), 'wb').write(gl)
    c1 = os.path.join(src_dir, key + '.c1')
    if os.path.exists(c1): open(os.path.join(out_dir, key + '.c1'), 'wb').write(open(c1, 'rb').read())
    desc, ins_, outs, local = reflect(mobile)
    role = {0: 'support'}
    for b in r['cbufs']: role[b['binding']] = f"cbuf {b['slot']}"
    for b in r['sbufs']: role[b['binding']] = f"sbuf {b['cb_slot']} {b['cb_offset']} {b['flags']}"
    for b in r['textures']: role[b['binding']] = f"tex {b['type']} {b['handle']} {b['cbuf_slot']}"
    for b in r['images']: role[b['binding']] = f"img {b['type']} {b['handle']} {b['cbuf_slot']}"
    lines = [f'stage {stage}']
    if stage == 'cs': lines.append('local ' + ' '.join(str(x) for x in (local or (1, 1, 1))))
    for b in sorted(desc):
        d, n = desc[b]
        lines.append(f'b {b} {d} {n if n else 32} {role.get(b, "unknown")}')
    if stage == 'vs':
        for loc in sorted(ins_): lines.append(f'in {loc} {ins_[loc][0]} {ins_[loc][1]}')
    if stage == 'ps':
        for loc in sorted(outs): lines.append(f'out {loc} {outs[loc][0]}')
    open(os.path.join(out_dir, key + '.res'), 'w').write('\n'.join(lines) + '\n')
    return None

def main():
    src, out = sys.argv[1], sys.argv[2]; keys = sys.argv[3:]
    os.makedirs(out, exist_ok=True)
    if not keys: keys = sorted(f[:-4] for f in os.listdir(src) if f.endswith('.spv') and not f.endswith('.gl.spv') and not f.endswith('.reader.spv'))
    bad = [m for m in (convert(src, out, k) for k in keys) if m]
    for m in bad: print('[RED] ' + m)
    print(f'{len(keys) - len(bad)} shaders for Vulkan, {len(bad)} not')
    return 1 if bad and len(bad) == len(keys) else 0

if __name__ == '__main__':
    sys.exit(main())
