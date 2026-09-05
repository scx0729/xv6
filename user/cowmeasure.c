#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/cow.h"

#define PGSIZE 4096
// 构造一个可重复的 COW 工作负载：先准备 N 个堆页，fork 后只让
// 子进程修改其中 M 页。统计值覆盖进程的全部有效用户页，不只这 N 页。
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

  // sbrk() 只扩大虚拟地址空间。逐页写入一次以触发 Lazy Allocation，
  // 确保这 N 页在 fork 时已经映射，否则 uvmcopy() 会跳过尚未分配的页。
  for(int i = 0; i < N; i++){
    p[i * PGSIZE] = 1;
  }

  // 清除以前的统计并从本次 fork 前开始测量。uvmcopy() 会把普通
  // fork 原本需要复制的全部有效用户页累计到 base_pages。
  cowreset();

  pid = fork();

  if(pid < 0){
    printf("cowmeasure: FAIL\n");
    exit(1);
  }

  if(pid == 0){
    // 子进程只修改目标堆区的前 M 页。每次首次写入共享页都会触发
    // COW；如果引用数大于 1，cowalloc() 才真正复制一个物理页。
    // 进程运行时也可能写栈，因此总复制数不保证恰好等于 M。
    for(int i = 0; i < M; i++){
      p[i * PGSIZE] = 2;
    }

    // 子进程必须看到自己对前 M 页的修改。
    for(int i = 0; i < M; i++){
      if(p[i * PGSIZE] != 2){
        printf("cowmeasure: FAIL\n");
        exit(1);
      }
    }

    // 子进程没有修改的页面仍应保持 fork 时的内容。
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

  // COW 必须隔离父子地址空间：子进程的写入不能改变父进程看到的内容。
  for(int i = 0; i < N; i++){
    if(p[i * PGSIZE] != 1){
      printf("cowmeasure: FAIL\n");
      exit(1);
    }
  }

  // base 表示普通 fork 会复制的页数，copied 表示本次 COW 工作负载
  // 实际执行整页复制的次数；二者之差就是 COW 减少的页面复制次数。
  uint64 base = cowstat(COWSTAT_BASE_PAGES);
  uint64 copied = cowstat(COWSTAT_COPIED_PAGES);

  if(base == (uint64)-1 ||
     copied == (uint64)-1 ||
     copied > base){
    printf("cowmeasure: invalid statistics\n");
    exit(1);
  }

  // 每次页面复制固定移动 PGSIZE 字节，因此页面差乘 PGSIZE 即为
  // COW 减少的复制字节数。
  uint64 reduced = base - copied;

  printf("normal fork copied pages: %d\n", (int)base);
  printf("cow copied pages: %d\n", (int)copied);
  printf("reduced copied pages: %d\n", (int)reduced);

  printf("normal fork copied bytes: %d\n",
         (int)(base * PGSIZE));
  printf("cow copied bytes: %d\n",
         (int)(copied * PGSIZE));
  printf("reduced copied bytes: %d\n",
         (int)(reduced * PGSIZE));

  printf("cowmeasure: OK\n");

  exit(0);
}
