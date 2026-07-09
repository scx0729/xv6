#include "kernel/types.h"
#include "user/user.h"
#include "kernel/param.h"

int runline(int argc, char *argv[], char *line){
    char *args[MAXARG];
    int n = 0;
    int added = 0;
    for(int i = 1;i < argc;i++){
        if(n >= MAXARG - 1){
            fprintf(2, "xargs: too many arguments\n");
            return -1;
        }
        args[n++] = argv[i];
    }
    char *p = line;
    while(*p){
        while(*p == ' ') p++;
        if(*p == 0) break;
        if(n >= MAXARG - 1){
            fprintf(2, "xargs: too many arguments\n");
            return -1;
        }
        args[n++] = p;
        added++;
        while(*p && *p != ' ') p++;
        if(*p) *p++ = 0;
    }
    if(added == 0) return 0;
    args[n] = 0;
    int pid = fork();
    if(pid < 0){
        fprintf(2, "xargs: fork failed\n");
        return -1;
    }
    if(pid == 0){
        exec(argv[1], args);
        // 终止子进程
        fprintf(2, "xargs: exec %s failed\n", args[0]);
        exit(1);
    }
    wait(0);
    return 0;
}

int
main(int argc, char *argv[]){
    char line[512];
    char ch;
    int n = 0;
    int count;
    if(argc < 2){
        fprintf(2, "usage: xargs command [arguments ...]\n");
        exit(1);
    }
    while((count = read(0, &ch, sizeof(ch))) > 0){
        if(ch == '\n'){
            line[n] = 0;
            if(runline(argc, argv, line) < 0){
                exit(1);
            }
            n = 0;
        } else {
            if(n >= 512 - 1){
                fprintf(2, "xargs: input line too long\n");
                exit(1);
            }
            line[n++] = ch;
        }
    }
    if(count < 0){
        fprintf(2, "xargs: read failed\n");
        exit(1);
    }
    // 处理最后一行没有换行符的情况
    if(n > 0){
        line[n] = 0;
        if(runline(argc, argv, line) < 0){
            exit(1);
        }
    }
    exit(0);
}