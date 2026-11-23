#include <sys/mount.h>
#include <stdio.h>
int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr, "usage: %s <src> <dst>\n", argv[0]); return 2; }
    if (mount(argv[1], argv[2], "", MS_BIND, "")){ perror("mount MS_BIND"); return 1; }
    return 0;
}
