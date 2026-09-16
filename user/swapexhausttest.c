#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "kernel/param.h"
#include "user/user.h"

#define TEST_PAGES 34000
#define CHECK_FROM_PAGE 32000

int
main(int argc, char **argv)
{
  int ready[2];
  int status;
  int initial_slots = swapstat(1);
  if(initial_slots < 0 || pipe(ready) < 0){
    printf("swapexhausttest: setup failed\n");
    exit(1);
  }

  int pid = fork();
  if(pid < 0){
    printf("swapexhausttest: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    close(ready[0]);
    volatile char *mem = sbrk(TEST_PAGES * PGSIZE);
    if((char *)mem == (char *)-1)
      exit(2);

    int reported_full = 0;
    for(int i = 0; i < TEST_PAGES; i++){
      mem[(uint64)i * PGSIZE] = (char)i;
      if(i >= CHECK_FROM_PAGE && !reported_full && swapstat(1) == NSWAP){
        char full = 'F';
        if(write(ready[1], &full, 1) != 1)
          exit(3);
        reported_full = 1;
      }
    }

    // Reaching here means exhausting the swap area did not stop allocation.
    exit(4);
  }

  close(ready[1]);
  char full = 0;
  int notified = read(ready[0], &full, 1) == 1 && full == 'F';
  close(ready[0]);
  int waited = wait(&status);
  int final_slots = swapstat(1);

  if(!notified || waited != pid || status != -1 || final_slots != initial_slots){
    printf("swapexhausttest: FAIL full=%d status=%d slots=%d->%d\n",
           notified, status, initial_slots, final_slots);
    exit(1);
  }
  printf("swapexhausttest: OK\n");
  exit(0);
}
