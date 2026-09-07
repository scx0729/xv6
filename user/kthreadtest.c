#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

static volatile int stage;

static void
worker(void *arg)
{
  volatile int *shared_stage = (volatile int *)arg;

  *shared_stage = 1;
  sleep(10);
  *shared_stage = 2;
  kthread_exit(37);
}

int
main(void)
{
  char *stack;
  uint64 stack_top;
  int tid;
  int status = -1;

  stack = malloc(PGSIZE + 16);
  if(stack == 0){
    printf("kthreadtest: stack allocation failed\n");
    exit(1);
  }

  // Keep the supplied stack top 16-byte aligned for the RISC-V ABI.
  stack_top = ((uint64)stack + PGSIZE + 16) & ~((uint64)0xf);
  tid = kthread_create(worker, (void *)&stage, (void *)stack_top);
  if(tid < 0){
    printf("kthreadtest: kthread_create failed\n");
    free(stack);
    exit(1);
  }

  if(kthread_join(tid, &status) != tid){
    printf("kthreadtest: kthread_join failed\n");
    free(stack);
    exit(1);
  }

  if(stage != 2 || status != 37){
    printf("kthreadtest: bad result stage=%d status=%d\n", stage, status);
    free(stack);
    exit(1);
  }

  free(stack);
  printf("kthreadtest: OK\n");
  exit(0);
}
