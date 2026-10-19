import unittest
from .disasm import Instruction
from .lifter import Lifter


class InterruptControlTest(unittest.TestCase):
    def test_cli_and_sti_change_guest_interrupt_state(self):
        for name, enabled in (("cli", 0), ("sti", 1)):
            instruction = Instruction(0x23472b, 1, name, "", "")
            self.assertEqual(Lifter().lift_instruction(instruction),
                             [f"recomp_set_interrupts_enabled({enabled}); /* {name} */"])
