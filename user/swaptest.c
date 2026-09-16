#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

// Data pages plus their page-table pages slightly exceed xv6's free RAM.
#define TEST_PAGES 32400

static uchar
pattern(int page)
{
  return (uchar)((page * 37 + 11) & 0xff);
}

int
main(int argc, char **argv)
{
  uint64 bytes = (uint64)TEST_PAGES * PGSIZE;
  volatile char *mem = sbrk((int)bytes);

  if((char *)mem == (char *)-1){
    printf("swaptest: sbrk failed\n");
    exit(1);
  }

  printf("swaptest: writing %d pages\n", TEST_PAGES);
  for(int i = 0; i < TEST_PAGES; i++){
    mem[(uint64)i * PGSIZE] = pattern(i);
    mem[(uint64)i * PGSIZE + PGSIZE - 1] = pattern(i) ^ 0x5a;
    if(i != 0 && i % 4096 == 0)
      printf("swaptest: wrote %d pages\n", i);
  }

  printf("swaptest: reading pages in reverse order\n");
  for(int i = TEST_PAGES - 1; i >= 0; i--){
    uchar first = (uchar)mem[(uint64)i * PGSIZE];
    uchar last = (uchar)mem[(uint64)i * PGSIZE + PGSIZE - 1];
    if(first != pattern(i) || last != (uchar)(pattern(i) ^ 0x5a)){
      printf("swaptest: mismatch on page %d, first=%x last=%x\n",
             i, first, last);
      exit(1);
    }
  }

  if(sbrk(-(int)bytes) == (char *)-1){
    printf("swaptest: shrink failed\n");
    exit(1);
  }

  printf("swaptest: OK\n");
  exit(0);
}
