#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define TEST_BODY_LEN 4096
#define LOCAL_PORT 4100

static char response_header[] =
  "HTTP/1.0 200 OK\r\n"
  "Content-Length: 4096\r\n"
  "Connection: close\r\n"
  "\r\n";

static int
write_all(int fd, const char *buf, int len)
{
  int written = 0;

  while(written < len){
    int n = write(fd, buf + written, len - written);
    if(n <= 0)
      return -1;
    written += n;
  }
  return 0;
}

int
main(int argc, char **argv)
{
  uint32 host = (10U << 24) | (0U << 16) | (2U << 8) | 2U;
  uint16 port = NET_TESTS_PORT;
  char request[] =
    "GET /xv6 HTTP/1.0\r\n"
    "Host: 10.0.2.2\r\n"
    "Connection: close\r\n"
    "\r\n";
  char buf[127];
  int total = 0;
  int fd;
  int n;

  if(argc != 1){
    fprintf(2, "usage: tcptest\n");
    exit(1);
  }

  fd = tcp_connect(host, LOCAL_PORT, port);
  if(fd < 0){
    fprintf(2, "tcptest: tcp_connect failed\n");
    exit(1);
  }

  if(write_all(fd, request, strlen(request)) < 0){
    fprintf(2, "tcptest: write failed\n");
    close(fd);
    exit(1);
  }

  while((n = read(fd, buf, sizeof(buf))) > 0){
    for(int i = 0; i < n; i++, total++){
      char expected;
      if(total < strlen(response_header))
        expected = response_header[total];
      else
        expected = 'A' + ((total - strlen(response_header)) % 26);
      if(buf[i] != expected){
        fprintf(2, "tcptest: bad byte at offset %d\n", total);
        close(fd);
        exit(1);
      }
    }
  }

  if(n < 0){
    fprintf(2, "tcptest: read failed\n");
    close(fd);
    exit(1);
  }

  int expected_total = strlen(response_header) + TEST_BODY_LEN;
  if(total != expected_total){
    fprintf(2, "tcptest: got %d bytes, expected %d\n",
            total, expected_total);
    close(fd);
    exit(1);
  }

  close(fd);
  printf("TCP TEST OK (%d bytes)\n", total);
  exit(0);
}
