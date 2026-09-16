#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define PARENT_PAGES 32
#define PRESSURE_PAGES 32400
#define RESERVE_PAGES 256

static uchar
parent_pattern(int page)
{
  return (uchar)((page * 29 + 7) & 0xff);
}

static uchar
pressure_pattern(int page)
{
  return (uchar)((page * 43 + 19) & 0xff);
}

static int
check_parent(volatile char *mem)
{
  for(int i = 0; i < PARENT_PAGES; i++){
    uchar first = (uchar)mem[(uint64)i * PGSIZE];
    uchar last = (uchar)mem[(uint64)i * PGSIZE + PGSIZE - 1];
    if(first != parent_pattern(i) ||
       last != (uchar)(parent_pattern(i) ^ 0x5a)){
      printf("swapforktest: parent page %d mismatch\n", i);
      return -1;
    }
  }
  return 0;
}

static void
pressure_worker(int ready_fd, int go_fd)
{
  uint64 bytes = (uint64)PRESSURE_PAGES * PGSIZE;
  volatile char *mem = sbrk((int)bytes);
  char signal;

  if((char *)mem == (char *)-1){
    signal = 'E';
    write(ready_fd, &signal, 1);
    exit(1);
  }

  for(int i = 0; i < PRESSURE_PAGES; i++){
    mem[(uint64)i * PGSIZE] = pressure_pattern(i);
    mem[(uint64)i * PGSIZE + PGSIZE - 1] = pressure_pattern(i) ^ 0xa5;
  }

  // Leave enough free pages for allocproc() and the child's page tables.
  if(sbrk(-(RESERVE_PAGES * PGSIZE)) == (char *)-1){
    signal = 'E';
    write(ready_fd, &signal, 1);
    exit(1);
  }

  signal = 'R';
  if(write(ready_fd, &signal, 1) != 1 || read(go_fd, &signal, 1) != 1)
    exit(1);

  int active_pages = PRESSURE_PAGES - RESERVE_PAGES;
  for(int i = 0; i < active_pages; i += 97){
    uchar first = (uchar)mem[(uint64)i * PGSIZE];
    uchar last = (uchar)mem[(uint64)i * PGSIZE + PGSIZE - 1];
    if(first != pressure_pattern(i) ||
       last != (uchar)(pressure_pattern(i) ^ 0xa5)){
      printf("swapforktest: pressure page %d mismatch\n", i);
      exit(1);
    }
  }

  exit(0);
}

int
main(int argc, char **argv)
{
  int ready[2], go[2];
  int status;
  int initial_slots = swapstat(1);
  if(initial_slots < 0){
    printf("swapforktest: swapstat failed\n");
    exit(1);
  }
  uint64 parent_bytes = (uint64)PARENT_PAGES * PGSIZE;
  volatile char *parent_mem = sbrk((int)parent_bytes);

  if((char *)parent_mem == (char *)-1){
    printf("swapforktest: parent sbrk failed\n");
    exit(1);
  }
  for(int i = 0; i < PARENT_PAGES; i++){
    parent_mem[(uint64)i * PGSIZE] = parent_pattern(i);
    parent_mem[(uint64)i * PGSIZE + PGSIZE - 1] = parent_pattern(i) ^ 0x5a;
  }

  if(pipe(ready) < 0 || pipe(go) < 0){
    printf("swapforktest: pipe failed\n");
    exit(1);
  }

  int pressure_pid = fork();
  if(pressure_pid < 0){
    printf("swapforktest: pressure fork failed\n");
    exit(1);
  }
  if(pressure_pid == 0){
    close(ready[0]);
    close(go[1]);
    pressure_worker(ready[1], go[0]);
  }

  close(ready[1]);
  close(go[0]);
  char signal;
  if(read(ready[0], &signal, 1) != 1 || signal != 'R'){
    printf("swapforktest: pressure setup failed\n");
    wait(&status);
    exit(1);
  }
  close(ready[0]);

  int parent_swapped = swapstat(0);
  if(parent_swapped <= 0){
    signal = 'G';
    write(go[1], &signal, 1);
    close(go[1]);
    wait(&status);
    printf("swapforktest: parent has no swapped pages before fork (%d)\n", parent_swapped);
    exit(1);
  }
  printf("swapforktest: parent has %d swapped pages before fork\n", parent_swapped);

  int verify_pid = fork();
  if(verify_pid < 0){
    signal = 'G';
    write(go[1], &signal, 1);
    close(go[1]);
    wait(&status);
    printf("swapforktest: verification fork failed\n");
    exit(1);
  }
  if(verify_pid == 0){
    close(go[1]);
    int child_swapped = swapstat(0);
    if(child_swapped <= 0){
      printf("swapforktest: child inherited no swapped pages (%d)\n", child_swapped);
      exit(1);
    }
    exit(check_parent(parent_mem) < 0);
  }

  signal = 'G';
  if(write(go[1], &signal, 1) != 1){
    printf("swapforktest: pressure release failed\n");
    exit(1);
  }
  close(go[1]);

  int failed = check_parent(parent_mem) < 0;
  for(int i = 0; i < 2; i++){
    if(wait(&status) < 0 || status != 0)
      failed = 1;
  }

  if(sbrk(-(int)parent_bytes) == (char *)-1)
    failed = 1;
  int final_slots = swapstat(1);
  if(final_slots != initial_slots){
    printf("swapforktest: leaked swap slots, before=%d after=%d\n",
           initial_slots, final_slots);
    failed = 1;
  }

  if(failed){
    printf("swapforktest: FAIL\n");
    exit(1);
  }

  printf("swapforktest: OK\n");
  exit(0);
}
