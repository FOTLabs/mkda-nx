"""Exercise the actual POSIX mkdir_p against a filesystem, counting calls."""
from pathlib import Path
import re, subprocess, tempfile
source=(Path(__file__).resolve().parents[1]/"src/kernel/kernel_path.c").read_text()
function=re.search(r"^static void mkdir_p\([^\n]*\)\n\{.*?^\}",source,re.M|re.S).group()
harness=r"""#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define MAX_PATH 4096
#define XBOX_LOG_WARN 0
#define XBOX_LOG_PATH 0
#define xbox_log(...) ((void)0)
static unsigned calls;
static int counted_mkdir(const char *path,mode_t mode){calls++;return mkdir(path,mode);}
#define mkdir counted_mkdir
"""+function+r"""
#undef mkdir
int main(int argc,char **argv){
 assert(argc==2);char path[MAX_PATH],parent[MAX_PATH];struct stat st;
 snprintf(parent,sizeof parent,"%s/new",argv[1]);snprintf(path,sizeof path,"%s/new/child",argv[1]);
 mkdir_p(path);assert(!stat(path,&st)&&S_ISDIR(st.st_mode));
 calls=0;for(int i=0;i<100;i++)mkdir_p(path);assert(calls==100);
 assert(!rmdir(path));calls=0;mkdir_p(path);assert(calls==1&&!stat(path,&st));
 assert(!rmdir(path)&&!rmdir(parent));mkdir_p(path);assert(!stat(path,&st));
 puts("PASS: one mkdir per existing path; missing roots/parents recreated correctly");
 return 0;
}
"""
with tempfile.TemporaryDirectory() as root:
 executable=root+"/test"
 subprocess.run(["cc","-x","c","-O2","-o",executable,"-"],input=harness,text=True,check=True)
 subprocess.run([executable,root],check=True)
