"""fincstp/fdecstp move the x87 TOP (found lifting Conker: Live & Reloaded,
whose rotation builder at 0x003E2E60 uses fincstp after three fsincos)."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

def translate(hexcode):
    image=bytes.fromhex(hexcode)
    base=0x10000
    config._install([config.Section('.text',base,len(image),0,len(image),True)],
                    entry_point=base,kernel_thunk_addr=base,origin='x87-stack-ptr-test')
    db={base:{'start':hex(base),'end':base+len(image),'_addr':base,'size':len(image)}}
    return FunctionTranslator(image,db).translate_function(base,db[base])

def test_fincstp_moves_top():
    # fld1; fldz; fincstp; ret
    code=translate('d9e8d9eed9f7c3')
    assert 'fp_pop(); /* fincstp */' in code
    assert '/* FPU: fincstp' not in code

def test_ficomp_compares_memory_integer_and_pops():
    # fld1; ficomp word ptr [eax]; ret
    code=translate('d9e8de18c3')
    assert 'RECOMP_FCMP(fp_top(), (double)' in code
    assert 'fp_pop(); /* ficomp' in code

def test_fisttp_truncates_and_pops():
    # fld1; fisttp dword ptr [ecx]; ret
    code=translate('d9e8db09c3')
    assert 'trunc(fp_top()); fp_pop(); /* fisttp */' in code

def test_fxtract():
    # fld1; fxtract; ret
    code=translate('d9e8d9f4c3')
    assert '/* fxtract */' in code and 'frexp' in code

def test_fcmovnbe_after_fcomi():
    # fld1; fldz; fcomi st(1); fcmovnbe st(0), st(1); ret
    code=translate('d9e8d9eedbf1dbd1c3')
    assert 'fp_top() = fp_st(1); /* fcmovnbe' in code

def test_unhandled_x87_is_loud():
    # fbld tbyte ptr [eax]; ret -- not modelled: must be RECOMP_UNIMPL
    code=translate('df20c3')
    assert 'RECOMP_UNIMPL("fbld' in code
    assert '/* FPU: fbld' not in code

def test_fdecstp_moves_top():
    # fld1; fdecstp; ret
    code=translate('d9e8d9f6c3')
    assert '/* fdecstp */' in code
    assert '/* FPU: fdecstp' not in code
