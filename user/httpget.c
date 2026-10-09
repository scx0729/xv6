#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define LOCAL_PORT 4101

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
  uint32 host = (104U << 24) | (154U << 16) | (89U << 8) | 105U;
  uint16 port = 80;
  char request[] =
    "GET / HTTP/1.0\r\n"
    "Host: http.badssl.com\r\n"
    "Connection: close\r\n"
    "\r\n";
  char header_end[] = "\r\n\r\n";
  char buf[256];
  int header_match = 0;
  int headers_done = 0;
  int fd;
  int n;

  if(argc != 1){
    fprintf(2, "usage: httpget\n");
    exit(1);
  }

  fd = tcp_connect(host, LOCAL_PORT, port);
  if(fd < 0){
    fprintf(2, "httpget: tcp_connect failed\n");
    exit(1);
  }

  if(write_all(fd, request, strlen(request)) < 0){
    fprintf(2, "httpget: request write failed\n");
    close(fd);
    exit(1);
  }

  while((n = read(fd, buf, sizeof(buf))) > 0){
    int body_start = 0;

    if(!headers_done){
      body_start = n;
      for(int i = 0; i < n; i++){
        if(buf[i] == header_end[header_match]){
          header_match++;
          if(header_match == 4){
            headers_done = 1;
            body_start = i + 1;
            break;
          }
        } else {
          header_match = (buf[i] == '\r') ? 1 : 0;
        }
      }
    }

    if(headers_done && body_start < n){
      if(write_all(1, buf + body_start, n - body_start) < 0){
        fprintf(2, "httpget: stdout write failed\n");
        close(fd);
        exit(1);
      }
    }
  }

  if(n < 0){
    fprintf(2, "httpget: response read failed\n");
    close(fd);
    exit(1);
  }
  if(!headers_done){
    fprintf(2, "httpget: incomplete HTTP response\n");
    close(fd);
    exit(1);
  }

  close(fd);
  exit(0);
}
