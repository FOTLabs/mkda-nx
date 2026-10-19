"""Initialization must stop at the real XBE import count, not resolve padding."""
from pathlib import Path
import re,subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/"src/kernel/kernel_thunks.c").read_text()
w=re.search(r"^void xbox_kernel_init\(void\)\n\{.*?^\}",s,re.M|re.S).group()
h=r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#define ULONG uint32_t
#define ULONG_PTR uintptr_t
#define XBOX_KERNEL_THUNK_TABLE_SIZE 8
#define XBOX_LOG_INFO 1
#define XBOX_LOG_WARN 1
#define XBOX_LOG_ERROR 0
#define XBOX_LOG_TRACE 3
#define XBOX_LOG_THUNK "THUNK"
#define TRUE 1
static int g_log_cs,g_log_cs_init,g_log_level;
static FILE *g_log_file;
static struct {int Major,Minor,Build,Qfe;} xbox_KrnlVersion;
static ULONG_PTR xbox_kernel_thunk_table[8];
static ULONG g_thunk_ordinals[8]={1,2,3,0,0,0,0,0};
static uint32_t mem[16],base,count,resolves,unresolved;
static uintptr_t g_xbox_mem_offset;
static void InitializeCriticalSection(int *p){(void)p;}
#define xbox_log(...) ((void)0)
static void xbox_kernel_get_thunk_address(uint32_t*a,uint32_t*b){*a=base;*b=count;}
static void *xbox_GetMemoryBase(void){return mem;}
static void xbox_unresolved_thunk(void){}
static uintptr_t xbox_resolve_ordinal(uint32_t n){resolves++;if(!n || n>3)unresolved++;return n+100;}
'''+w+r'''
int main(void){
 setenv("RECOMP_NO_LOG","1",1);g_xbox_mem_offset=(uintptr_t)mem;
 base=16;count=3;mem[4]=0x80000001;mem[5]=0x80000002;mem[6]=0x80000003;
 mem[7]=0x80000077; /* nonzero data after declared table must not be read */
 xbox_kernel_init();assert(resolves==3 && !unresolved && !xbox_kernel_thunk_table[3]);
 resolves=0;mem[5]=0;xbox_kernel_init();assert(resolves==1);
 resolves=0;base=count=0;xbox_kernel_init();assert(resolves==3 && !unresolved);
 puts("PASS: exact mapped thunk count, early terminator and unused reference-slot exclusion");
}
'''
with tempfile.TemporaryDirectory() as d:
 exe=d+"/test";subprocess.run(["cc","-x","c","-O2","-o",exe,"-"],input=h,text=True,check=True);subprocess.run([exe],check=True)
