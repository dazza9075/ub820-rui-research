#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
static void we(const char*s){ write(2,s,strlen(s)); }
int main(int argc,char**argv){
    unsigned long long off = (argc>1)? strtoull(argv[1],0,0) : 0xE1010000ULL;
    unsigned long len = (argc>2)? strtoul(argv[2],0,0) : 0x7E9000UL;
    int fd=open("/dev/exmem",O_RDONLY|O_SYNC);
    if(fd<0){ we("open /dev/exmem fail\n"); return 1; }
    void* m=mmap64(NULL,len,PROT_READ,MAP_SHARED,fd,(off64_t)off);
    if(m==MAP_FAILED){ we("mmap fail\n"); return 2; }
    unsigned long done=0; const char* p=(const char*)m;
    while(done<len){ ssize_t n=write(1,p+done,len-done); if(n<=0) break; done+=(unsigned long)n; }
    munmap(m,len); close(fd); return 0;
}
