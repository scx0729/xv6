#include "kernel/types.h"
#include "user/user.h"

#define NSOCKHASH 13
#define NWORKERS 6
#define NPACKETS 4
#define RCU_WORKERS 4
#define RCU_ROUNDS 32
#define RCU_BURST 4

static uint32
ipaddr(int a, int b, int c, int d)
{
  return ((uint32)a << 24) | ((uint32)b << 16) |
         ((uint32)c << 8) | (uint32)d;
}

static uint32
sockhash(uint32 raddr, uint16 lport, uint16 rport)
{
  uint32 h = raddr;
  h = h * 31 + lport;
  h = h * 31 + rport;
  return h % NSOCKHASH;
}

static void
exchange(int fd, int count)
{
  char *request = "socket hash test";

  for(int i = 0; i < count; i++){
    if(write(fd, request, strlen(request)) != strlen(request)){
      fprintf(2, "sockhashtest: write failed\n");
      exit(1);
    }
  }
  for(int i = 0; i < count; i++){
    char reply[32];
    int n = read(fd, reply, sizeof(reply) - 1);
    if(n < 0){
      fprintf(2, "sockhashtest: read failed\n");
      exit(1);
    }
    reply[n] = 0;
    if(strcmp(reply, "this is the host!") != 0){
      fprintf(2, "sockhashtest: bad reply\n");
      exit(1);
    }
  }
}

static void
warmup(uint32 host, uint16 dport)
{
  int fd = connect(host, 4999, dport);
  if(fd < 0){
    fprintf(2, "sockhashtest: warmup connect failed\n");
    exit(1);
  }
  exchange(fd, 1);
  close(fd);
}

static void
different_buckets(uint32 host, uint16 dport)
{
  const uint16 base = 5000;

  for(int i = 0; i < NWORKERS; i++){
    for(int j = 0; j < i; j++){
      if(sockhash(host, base + i, dport) == sockhash(host, base + j, dport)){
        fprintf(2, "sockhashtest: test ports are not in different buckets\n");
        exit(1);
      }
    }
    int pid = fork();
    if(pid < 0){
      fprintf(2, "sockhashtest: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      int fd = connect(host, base + i, dport);
      if(fd < 0){
        fprintf(2, "sockhashtest: worker connect failed\n");
        exit(1);
      }
      exchange(fd, NPACKETS);
      close(fd);
      exit(0);
    }
  }

  for(int i = 0; i < NWORKERS; i++){
    int status;
    if(wait(&status) < 0 || status != 0){
      fprintf(2, "sockhashtest: worker failed\n");
      exit(1);
    }
  }
}

static void
colliding_sockets(uint32 host, uint16 dport)
{
  const uint16 port1 = 5100;
  const uint16 port2 = port1 + NSOCKHASH;

  if(sockhash(host, port1, dport) != sockhash(host, port2, dport)){
    fprintf(2, "sockhashtest: collision ports hash differently\n");
    exit(1);
  }

  int fd1 = connect(host, port1, dport);
  int fd2 = connect(host, port2, dport);
  if(fd1 < 0 || fd2 < 0){
    fprintf(2, "sockhashtest: collision connect failed\n");
    exit(1);
  }

  exchange(fd1, NPACKETS);
  exchange(fd2, NPACKETS);
  close(fd1);
  close(fd2);
}

static void
duplicate_socket(uint32 host, uint16 dport)
{
  const uint16 port = 5200;
  int fd1 = connect(host, port, dport);
  if(fd1 < 0){
    fprintf(2, "sockhashtest: first duplicate connect failed\n");
    exit(1);
  }

  int fd2 = connect(host, port, dport);
  if(fd2 >= 0){
    fprintf(2, "sockhashtest: duplicate connect succeeded\n");
    close(fd2);
    exit(1);
  }
  close(fd1);

  int fd3 = connect(host, port, dport);
  if(fd3 < 0){
    fprintf(2, "sockhashtest: reconnect after close failed\n");
    exit(1);
  }
  close(fd3);
}

static void
rcu_close_worker(uint32 host, uint16 dport, int id)
{
  char *request = "rcu close race";

  for(int round = 0; round < RCU_ROUNDS; round++){
    uint16 sport = 5300 + id * RCU_ROUNDS + round;
    int fd = connect(host, sport, dport);
    if(fd < 0){
      fprintf(2, "sockhashtest: RCU worker connect failed\n");
      exit(1);
    }
    for(int i = 0; i < RCU_BURST; i++){
      if(write(fd, request, strlen(request)) != strlen(request)){
        fprintf(2, "sockhashtest: RCU worker write failed\n");
        exit(1);
      }
    }

    // Close without reading so incoming replies race with socket removal.
    close(fd);
    if((round & 7) == 7)
      sleep(1);
  }
  exit(0);
}

static void
rcu_close_race(uint32 host, uint16 dport)
{
  for(int i = 0; i < RCU_WORKERS; i++){
    int pid = fork();
    if(pid < 0){
      fprintf(2, "sockhashtest: RCU fork failed\n");
      exit(1);
    }
    if(pid == 0)
      rcu_close_worker(host, dport, i);
  }

  for(int i = 0; i < RCU_WORKERS; i++){
    int status;
    if(wait(&status) < 0 || status != 0){
      fprintf(2, "sockhashtest: RCU worker failed\n");
      exit(1);
    }
  }

  // Let late replies traverse the table after their sockets were removed.
  sleep(10);

  // Verify that socket lists and allocator state remain usable after the race.
  int fd = connect(host, 6500, dport);
  if(fd < 0){
    fprintf(2, "sockhashtest: post-RCU connect failed\n");
    exit(1);
  }
  exchange(fd, NPACKETS);
  close(fd);
}

int
main(void)
{
  uint32 host = ipaddr(10, 0, 2, 2);
  uint16 dport = NET_TESTS_PORT;

  warmup(host, dport);

  printf("testing sockets in different buckets: ");
  different_buckets(host, dport);
  printf("OK\n");

  printf("testing sockets in one collision chain: ");
  colliding_sockets(host, dport);
  printf("OK\n");

  printf("testing duplicate socket rejection: ");
  duplicate_socket(host, dport);
  printf("OK\n");

  printf("testing close against RCU readers: ");
  rcu_close_race(host, dport);
  printf("OK\n");

  printf("SOCKET HASH TESTS PASSED\n");
  exit(0);
}
