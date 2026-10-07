#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define SERVER_PORT 2000

static void
fail(char *message)
{
  fprintf(2, "udpservertest: %s\n", message);
  exit(1);
}

int
main(void)
{
  int fd = bind(SERVER_PORT);
  if(fd < 0)
    fail("bind failed");

  int duplicate = bind(SERVER_PORT);
  if(duplicate >= 0){
    close(duplicate);
    fail("duplicate bind succeeded");
  }

  if(write(fd, "x", 1) != -1)
    fail("write succeeded on a bound socket");

  int saw_one = 0;
  int saw_two = 0;
  for(int attempts = 0; attempts < 32 && (!saw_one || !saw_two); attempts++){
    char request[64];
    uint32 src_ip = 0;
    uint16 src_port = 0;
    int n = recvfrom(fd, request, sizeof(request) - 1, &src_ip, &src_port);
    if(n < 0)
      fail("recvfrom failed");
    request[n] = 0;

    if(src_ip == 0 || src_port == 0)
      fail("recvfrom returned an invalid source address");

    char *reply;
    if(strcmp(request, "udp-one") == 0){
      saw_one = 1;
      reply = "reply-one";
    } else if(strcmp(request, "udp-two") == 0){
      saw_two = 1;
      reply = "reply-two";
    } else {
      fail("unexpected request payload");
    }

    int reply_len = strlen(reply);
    if(sendto(fd, reply, reply_len, src_ip, src_port) != reply_len)
      fail("sendto failed");
  }

  if(!saw_one || !saw_two)
    fail("did not receive both clients");

  close(fd);
  printf("UDP SERVER TEST PASSED\n");
  exit(0);
}
