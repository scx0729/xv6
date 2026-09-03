#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define N 20

int
main(void)
{
  char *p;
  int pid;

  p = sbrk(N * PGSIZE);
  if(p == (char *)-1){
    printf("cowopt: FAIL\n");
    exit(1);
  }

  // 让 N 个 Lazy 页真正分配出来
  for(int i = 0; i < N; i++){
    p[i * PGSIZE] = 1;
  }

  pid = fork();

  if(pid < 0){
    printf("cowopt: FAIL\n");
    exit(1);
  }

  if(pid == 0){
    // 子进程不修改这些页面，直接退出
    exit(0);
  }

  if(wait(0) < 0){
    printf("cowopt: FAIL\n");
    exit(1);
  }

  /*
   * 子进程已经退出。
   * 这些物理页理论上：
   *
   * PTE_COW = 1
   * PTE_W   = 0
   * refcount = 1
   *
   * 从这里开始单独测 ref==1 优化。
   */
  cowreset();

  for(int i = 0; i < N; i++){
    p[i * PGSIZE] = 2;
  }

  // 检查父进程写入是否成功
  for(int i = 0; i < N; i++){
    if(p[i * PGSIZE] != 2){
      printf("cowopt: FAIL\n");
      exit(1);
    }
  }

  cowstat();

  printf("cowopt: OK\n");

  exit(0);
}