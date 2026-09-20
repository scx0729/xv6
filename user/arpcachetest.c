#include "kernel/types.h"
#include "kernel/net.h"
#include "kernel/stat.h"
#include "user/user.h"

#define TEST_TTL_TICKS 30
#define RETRY_WAIT_TICKS 35

static uint32
ipaddr(int a, int b, int c, int d)
{
  return ((uint32)a << 24) | ((uint32)b << 16) |
         ((uint32)c << 8) | (uint32)d;
}

static void
host_ping(uint16 sport)
{
  char *request = "arp cache test";
  char reply[64];
  int fd = connect(ipaddr(10, 0, 2, 2), sport, NET_TESTS_PORT);
  if(fd < 0){
    fprintf(2, "arpcachetest: connect failed\n");
    exit(1);
  }
  if(write(fd, request, strlen(request)) != strlen(request)){
    fprintf(2, "arpcachetest: write failed\n");
    exit(1);
  }
  int n = read(fd, reply, sizeof(reply) - 1);
  if(n < 0){
    fprintf(2, "arpcachetest: read failed\n");
    exit(1);
  }
  reply[n] = 0;
  close(fd);
  if(strcmp(reply, "this is the host!") != 0){
    fprintf(2, "arpcachetest: bad reply\n");
    exit(1);
  }
}

int
main(void)
{
  printf("testing ARP cache expiration: ");
  host_ping(4100);
  sleep(TEST_TTL_TICKS + 5);
  host_ping(4101);
  printf("traffic generated\n");

  printf("testing ARP timeout retries: ");
  int fd = connect(ipaddr(10, 0, 2, 99), 4200, NET_TESTS_PORT);
  if(fd < 0){
    fprintf(2, "arpcachetest: timeout connect failed\n");
    exit(1);
  }
  if(write(fd, "unreachable", 11) != 11){
    fprintf(2, "arpcachetest: timeout write failed\n");
    exit(1);
  }
  close(fd);
  sleep(RETRY_WAIT_TICKS);
  printf("traffic generated\n");

  printf("ARP CACHE TEST DONE\n");
  exit(0);
}
