#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/cow.h"
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

  /*
   * 从 fork 前开始统计：base_pages 记录普通 fork 原本需要复制的
   * 全部有效用户页，cow_pages 记录本次 COW 工作负载真正复制的页。
   */
  cowreset();

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
   * 子进程退出后，父进程的这些页仍带有 PTE_COW，但物理页引用数
   * 已降为 1。记录当前复制次数，再写这 N 页；ref==1 优化正确时，
   * cowalloc() 只恢复 PTE_W，不会分配和复制新页。
   */
  int copied_before = cowstat(COWSTAT_COPIED_PAGES);
  if(copied_before < 0){
    printf("cowopt: cannot read COW statistics\n");
    exit(1);
  }

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

  int copied_after = cowstat(COWSTAT_COPIED_PAGES);
  int base_pages = cowstat(COWSTAT_BASE_PAGES);
  if(copied_after < 0 || base_pages < 0 || copied_after > base_pages){
    printf("cowopt: invalid COW statistics\n");
    exit(1);
  }

  int extra_copies = copied_after - copied_before;
  if(extra_copies != 0){
    printf("cowopt: ref==1 writes copied %d extra pages\n", extra_copies);
    exit(1);
  }

  printf("normal fork copied pages: %d\n", base_pages);
  printf("cow copied pages: %d\n", copied_after);
  printf("reduced copied pages: %d\n", base_pages - copied_after);
  printf("ref==1 optimization avoided page copies: %d\n", N);

  printf("cowopt: OK\n");

  exit(0);
}
