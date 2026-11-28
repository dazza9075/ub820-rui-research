/* runlow [-c CPUMASK] [-i] [-n NICE] cmd args... : set CPU affinity / SCHED_IDLE / nice, then exec (children inherit) */
#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
int main(int argc, char** argv){
    int i = 1; unsigned long mask = 0; int idle = 0, nice_v = 999;
    while(i < argc && argv[i][0] == '-'){
        if(!strcmp(argv[i], "-c") && i + 1 < argc){ mask = strtoul(argv[i+1], 0, 0); i += 2; }
        else if(!strcmp(argv[i], "-i")){ idle = 1; i++; }
        else if(!strcmp(argv[i], "-n") && i + 1 < argc){ nice_v = atoi(argv[i+1]); i += 2; }
        else if(!strcmp(argv[i], "--")){ i++; break; }
        else break;
    }
    if(i >= argc){ static const char u[] = "usage: runlow [-c CPUMASK] [-i] [-n NICE] cmd args...\n"; write(2, u, sizeof u - 1); return 2; }
    if(mask){ cpu_set_t s; CPU_ZERO(&s); for(int c = 0; c < 32; c++) if(mask & (1UL << c)) CPU_SET(c, &s); sched_setaffinity(0, sizeof s, &s); }
    if(nice_v != 999) setpriority(PRIO_PROCESS, 0, nice_v);
    if(idle){ struct sched_param p = { 0 }; sched_setscheduler(0, SCHED_IDLE, &p); }
    execvp(argv[i], argv + i);
    static const char e[] = "runlow: exec failed\n"; write(2, e, sizeof e - 1); return 127;
}
