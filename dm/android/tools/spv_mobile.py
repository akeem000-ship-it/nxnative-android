#!/usr/bin/env python3
# The translator's SPIR-V (dm/gfx/tools/dmsh_xlate, Ryujinx) for phones: Ryujinx declares capabilities by its desktop feature set, whether a shader uses them or not
# (Float64, FragmentShaderPixelInterlockEXT, the subgroup ballot / shuffle / vote groups, DrawParameters), and a driver may refuse a module that declares one the device lacks
# (most Adreno and Mali GPUs have no Float64 or pixel interlock). Each optional capability, with the extension and execution modes that come with it, is dropped when the module
# has no instruction, type or builtin that needs it; a used one stays (the device must have it; nvn_vk.c enables what the device offers).
# Usage: spv_mobile.py <in.spv> <out.spv> (prints the capabilities dropped); also imported (strip(bytes) → (bytes, dropped))
import struct, sys

OP_CAPABILITY, OP_EXTENSION, OP_EXECUTION_MODE, OP_DECORATE, OP_MEMBER_DECORATE = 17, 10, 16, 71, 72
OP_TYPE_INT, OP_TYPE_FLOAT = 21, 22
CAP = {'Float64': 10, 'Int64': 11, 'FragmentShaderPixelInterlockEXT': 5378, 'FragmentShaderSampleInterlockEXT': 5363, 'FragmentShaderShadingRateInterlockEXT': 5372,
       'GroupNonUniformVote': 62, 'GroupNonUniformArithmetic': 63, 'GroupNonUniformBallot': 64, 'GroupNonUniformShuffle': 65, 'GroupNonUniformShuffleRelative': 66,
       'GroupNonUniformClustered': 67, 'GroupNonUniformQuad': 68, 'DrawParameters': 4427}
# What makes a capability used: opcodes, builtins (decorations), or 64-bit types
USES = {
    'FragmentShaderPixelInterlockEXT': {'ops': {5364, 5365}},   # OpBeginInvocationInterlockEXT / OpEndInvocationInterlockEXT
    'FragmentShaderSampleInterlockEXT': {'ops': {5364, 5365}}, 'FragmentShaderShadingRateInterlockEXT': {'ops': {5364, 5365}},
    'GroupNonUniformVote': {'ops': {334, 335, 336}},
    'GroupNonUniformArithmetic': {'ops': set(range(349, 362))},
    'GroupNonUniformBallot': {'ops': set(range(337, 345)), 'builtins': {4416, 4417, 4418, 4419, 4420}},   # broadcast / ballot ops, SubgroupEq..LtMask
    'GroupNonUniformShuffle': {'ops': {345, 346}}, 'GroupNonUniformShuffleRelative': {'ops': {347, 348}},
    'GroupNonUniformClustered': {'ops': set()}, 'GroupNonUniformQuad': {'ops': {365, 366}},
    'DrawParameters': {'builtins': {4424, 4425, 4426}},   # BaseVertex, BaseInstance, DrawIndex
    'Float64': {'float': 64}, 'Int64': {'int': 64},
}
EXT_OF = {'FragmentShaderPixelInterlockEXT': 'SPV_EXT_fragment_shader_interlock', 'FragmentShaderSampleInterlockEXT': 'SPV_EXT_fragment_shader_interlock',
          'FragmentShaderShadingRateInterlockEXT': 'SPV_EXT_fragment_shader_interlock', 'DrawParameters': 'SPV_KHR_shader_draw_parameters'}
INTERLOCK_MODES = {5366, 5367, 5368, 5369, 5370, 5371}   # PixelInterlockOrderedEXT … ShadingRateInterlockUnorderedEXT
BUILTIN = 11

def words(b): return list(struct.unpack(f'<{len(b) // 4}I', b))

def strip(spv):
    w = words(spv)
    if w[0] != 0x07230203: raise ValueError('not SPIR-V')
    insns, i = [], 5
    while i < len(w):
        n = w[i] >> 16
        if n == 0: raise ValueError(f'bad instruction length at word {i}')
        insns.append(w[i:i + n]); i += n
    ops, builtins, ints, floats = set(), set(), set(), set()
    for ins in insns:
        op = ins[0] & 0xffff; ops.add(op)
        if op == OP_DECORATE and len(ins) >= 4 and ins[2] == BUILTIN: builtins.add(ins[3])
        if op == OP_MEMBER_DECORATE and len(ins) >= 5 and ins[3] == BUILTIN: builtins.add(ins[4])
        if op == OP_TYPE_INT: ints.add(ins[2])
        if op == OP_TYPE_FLOAT: floats.add(ins[2])
    def used(name):
        u = USES[name]
        return bool(u.get('ops', set()) & ops) or bool(u.get('builtins', set()) & builtins) or u.get('float') in floats or u.get('int') in ints
    drop = {name for name, c in CAP.items() if any((ins[0] & 0xffff) == OP_CAPABILITY and ins[1] == c for ins in insns) and not used(name)}
    caps = {CAP[n] for n in drop}
    kept_caps = {ins[1] for ins in insns if (ins[0] & 0xffff) == OP_CAPABILITY and ins[1] not in caps}
    drop_ext = {EXT_OF[n] for n in drop if n in EXT_OF} - {EXT_OF[n] for n, c in CAP.items() if n in EXT_OF and c in kept_caps}
    out = []
    for ins in insns:
        op = ins[0] & 0xffff
        if op == OP_CAPABILITY and ins[1] in caps: continue
        if op == OP_EXTENSION and struct.pack(f'<{len(ins) - 1}I', *ins[1:]).split(b'\0')[0].decode() in drop_ext: continue
        if op == OP_EXECUTION_MODE and ins[2] in INTERLOCK_MODES and not ({5378, 5363, 5372} & kept_caps): continue
        out += ins
    return struct.pack(f'<{5 + len(out)}I', *w[:5], *out), sorted(drop)

OP_DECORATION_SET, OP_TYPE_VOID, OP_TYPE_VECTOR, OP_TYPE_POINTER, OP_TYPE_STRUCT, OP_CONSTANT, OP_VARIABLE = 34, 19, 23, 32, 30, 43, 59
OP_FUNCTION, OP_FUNCTION_END, OP_RETURN, OP_LOAD, OP_STORE, OP_ACCESS_CHAIN = 54, 56, 253, 61, 62, 65
OP_COMPOSITE_EXTRACT, OP_COMPOSITE_INSERT, OP_FADD, OP_FMUL, OP_ENTRY_POINT, OP_LABEL = 81, 82, 129, 133, 15, 248
DESCRIPTOR_SET, POSITION, STORAGE_OUTPUT = 34, 0, 3

def _insns(spv):
    w = words(spv); out, i = [], 5
    while i < len(w): n = w[i] >> 16; out.append(w[i:i + n]); i += n
    return w[:5], out
def _pack(head, insns): flat = [x for ins in insns for x in ins]; return struct.pack(f'<{5 + len(flat)}I', *head, *flat)

def set_descriptor_set(spv, n):
    """every resource into descriptor set n (the vertex and pixel shaders of a draw both number their bindings from 1: each stage gets its own set, nvn_vk.c)"""
    head, insns = _insns(spv)
    for ins in insns:
        if (ins[0] & 0xffff) == OP_DECORATE and len(ins) >= 4 and ins[2] == DESCRIPTOR_SET: ins[3] = n
    return _pack(head, insns)

def gl_depth(spv):
    """NVN's GL depth convention (near = -w) for Vulkan's 0..w: gl_Position.z = (z + w) / 2 before each return of the vertex entry point (the Metal build's main0; the unchanged
    shader is its main0_z, for a game that sets near = 0). None when the module has no Position output"""
    head, insns = _insns(spv)
    nid = [head[3]]
    def new(): nid[0] += 1; return nid[0] - 1
    entry = next((ins[2] for ins in insns if (ins[0] & 0xffff) == OP_ENTRY_POINT and ins[1] == 0), None)   # ExecutionModel Vertex
    if entry is None: return None
    types = {}   # id → instruction
    for ins in insns:
        if (ins[0] & 0xffff) in (OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_POINTER, OP_TYPE_STRUCT): types[ins[1]] = ins
    f32 = next((i for i, t in types.items() if (t[0] & 0xffff) == OP_TYPE_FLOAT and t[2] == 32), None)
    v4 = next((i for i, t in types.items() if (t[0] & 0xffff) == OP_TYPE_VECTOR and t[2] == f32 and t[3] == 4), None)
    if f32 is None or v4 is None: return None
    # the Position output: a gl_PerVertex block member, or a variable of its own
    pos_member = {(ins[1], ins[2]) for ins in insns if (ins[0] & 0xffff) == OP_MEMBER_DECORATE and ins[3] == BUILTIN and ins[4] == POSITION}
    pos_var = {ins[1] for ins in insns if (ins[0] & 0xffff) == OP_DECORATE and ins[2] == BUILTIN and ins[3] == POSITION}
    var, member = None, None
    for ins in insns:
        if (ins[0] & 0xffff) == OP_VARIABLE and ins[3] == STORAGE_OUTPUT:
            pt = types.get(ins[1]); st = pt[3] if pt else None
            if ins[2] in pos_var: var = ins[2]
            for s_, m in pos_member:
                if s_ == st: var, member = ins[2], m
    if var is None: return None
    new_types = []
    ptr = next((i for i, t in types.items() if (t[0] & 0xffff) == OP_TYPE_POINTER and t[2] == STORAGE_OUTPUT and t[3] == v4), None)
    if ptr is None: ptr = new(); new_types.append([(4 << 16) | OP_TYPE_POINTER, ptr, STORAGE_OUTPUT, v4])
    half = new(); new_types.append([(4 << 16) | OP_CONSTANT, f32, half, 0x3f000000])
    idx = None
    if member is not None:
        i32 = next((i for i, t in types.items() if (t[0] & 0xffff) == OP_TYPE_INT and t[2] == 32 and t[3] == 1), None)
        if i32 is None: i32 = new(); new_types.append([(4 << 16) | OP_TYPE_INT, i32, 32, 1])
        idx = new(); new_types.append([(4 << 16) | OP_CONSTANT, i32, idx, member])
    out, in_entry, done_types, n = [], False, False, 0
    for ins in insns:
        op = ins[0] & 0xffff
        if op == OP_FUNCTION and not done_types: out += new_types; done_types = True
        if op == OP_FUNCTION: in_entry = ins[2] == entry
        if op == OP_FUNCTION_END: in_entry = False
        if in_entry and op == OP_RETURN:
            p = var
            if idx is not None: p = new(); out.append([(5 << 16) | OP_ACCESS_CHAIN, ptr, p, var, idx])
            ld, z, w_, s_, h, np_ = new(), new(), new(), new(), new(), new()
            out += [[(4 << 16) | OP_LOAD, v4, ld, p], [(5 << 16) | OP_COMPOSITE_EXTRACT, f32, z, ld, 2], [(5 << 16) | OP_COMPOSITE_EXTRACT, f32, w_, ld, 3],
                    [(5 << 16) | OP_FADD, f32, s_, z, w_], [(5 << 16) | OP_FMUL, f32, h, s_, half], [(6 << 16) | OP_COMPOSITE_INSERT, v4, np_, h, ld, 2], [(3 << 16) | OP_STORE, p, np_]]
            n += 1
        out.append(ins)
    if not n: return None
    head = list(head); head[3] = nid[0]
    return _pack(head, out)

if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(); ap.add_argument('src'); ap.add_argument('dst'); ap.add_argument('--set', type=int); ap.add_argument('--gl-depth', action='store_true')
    a = ap.parse_args()
    data, dropped = strip(open(a.src, 'rb').read())
    if a.set is not None: data = set_descriptor_set(data, a.set)
    if a.gl_depth: data = gl_depth(data) or data
    open(a.dst, 'wb').write(data)
    print(' '.join(dropped) if dropped else '(none)')
