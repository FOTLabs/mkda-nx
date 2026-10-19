"""Ordered branches after a narrow SUB compare at the operand's width, and LAHF
declares the CF it reads (both found lifting Conker: Live & Reloaded)."""
import pytest
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

def translate(hexcode):
    image=bytes.fromhex(hexcode)
    base=0x10000
    config._install([config.Section('.text',base,len(image),0,len(image),True)],
                    entry_point=base,kernel_thunk_addr=base,origin='sub-width-test')
    db={base:{'start':hex(base),'end':base+len(image),'_addr':base,'size':len(image)}}
    return FunctionTranslator(image,db).translate_function(base,db[base])

# sub <op>; j<cc> +6; mov eax,0; ret; mov eax,1; ret
TAIL='b800000000c3b801000000c3'

@pytest.mark.parametrize('sub,width',[('28cd','uint8_t'),     # sub ch, cl
                                      ('6629c8','uint16_t'),  # sub ax, cx
                                      ('29c8','uint32_t')])   # sub eax, ecx
@pytest.mark.parametrize('jcc',['7c06','7d06','7e06','7f06'])  # jl, jge, jle, jg
def test_sub_condition_uses_operand_width(sub,width,jcc):
    code=translate(sub+jcc+TAIL)
    cond=code[code.index('if ('):].split('\n')[0]
    assert '(uint32_t)_fa + (uint32_t)_fb' not in cond
    assert f'({width})(_fa + _fb)' in cond

def test_lahf_after_dec_declares_cf():
    # dec eax; lahf; ret -- dec keeps CF, lahf packs it into AH
    code=translate('489fc3')
    assert 'int _cf = 0;' in code
