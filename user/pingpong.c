#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[]){
    int p1[2];
    int p2[2];
    pipe(p1);
    pipe(p2);
    if(fork() == 0){
        char buf[1];
        close(p1[0]);
        close(p2[1]);
        read(p2[0], buf, 1);
        fprintf(1, "%d: received ping\n", getpid());
        write(p1[1], buf, 1);
        exit(0);
    } else {
        char buf[1];
        close(p1[1]);
        close(p2[0]);
        write(p2[1], "*", 1);
        read(p1[0], buf, 1);
        fprintf(1, "%d: received pong\n", getpid());
        // 不是父进程的 exit() 回收子进程，而是父进程在 exit() 之前调用的 wait() 回收子进程。
        // 父进程自己的 exit() 不负责清理已退出的 child；
        // 如果父进程不 wait() 就退出，xv6 会把 child 交给init，之后由 init 来 wait() 回收。
        wait(0);
        exit(0);
    }
}