#include "kernel/types.h"
#include "kernel/net.h"
#include "kernel/stat.h"
#include "user/user.h"

#define LOCAL_PORT 7300
#define MIN_FLOOD_PACKETS 32

int
main(void)
{
  uint32 host = (10 << 24) | (0 << 16) | (2 << 8) | 2;
  char buf[128];
  int fd = connect(host, LOCAL_PORT, NET_TESTS_PORT);

  if(fd < 0){
    fprintf(2, "rxlivelocktest: connect failed\n");
    exit(1);
  }
  if(write(fd, "START", 5) != 5){
    fprintf(2, "rxlivelocktest: start failed\n");
    exit(1);
  }

  // Wait until the host has started the receive flood.
  for(;;){
    int n = read(fd, buf, sizeof(buf) - 1);
    if(n < 0){
      fprintf(2, "rxlivelocktest: ready read failed\n");
      exit(1);
    }
    buf[n] = 0;
    if(strcmp(buf, "READY") == 0)
      break;
  }

  int pid = fork();
  if(pid < 0){
    fprintf(2, "rxlivelocktest: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    volatile uint64 sum = 0;
    for(uint64 i = 0; i < 5000000; i++)
      sum += i;
    if(sum == 0 || write(fd, "PROGRESS", 8) != 8)
      exit(1);
    close(fd);
    exit(0);
  }

  int flood_packets = 0;
  int passed = 0;
  for(;;){
    int n = read(fd, buf, sizeof(buf) - 1);
    if(n < 0){
      fprintf(2, "rxlivelocktest: result read failed\n");
      exit(1);
    }
    buf[n] = 0;
    if(strcmp(buf, "FLOOD") == 0){
      flood_packets++;
    } else if(strcmp(buf, "PASS") == 0){
      passed = 1;
      break;
    } else if(strcmp(buf, "FAIL") == 0){
      break;
    }
  }

  int status;
  if(wait(&status) != pid || status != 0){
    fprintf(2, "rxlivelocktest: worker failed\n");
    exit(1);
  }
  close(fd);

  if(!passed){
    fprintf(2, "rxlivelocktest: worker made no progress during flood\n");
    exit(1);
  }
  if(flood_packets < MIN_FLOOD_PACKETS){
    fprintf(2, "rxlivelocktest: insufficient receive load %d\n", flood_packets);
    exit(1);
  }

  printf("RX LIVELOCK TEST PASSED (%d packets)\n", flood_packets);
  exit(0);
}
