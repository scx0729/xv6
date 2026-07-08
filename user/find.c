#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char*
fmtname(char *path)
{
  char *p;
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  return p+1;
}

void
find(char *path, char *target){
    char buf[512], *p;
    char name[DIRSIZ + 1];
    int fd;
    struct dirent de;
    struct stat st;
    if((fd = open(path, 0)) < 0){
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }
    if(fstat(fd, &st) < 0){
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }
    switch (st.type) {
    case T_FILE:
      if(strcmp(fmtname(path), target) == 0) printf("%s\n", path);
      close(fd);
      return;
    case T_DIR:
      if(strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)){
        fprintf(2, "find: path too long\n");
        close(fd);
        return;
      }
      strcpy(buf, path);
      p = buf + strlen(buf);
      *p++ = '/';
      while(read(fd, &de, sizeof(de)) == sizeof(de)){
        if(de.inum == 0) continue;
        memmove(name, de.name, DIRSIZ);
        name[DIRSIZ] = 0;
        if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        strcpy(p, name);
        find(buf, target);
      }
    }
    close(fd);
}

int
main(int argc, char *argv[]){
    if(argc != 3){
        fprintf(2, "usage: find path filename\n");
        exit(1);
    }
  find(argv[1], argv[2]);
  exit(0);
}