import unittest
from .disasm import Instruction
from .lifter import Lifter


class MemoryFencesTest(unittest.TestCase):
    def test_cache_flush_and_fences_preserve_host_ordering(self):
        for mnemonic in ("wbinvd", "mfence", "sfence", "lfence"):
            instruction = Instruction(0x1de572, 2, mnemonic, "", "")
            self.assertEqual(Lifter().lift_instruction(instruction),
                             [f"recomp_memory_fence(); /* {mnemonic}: coherent host memory */"])
