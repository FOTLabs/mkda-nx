/* gcc -O2 -ffunction-sections -fdata-sections -I src -I include
 * tests/test_read_ahead_media.c -Wl,--gc-sections,--wrap=read -pthread -lm -o /tmp/test_read_ahead_media */
#include "../src/kernel/kernel_file.c"
#include <assert.h>
static const char *test_path = "sdmc:/switch/buffyx/save/Cache//sound/music/test.wma";
static int test_flags = O_RDONLY, read_calls;
static uint32_t generation = 1;
const char *w32_handle_path(HANDLE h) { (void)h; return test_path; }
int w32_handle_open_flags(HANDLE h) { (void)h; return test_flags; }
uint32_t w32_handle_file_generation(HANDLE h) { (void)h; return generation; }
int xbox_path_in_game_dir(const char *p) { return !strncmp(p, "/disc/", 6); }
ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __wrap_read(int fd, void *buf, size_t n) { ++read_calls; return __real_read(fd, buf, n); }
int main(void) {
    char file[] = "/tmp/buffy-ra-test-XXXXXX";
    int fd = mkstemp(file); assert(fd >= 0); unlink(file);
    unsigned char source[100000], out[512];
    for (unsigned i=0;i<sizeof source;i++) source[i]=(unsigned char)(i*17+7);
    assert(write(fd,source,sizeof source)==sizeof source);
    setenv("RECOMP_READAHEAD_MEDIA","1",1);
    assert(ra_media_path(test_path));
    assert(ra_media_path("/save/cache/interfc/Logo.WMV"));
    assert(ra_media_path("sdmc:/switch/buffyx/save/Cache//interfc/attract.wmv"));
    assert(ra_media_path("sdmc:/switch/buffyx/save/Cache///sound//music/intro.wma"));
    assert(!ra_media_path("/save/Cache//sound/music/intro.wma_"));
    assert(!ra_media_path("/save/Cache/sound/music/test.wma_"));
    assert(!ra_media_path("/save/UDATA/test.wma"));
    int k=ra_slot((HANDLE)1,fd);assert(k>=0);
    for(unsigned pos=0;pos<sizeof source;pos+=sizeof out) {
        unsigned n=sizeof source-pos<sizeof out?sizeof source-pos:sizeof out;
        assert(ra_read(k,fd,out,sizeof out,pos)==n);
        assert(!memcmp(out,source+pos,n));
        assert(lseek(fd,0,SEEK_CUR)==pos+n);
    }
    assert(read_calls<=6 && s_rd_hits>180);
    /* Decoder-style repeated/non-adjacent sectors must hit the first media
     * window, even when no pair is sequential. This was 0 hits on hardware. */
    int before_calls=read_calls;
    for(int i=0;i<100;i++) {
        unsigned pos=(i%2)?4096:1024;
        assert(ra_read(k,fd,out,sizeof out,pos)==sizeof out);
        assert(!memcmp(out,source+pos,sizeof out));
    }
    assert(read_calls-before_calls<=1); /* one refill after the preceding short EOF read */

    assert(ra_read(k,fd,out,sizeof out,sizeof source)==0);
    assert(lseek(fd,0,SEEK_CUR)==sizeof source);
    assert(ra_read(k,fd,out,sizeof out,1024)==sizeof out);
    assert(!memcmp(out,source+1024,sizeof out));
    assert(lseek(fd,0,SEEK_CUR)==1536);
    /* A close/reopen may reuse BOTH the descriptor and path. Its generation
     * must invalidate a buffered window before delivering new bytes. */
    ++generation;memset(source,0xA5,sizeof source);
    assert(pwrite(fd,source,sizeof source,0)==sizeof source);
    k=ra_slot((HANDLE)1,fd);assert(k>=0 && s_ra[k].len==0);
    assert(ra_read(k,fd,out,sizeof out,1024)==sizeof out);
    assert(!memcmp(out,source+1024,sizeof out));
    test_flags=O_RDWR;assert(ra_slot((HANDLE)1,fd)==-1);
    test_flags=O_WRONLY;assert(ra_slot((HANDLE)1,fd)==-1);
    test_flags=O_RDONLY;setenv("RECOMP_READAHEAD_MEDIA","0",1);assert(ra_slot((HANDLE)1,fd)==-1);
    test_path="/disc/archive.rpe";assert(ra_slot((HANDLE)1,fd)>=0);
    k=ra_slot((HANDLE)1,fd);before_calls=read_calls;
    unsigned char packed[32768];
    for(unsigned pos=0;pos<sizeof source;pos+=sizeof packed) {
        unsigned n=sizeof source-pos<sizeof packed?sizeof source-pos:sizeof packed;
        assert(ra_read(k,fd,packed,sizeof packed,pos)==n);
        assert(!memcmp(packed,source+pos,n));
        assert(lseek(fd,0,SEEK_CUR)==pos+n);
    }
    assert(read_calls-before_calls<=3); /* final partial read preserves normal EOF semantics */

    close(fd);
    printf("PASS: sequential sectors, repeated media sectors and 32KB assets batched; random/short/EOF bytes and cursors preserved; generation invalidation and writable/save/temp-file exclusion (%d host reads)\n",read_calls);
}
