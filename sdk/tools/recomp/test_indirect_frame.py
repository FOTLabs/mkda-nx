import pathlib
import shutil
import subprocess
import tempfile
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter
from . import config
from .translator import FunctionTranslator


class IndirectFrameTest(unittest.TestCase):
    def test_frameless_dispatcher_publishes_its_live_ebp(self):
        # mov ecx,ebp; call eax; ret -- inherits a frame, owns no prologue.
        base = 0x10000
        image = bytes.fromhex("89e9ffd0c3")
        config._install([config.Section(".text", base, len(image), 0,
                                        len(image), True)],
                        entry_point=base, kernel_thunk_addr=base,
                        origin="frameless-dispatcher-test")
        db = {base: {"start": hex(base), "end": base + len(image),
                     "_addr": base, "size": len(image)}}
        code = FunctionTranslator(image, db).translate_function(base, db[base])
        self.assertIn("g_ebp = ebp; /* frame stays current across indirect calls */", code)
        self.assertIn("g_seh_ebp = ebp;", code)

    def indirect(self, operand):
        instruction = Instruction(0x200, 3, "call", "", "")
        instruction.operands = [operand]
        lifter = Lifter()
        lifter.publishes_ebp = True
        return lifter.lift_instruction(instruction)

    def test_register_and_memory_calls_publish_caller_frame(self):
        for operand in (Operand(type="reg", reg="eax"),
                        Operand(type="mem", mem_base="eax", mem_disp=0x14, mem_size=4)):
            lines = self.indirect(operand)
            self.assertIn("g_ebp = ebp;", lines[0])
            self.assertEqual(lines[1], "g_seh_ebp = ebp;")

    def test_recorded_target_guard_still_publishes_frame(self):
        lifter = Lifter(func_db={0x1200: {}})
        lifter.publishes_ebp = True
        lifter.icall_site_targets = {0x200: [0x1200]}
        instruction = Instruction(0x200, 2, "call", "eax", "ffd0")
        instruction.operands = [Operand(type="reg", reg="eax")]
        lines = lifter.lift_instruction(instruction)
        self.assertIn("g_ebp = ebp;", lines[0])
        self.assertEqual(lines[1], "g_seh_ebp = ebp;")
        self.assertIn("RECOMP_ICALL_GUARD_HIT", lines[2])

    @unittest.skipUnless(shutil.which("gcc"), "native regression requires GCC")
    def test_frameless_callback_inherits_caller_after_nested_frame(self):
        lifter = Lifter()
        lifter.publishes_ebp = True
        direct = Instruction(0x100, 5, "call", "", "", call_target=0x1100)
        code = r'''
#include <assert.h>
#include <stdint.h>
static uint32_t ram[1024], g_ebp, g_seh_ebp, esp;
#define MEM32(a) ram[(uint32_t)(a)/4]
#define PUSH32(s,v) ((s)-=4, MEM32(s)=(v))
#define RECOMP_ABI_CALL(va, fn) fn()
#define RECOMP_ICALL_SAFE_AT(va, saved, site) callback()
static void sub_00001100(void) { g_ebp = g_seh_ebp = 0x40; esp += 4; }
static void callback(void) {
    assert(g_ebp == 0x200 && g_seh_ebp == 0x200);
    MEM32(g_seh_ebp - 16) = 0xcafebabe;
    esp += 4;
}
int main(void) {
    uint32_t ebp = 0x200, eax = 0x1200;
    g_ebp = g_seh_ebp = ebp; esp = 0x300;
'''
        code += "\n".join(lifter.lift_instruction(direct))
        code += "\nuint32_t _icall_esp = esp;\n"
        code += "\n".join(self.indirect(Operand(type="reg", reg="eax")))
        code += "\nassert(esp == 0x300); assert(MEM32(0x1f0) == 0xcafebabe); return 0; }\n"
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "frame.c"
            binary = pathlib.Path(directory) / "frame"
            source.write_text(code)
            subprocess.run(["gcc", "-O2", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], cwd=directory, check=True)
