#include "kernel/types.h"
#include "user/user.h"

void sieve(int left_fd){
    int prime;
    if(read(left_fd, &prime, sizeof(prime)) == 0){
        close(left_fd);
        exit(0);
    }
    int p[2];
    pipe(p);
    if(fork() > 0){
        close(p[0]);
        fprintf(1, "prime %d\n", prime);
        int i;
        while(read(left_fd, &i, sizeof(i))){
            if(i % prime != 0){
                write(p[1], &i, sizeof(i));
            }
        }
        close(left_fd);
        close(p[1]);
        wait(0);
        exit(0);
    } else {
        close(p[1]);
        close(left_fd);
        sieve(p[0]);
    }
}

int
main(int argc, char *argv[]){
    int p[2];
    pipe(p);
    if(fork() > 0){
        close(p[0]);
        for(int i = 2;i <= 35;i++){
            write(p[1], &i, sizeof(i));
        }
        close(p[1]);
        wait(0);
        exit(0);
    } else {
        close(p[1]);
        sieve(p[0]);
        exit(0);
    }
}