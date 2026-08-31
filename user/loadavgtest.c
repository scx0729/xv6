#include "kernel/types.h"
#include "kernel/sysinfo.h"
#include "user/user.h"

#define NWORKERS 8

static uint64
readload(void)
{
  struct sysinfo info;

  if(sysinfo(&info) < 0){
    printf("loadavgtest: sysinfo failed\n");
    exit(1);
  }
  return info.loadavg;
}

static void
printload(char *label, uint64 load)
{
  int whole = load / 100;
  int fraction = load % 100;

  printf("%s: %d.", label, whole);
  if(fraction < 10)
    printf("0");
  printf("%d (raw=%d)\n", fraction, (int)load);
}

int
main(void)
{
  int pids[NWORKERS];
  int nchildren = 0;

  printf("loadavgtest: measuring idle load\n");
  sleep(20);
  uint64 idle = readload();
  printload("idle", idle);

  for(int i = 0; i < NWORKERS; i++){
    int pid = fork();
    if(pid < 0){
      printf("loadavgtest: fork failed\n");
      for(int j = 0; j < nchildren; j++)
        kill(pids[j]);
      for(int j = 0; j < nchildren; j++)
        wait(0);
      exit(1);
    }
    if(pid == 0){
      volatile uint64 counter = 0;
      for(;;)
        counter++;
    }
    pids[nchildren++] = pid;
  }

  printf("loadavgtest: running %d CPU-bound processes\n", NWORKERS);
  sleep(100);
  uint64 busy = readload();
  printload("busy", busy);

  for(int i = 0; i < nchildren; i++)
    kill(pids[i]);
  for(int i = 0; i < nchildren; i++)
    wait(0);

  printf("loadavgtest: measuring recovery\n");
  sleep(100);
  uint64 recovered = readload();
  printload("recovered", recovered);

  if(busy <= idle){
    printf("loadavgtest: FAIL: load did not increase\n");
    exit(1);
  }
  if(recovered >= busy){
    printf("loadavgtest: FAIL: load did not decrease\n");
    exit(1);
  }

  printf("loadavgtest: OK\n");
  exit(0);
}
