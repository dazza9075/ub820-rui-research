/* semfix <semid> <semnum> <value> : SETVAL on a SysV semaphore whose holder died (recovery tool). Prints before/after. */
#include <sys/sem.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
union semun { int val; struct semid_ds* buf; unsigned short* array; };
static void w(const char* s){ write(2, s, strlen(s)); }
int main(int c, char** v){
    if(c < 4){ w("usage: semfix <semid> <semnum> <value>\n"); return 1; }
    int id = atoi(v[1]), n = atoi(v[2]); union semun u; u.val = atoi(v[3]); char b[96];
    int before = semctl(id, n, GETVAL), pid = semctl(id, n, GETPID), nc = semctl(id, n, GETNCNT);
    snprintf(b, sizeof b, "before: val=%d lastpid=%d waiters=%d\n", before, pid, nc); w(b);
    if(semctl(id, n, SETVAL, u) < 0){ w("SETVAL failed\n"); return 2; }
    snprintf(b, sizeof b, "after: val=%d\n", semctl(id, n, GETVAL)); w(b);
    return 0;
}
