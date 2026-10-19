import unittest
from .disasm import Instruction, Operand
from .lifter import Lifter


class PortIoTest(unittest.TestCase):
    def test_input_preserves_upper_accumulator_bits(self):
        insn = Instruction(0x1df04c, 1, "in", "al, dx", "ec")
        insn.operands = [Operand(type="reg", reg="al"), Operand(type="reg", reg="dx")]
        self.assertEqual(Lifter().lift_instruction(insn),
                         ["SET_LO8(eax, recomp_port_read(LO16(edx), 1, 0x001DF04Cu));"])

    def test_output_passes_port_value_width_and_guest_pc(self):
        for register, value, width in (("al", "LO8(eax)", 1), ("ax", "LO16(eax)", 2),
                                       ("eax", "eax", 4)):
            insn = Instruction(0x1e0122, 1, "out", "dx, " + register, "")
            insn.operands = [Operand(type="reg", reg="dx"), Operand(type="reg", reg=register)]
            self.assertEqual(Lifter().lift_instruction(insn),
                             [f"recomp_port_write(LO16(edx), {value}, {width}, 0x001E0122u);"])
