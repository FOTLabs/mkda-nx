"""Comparison snapshots must survive a join of different CMP operands."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator, _merge_flag_states
from tools.recomp.disasm import Operand

BASE = 0x10000

def translate_join(consumer=bytes.fromhex('0f95c0c3')):
    # test ecx,ecx; jz alternate; cmp eax,edx; jmp join; nop;
    # alternate: cmp ebx,esi; join: consumer.
    image = bytes.fromhex('85c9740539d0eb039039f3') + consumer
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                              entry_point=BASE, kernel_thunk_addr=BASE,
                              origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

def test_different_cmp_operands_join_for_setne():
    code = translate_join()
    assert 'CMP_NE(_fa, _fb)' in code, code
    assert '_flags /* setne */' not in code

def test_different_cmp_operands_join_for_cmovne():
    code = translate_join(bytes.fromhex('0f45c7c3'))
    assert 'if (CMP_NE(_fa, _fb)) eax = edi;' in code, code

def test_unknown_path_is_not_guessed():
    assert _merge_flag_states([None, ('cmp', [])]) is None

def test_mixed_operations_are_not_guessed():
    a = Operand(type='reg', reg='eax')
    b = Operand(type='reg', reg='edx')
    assert _merge_flag_states([('cmp', [a, b]), ('test', [a, b])]) is None

def test_mixed_widths_merge_as_marker():
    wide = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
    narrow = [Operand(type='reg', reg='al'), Operand(type='reg', reg='dl')]
    merged = _merge_flag_states([('cmp', wide), ('cmp', narrow)])
    assert merged is not None and merged[0] == '__cmp_mixed', merged

def test_mixed_width_marker_answers_only_exact_conditions():
    from tools.recomp.lifter import _make_condition
    ops = [Operand(type='reg', reg='eax'), Operand(type='imm', imm=7)]
    assert 'CMP_NE(_fa, _fb)' in _make_condition('jne', '__cmp_mixed', ops)[0]
    assert '_fas' in _make_condition('jl', '__cmp_mixed', ops)[0]
    assert _make_condition('js', '__cmp_mixed', ops) is None
    assert _make_condition('jo', '__cmp_mixed', ops) is None
    assert _make_condition('jb', '__test_mixed', ops) is None

def test_mixed_width_cmp_join_for_jne():
    # Conker 0x00553293..0x0055329D: cmp eax,7; jmp join / cmp word [esi+0x30],7;
    # join: jne. Here: test ecx,ecx; jz alt; cmp eax,7; jmp join; alt: cmp
    # word [esi+0x30],7; join: setne al; ret.
    image = bytes.fromhex('85c9740583f807eb06' '66837e3007' '0f95c0c3')
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    code = FunctionTranslator(image, db).translate_function(BASE, db[BASE])
    assert 'CMP_NE(_fa, _fb)' in code, code
    assert '_flags /* setne */' not in code, code
