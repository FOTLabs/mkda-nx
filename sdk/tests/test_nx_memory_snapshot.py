"""Compile the actual Switch memory-query implementation against a fake clock/SVC."""
from pathlib import Path
import re,subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/"src/platform/win32_compat.c").read_text()
w=re.search(r"^BOOL GlobalMemoryStatusEx\(LPMEMORYSTATUSEX b\)\n\{.*?^\}",s,re.M|re.S).group()
h=r'''#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define __SWITCH__ 1
#define BOOL int
#define FALSE 0
#define TRUE 1
#define DWORD uint32_t
#define R_FAILED(x) ((x)!=0)
#define InfoType_TotalMemorySize 1
#define InfoType_UsedMemorySize 2
#define CUR_PROCESS_HANDLE 0
#define u64 uint64_t
static u64 clock_tick=1,used=200,total=1000;static int calls,fail;
static u64 armGetSystemTick(void){return clock_tick;}
static u64 armGetSystemTickFreq(void){return 60000;}
static int svcGetInfo(u64*out,int type,int h,int x){(void)h;(void)x;calls++;if(fail)return 1;*out=type==1?total:used;return 0;}
typedef struct {u64 ullTotalPhys,ullAvailPhys,ullTotalPageFile,ullAvailPageFile,ullTotalVirtual,ullAvailVirtual,ullAvailExtendedVirtual;DWORD dwMemoryLoad;} MEMORYSTATUSEX,*LPMEMORYSTATUSEX;
'''+w+r'''
int main(void){
 MEMORYSTATUSEX b;assert(!GlobalMemoryStatusEx(NULL) && !calls);
 assert(GlobalMemoryStatusEx(&b) && calls==2 && b.ullAvailPhys==800 && b.dwMemoryLoad==20);
 used=600;for(int i=0;i<10000;i++)assert(GlobalMemoryStatusEx(&b) && b.ullAvailPhys==800);
 assert(calls==2);clock_tick=1001;assert(GlobalMemoryStatusEx(&b) && calls==4 && b.ullAvailPhys==400);
 clock_tick+=1000;used=2000;assert(GlobalMemoryStatusEx(&b) && b.ullAvailPhys==0);
 clock_tick+=1000;fail=1;assert(!GlobalMemoryStatusEx(&b));fail=0;
 assert(GlobalMemoryStatusEx(&b) && b.ullAvailPhys==0);
 puts("PASS: 10000 memory polls share one frame snapshot; expiry, pressure, null and failed SVC paths");
}
'''
with tempfile.TemporaryDirectory() as d:
 exe=d+"/test";subprocess.run(["cc","-x","c","-O2","-o",exe,"-"],input=h,text=True,check=True);subprocess.run([exe],check=True)
