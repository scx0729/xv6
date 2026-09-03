#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define N 20
#define M 5

int
main(void)
{
  char *p;
  int pid;

  p = sbrk(N * PGSIZE);
  if(p == (char *)-1){
    printf("cowmeasure: FAIL\n");
    exit(1);
  }

  // 触发 Lazy Allocation，让这 N 页真正存在
  for(int i = 0; i < N; i++){
    p[i * PGSIZE] = 1;
  }

  cowreset();

  pid = fork();

  if(pid < 0){
    printf("cowmeasure: FAIL\n");
    exit(1);
  }

  if(pid == 0){
    // 子进程只修改前 M 页
    for(int i = 0; i < M; i++){
      p[i * PGSIZE] = 2;
    }

    // 检查修改的 M 页
    for(int i = 0; i < M; i++){
      if(p[i * PGSIZE] != 2){
        printf("cowmeasure: FAIL\n");
        exit(1);
      }
    }

    // 没修改的页应该仍然是 1
    for(int i = M; i < N; i++){
      if(p[i * PGSIZE] != 1){
        printf("cowmeasure: FAIL\n");
        exit(1);
      }
    }

    exit(0);
  }

  if(wait(0) < 0){
    printf("cowmeasure: FAIL\n");
    exit(1);
  }

  // 最关键：
  // 子进程修改页面后，父进程的数据不能被修改
  for(int i = 0; i < N; i++){
    if(p[i * PGSIZE] != 1){
      printf("cowmeasure: FAIL\n");
      exit(1);
    }
  }

  cowstat();

  printf("cowmeasure: OK\n");

  exit(0);
}