import unittest
from .disasm import Instruction, Operand
from .lifter import Lifter

class Float80Test(unittest.TestCase):
    def test_extended_load_and_stores(self):
        for mnemonic in ('fld', 'fst', 'fstp'):
            instruction = Instruction(0, 3, mnemonic, 'tbyte ptr [esp]', '')
            instruction.operands = [Operand(type='mem', mem_base='esp', mem_size=10)]
            result = '\n'.join(Lifter().lift_instruction(instruction))
            self.assertIn('MEM80(' if mnemonic == 'fld' else 'STORE80(', result)
            self.assertEqual('fp_pop()' in result, mnemonic == 'fstp')
            self.assertNotIn('MEMF(', result)

if __name__ == '__main__':
    unittest.main()
