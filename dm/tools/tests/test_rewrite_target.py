# patch_nso.py's DM_TARGET: the same module rewritten for Apple and for Android differs only in how a thread slot's base is read (emit_tsd_base).
# A synthetic NSO (x18 read and write, tpidr_el0 and tpidrro_el0 reads, an svc) goes through the whole rewriter for each target
import os, struct, subprocess, sys, tempfile, unittest
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1]
CODE = [0xD53BD040, 0xD53BD061, 0xAA0003F2, 0xAA1203E2, 0xD4000021, 0xD65F03C0]   # mrs x0,tpidr_el0; mrs x1,tpidrro_el0; mov x18,x0; mov x2,x18; svc #1; ret

def nso():
    text = bytearray(0x1000); struct.pack_into('<II', text, 0, 0x14000002, 0x800)   # b over the MOD0 offset word
    for i, w in enumerate(CODE): struct.pack_into('<I', text, 8 + 4 * i, w)
    text[0x800:0x81C] = b'MOD0' + struct.pack('<6i', 0x800, 0, 0, 0x900, 0x920, 0)   # dynamic at ro+0, eh_frame_hdr at ro+0x100
    ro = bytearray(0x1000); ro[0x100:0x114] = bytes([1, 0x1B, 0x03, 0x3B]) + struct.pack('<iIii', 0, 1, 8 - 0x1100, 0)   # one FDE: the function at 8
    data = bytes(0x1000)
    h = bytearray(0x100); h[:4] = b'NSO0'
    for i, (fo, mo) in enumerate(((0x100, 0), (0x1100, 0x1000), (0x2100, 0x2000))): struct.pack_into('<III', h, 0x10 + 0x10 * i, fo, mo, 0x1000)
    struct.pack_into('<I', h, 0x3C, 0x1000); h[0x40:0x50] = bytes(range(16)); struct.pack_into('<III', h, 0x60, 0x1000, 0x1000, 0x1000)
    return bytes(h + text + ro + data)

class RewriteTarget(unittest.TestCase):
    def rewrite(self, target):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 't.nso'; src.write_bytes(nso())
            r = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(src), d, 'main'], env=dict(os.environ, DM_TARGET=target), capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            fix = (Path(d) / 'main.fixups.tsv').read_text()
            img = (Path(d) / 'main.patched.bin').read_bytes()
        po = int(next(l.split('\t')[1] for l in fix.splitlines() if l.startswith('# PATCH_OFF')), 16)
        return fix, [f'{i.mnemonic} {i.op_str}' for i in Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(img[po:], po)]

    def test_apple_reads_the_tsd_register(self):
        fix, ins = self.rewrite('apple')
        self.assertIn('# TARGET\tapple', fix)
        self.assertEqual(sum('tpidrro_el0' in x for x in ins), 5)
        self.assertFalse(any(x.endswith('tpidr_el0') for x in ins))

    def test_android_reads_bionic_tls_through_tls_slot_minus_1(self):
        fix, ins = self.rewrite('android')
        self.assertIn('# TARGET\tandroid', fix)
        self.assertFalse(any('tpidrro_el0' in x for x in ins))
        reads = [i for i, x in enumerate(ins) if x.endswith('tpidr_el0')]
        self.assertEqual(len(reads), 5)
        for i in reads:
            r = ins[i].split()[1].rstrip(',')
            self.assertEqual(ins[i + 1], f'ldur {r}, [{r}, #-8]')

    def test_unknown_target_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 't.nso'; src.write_bytes(nso())
            r = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(src), d, 'main'], env=dict(os.environ, DM_TARGET='windows'), capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)

if __name__ == '__main__':
    unittest.main()
