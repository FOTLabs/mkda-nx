#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
ptrdiff_t g_xbox_mem_offset;

RECOMP_TLS uint32_t g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp, g_ebp, g_seh_ebp;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top, g_fp_cmp;
RECOMP_TLS int g_df;
RECOMP_TLS uint16_t g_fp_control_word, g_fp_cc;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;

volatile int g_gil_contended;
void recomp_preempt(void) {}
extern void sub_00207C4B_gen(void);
static void call_blit(void (*fn)(void), unsigned w, unsigned h, unsigned pitch, unsigned dest) {
    g_esp=0x1000; g_ebp=0x888; g_ebx=7; g_esi=8; g_edi=9;
    unsigned args[]={0xDEADBEEF,w,h,0x10000,0x100000,0x180000,dest,pitch};
    memcpy((void *)XBOX_PTR(g_esp),args,sizeof(args));
    fn(); assert(g_esp==0x1020 && g_ebx==7 && g_esi==8 && g_edi==9);
}
int main(void) {
    g_xbox_mem_offset=(ptrdiff_t)malloc(0x800000); assert(g_xbox_mem_offset);
    unsigned seed=97;
    for(unsigned i=0;i<0x200000;i++) { seed=seed*1664525u+1013904223u; MEM8(i)=seed>>24; }
    const unsigned dims[][2]={{0,0},{1,0},{0,1},{1,1},{2,3},{45,30},{46,31}};
    for(unsigned n=0;n<sizeof(dims)/sizeof(dims[0]);n++)
      for(unsigned pad=0;pad<=64;pad+=32) {
        unsigned w=dims[n][0],h=dims[n][1],pitch=w*32+pad;
        memset((void *)XBOX_PTR(0x200000),0xCD,0x100000);
        memset((void *)XBOX_PTR(0x400000),0xCD,0x100000);
        call_blit(sub_00207C4B_gen,w,h,pitch,0x200003);
        call_blit(sub_00207C4B,w,h,pitch,0x400003);
        if(w && h) assert(MEM8(0x400003)==MEM8(0x10000));
        assert(memcmp((void *)XBOX_PTR(0x200000),(void *)XBOX_PTR(0x400000),0x100000)==0);
      }
    for(unsigned mode=0;mode<2;mode++) {
      clock_t start=clock();
      for(unsigned i=0;i<60;i++)call_blit(mode?sub_00207C4B:sub_00207C4B_gen,45,30,1440,0x200000);
      printf("%s blit: %.6f ms/frame\n",mode?"native":"lifted",(clock()-start)*1000.0/CLOCKS_PER_SEC/60);
    }
    free((void *)g_xbox_mem_offset);
}
