#include "kernel/types.h"
#include "kernel/net.h"
#include "kernel/stat.h"
#include "user/user.h"

static uint32
ipaddr(int a, int b, int c, int d)
{
  return ((uint32)a << 24) | ((uint32)b << 16) |
         ((uint32)c << 8) | (uint32)d;
}

int
main(void)
{
  uint32 host = ipaddr(10, 0, 2, 2);
  char reply[64];
  char *request = "icmp port unreachable test";

  // The grader verifies that the port immediately after the echo port is free.
  int badfd = connect(host, 6100, NET_TESTS_PORT + 1);
  int goodfd = connect(host, 6101, NET_TESTS_PORT);
  if(badfd < 0 || goodfd < 0){
    fprintf(2, "icmptest: connect failed\n");
    exit(1);
  }

  if(write(badfd, request, strlen(request)) != strlen(request)){
    fprintf(2, "icmptest: write to closed port failed\n");
    exit(1);
  }
  if(write(goodfd, request, strlen(request)) != strlen(request)){
    fprintf(2, "icmptest: write to echo port failed\n");
    exit(1);
  }

  // An ICMP error for badfd must not be delivered to the healthy socket.
  int n = read(goodfd, reply, sizeof(reply) - 1);
  if(n < 0){
    fprintf(2, "icmptest: healthy socket received an error\n");
    exit(1);
  }
  reply[n] = 0;
  if(strcmp(reply, "this is the host!") != 0){
    fprintf(2, "icmptest: bad echo reply\n");
    exit(1);
  }

  // QEMU's user-mode network should translate the host's UDP error into an
  // ICMP destination-unreachable/port-unreachable packet for this socket.
  if(read(badfd, reply, sizeof(reply)) != -1){
    fprintf(2, "icmptest: closed UDP port did not report an error\n");
    exit(1);
  }

  close(goodfd);
  close(badfd);
  printf("ICMP PORT UNREACHABLE TEST PASSED\n");
  exit(0);
}
