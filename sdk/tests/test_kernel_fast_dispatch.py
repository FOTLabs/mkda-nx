"""Test actual dispatcher handover decisions for fast and blocking calls."""
from pathlib import Path
import re,subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/"src/kernel/kernel_bridge.c").read_text()
def fn(name):return re.search(r"^static (?:int|void) "+name+r"\([^\n]*\)\n\{.*?^\}",s,re.M|re.S).group()
h=r'''#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define ULONG unsigned
#define XBOX_KERNEL_THUNK_TABLE_SIZE 16
static unsigned g_slot_ordinals[16];static int g_kernel_dispatch_slot,depth,suspend_n,resume_n,body_n,flush_n;
static int kernel_call_at_dispatch_keeps_gil(int s){(void)s;return 0;}
static void kernel_thunk_dispatch_body(void){body_n++;}
static int xbox_gil_suspend(void){suspend_n++;return 7;}
static void xbox_gil_resume(int d){assert(d==7);resume_n++;}
static int GetCurrentThreadId(void){return 1;}
static void pending_start_flush(int d){assert(d==1);flush_n++;}
static void xbox_kernel_busy(int n){depth+=n;}
'''+fn("kernel_call_keeps_gil")+"\n"+fn("kernel_thunk_dispatch")+r'''
int main(int argc,char**argv){
 int off=argc>1;if(off)setenv("RECOMP_KERNEL_FAST","0",1);else unsetenv("RECOMP_KERNEL_FAST");
 unsigned ords[]={181,294,126,277,278,295,219,190,158,99};
 for(int i=0;i<10;i++){
  g_kernel_dispatch_slot=i;g_slot_ordinals[i]=ords[i];suspend_n=resume_n=body_n=flush_n=0;
  kernel_thunk_dispatch();assert(body_n==1 && !depth);
  int expect=off || i>=6;assert(suspend_n==expect && resume_n==expect && flush_n==expect);
 }
 puts("PASS: bounded query/CS fast entry keeps GIL; waits, reads and creates hand over; opt-out preserved");
}
'''
with tempfile.TemporaryDirectory() as d:
 exe=d+"/test";subprocess.run(["cc","-x","c","-O2","-o",exe,"-"],input=h,text=True,check=True);subprocess.run([exe],check=True);subprocess.run([exe,"off"],check=True)
