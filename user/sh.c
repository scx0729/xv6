// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "kernel/fs.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5
#define SUBSHELL 6

#define MAXARGS 10

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

struct subshellcmd{
  int type;
  struct cmd *cmd;
};

int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parsecmd(char*);
void runshellcmd(struct cmd*);

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;
  struct subshellcmd *sscmd;

  if(cmd == 0)
    exit(1);

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit(1);
    exec(ecmd->argv[0], ecmd->argv);
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    if(fork1() == 0){
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if(fork1() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    runcmd(bcmd->cmd);
    break;
  case SUBSHELL:
    sscmd = (struct subshellcmd*)cmd;
    runshellcmd(sscmd->cmd);
    break;
  }
  exit(0);
}

void
runshellcmd(struct cmd *cmd)
{
  if(cmd == 0)
    return;
  // wait必须在父Shell执行
  if(cmd->type == EXEC){
    struct execcmd *ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[1] == 0 && ecmd->argv[0] && strcmp(ecmd->argv[0], "wait") == 0){
      while(wait(0) > 0)
        ;
      return;
    }
    if(ecmd->argv[0] && strcmp(ecmd->argv[0], "cd") == 0){
      if(ecmd->argv[1] && ecmd->argv[2] == 0){
        if(chdir(ecmd->argv[1]) < 0)
          fprintf(2, "cannot cd %s\n", ecmd->argv[1]);
      } else
        fprintf(2, "usage: cd directory\n");
      return;
    }
  }
  // 顺序命令由父Shell递归处理
  if(cmd->type == LIST){
    struct listcmd *lcmd = (struct listcmd*)cmd;
    runshellcmd(lcmd->left);
    runshellcmd(lcmd->right);
    return;
  }
  // 后台命令由父Shell直接创建，不等待
  if(cmd->type == BACK){
    struct backcmd *bcmd = (struct backcmd*)cmd;
    int pid = fork1();
    if(pid == 0){
      runshellcmd(bcmd->cmd);
      exit(0);
    }
    return;
  }
  // 重定向命令由父Shell修改fd并递归处理
  if(cmd->type == REDIR){
    struct redircmd *rcmd = (struct redircmd*)cmd;
    int savefd = dup(rcmd->fd);
    if(savefd < 0){
      fprintf(2, "dup failed\n");
      return;
    }
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      dup(savefd);
      close(savefd);
      fprintf(2, "open %s failed\n", rcmd->file);
      return;
    }
    runshellcmd(rcmd->cmd);
    close(rcmd->fd);
    dup(savefd);
    close(savefd);
    return;
  }
  // 子Shell命令由父Shell直接fork
  if(cmd->type == SUBSHELL){
    struct subshellcmd *sscmd = (struct subshellcmd*)cmd;
    int pid = fork1();
    if(pid == 0){
      runshellcmd(sscmd->cmd);
      exit(0);
    }
    int wpid = 0;
    while((wpid = wait(0)) > 0 && wpid != pid)
      ;
    return;
  }
  // 其他命令作为前台命令执行
  int pid = fork1();
  if(pid == 0)
    runcmd(cmd);
  int wpid = 0;
  while((wpid = wait(0)) > 0 && wpid != pid)
    ;
}

int
complete(char *buf, int len, int max)
{
  int fd, prefix_len, common_len = 0, common_count = 0;
  struct dirent de;
  struct stat st;
  char common[DIRSIZ + 1];

  if((fd = open(".", 0)) < 0){
    fprintf(2, "complete: cannot open \".\"\n");
    return len;
  }
  if(fstat(fd, &st) < 0){
    fprintf(2, "complete: cannot stat \".\"\n");
    close(fd);
    return len;
  }
  if(st.type != T_DIR){
    close(fd);
    return len;
  }
  int start = len;
  while(start > 0 && strchr(symbols, buf[start - 1]) == 0 && strchr(whitespace, buf[start - 1]) == 0)
    start--;
  prefix_len = len - start;
  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;
    char candidate[DIRSIZ + 1];
    for(int i = 0;i < DIRSIZ;i++)
      candidate[i] = de.name[i];
    candidate[DIRSIZ] = '\0';
    int candidate_len = strlen(candidate);
    if(candidate_len < prefix_len)
      continue;
    int flag = 1;
    for(int i = start, j = 0;i < len;i++, j++){
      if(buf[i] != candidate[j]){
        flag = 0;
        break;
      }
    }
    if(!flag)
      continue;
    if(common_count == 0){
      for(int i = 0;i < candidate_len;i++)
        common[i] = candidate[i];
      common_len = candidate_len;
      common[common_len] = '\0';
    } else {
      int min_len = common_len < candidate_len ? common_len : candidate_len;
      common_len = min_len;
      common[common_len] = '\0';
      for(int i = 0;i < min_len;i++)
        if(common[i] != candidate[i]){
          common[i] = '\0';
          common_len = i;
          break;
        } 
    }
    common_count++;
  }
  if(common_count == 0)
    common_len = prefix_len;
  else{
    if(len + common_len - prefix_len + 1 <= max){
      for(int i = prefix_len;i < common_len;i++)
        buf[start + i] = common[i];
      buf[start + common_len] = '\0';
      write(2, common + prefix_len, common_len - prefix_len);
    } else {
      close(fd);
      return len;
    }
  }
  close(fd);
  return start + common_len;
}

char*
getshell(char *buf, int max)
{
  int i, cc;
  char c;
  
  for(i=0; i+1 < max; ){
    cc = read(0, &c, 1);
    if(cc < 1)
      break;
    if(c == '\t')
      i = complete(buf, i, max);
    else
      buf[i++] = c;
    if(c == '\n' || c == '\r')
      break;
  }
  buf[i] = '\0';
  return buf;
}

int
getcmd(char *buf, int nbuf)
{
  struct stat st;
  if(fstat(0, &st) >= 0 && st.type == T_DEVICE)
    fprintf(2, "$ ");
  memset(buf, 0, nbuf);
  getshell(buf, nbuf);
  if(buf[0] == 0) // EOF
    return -1;
  return 0;
}

void
freecmd(struct cmd *cmd)
{
  if(cmd == 0)
    return;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;
  struct subshellcmd *sscmd;

  switch (cmd->type) {
  case EXEC:
      break;
  case REDIR:
      rcmd = (struct redircmd*)cmd;
      freecmd(rcmd->cmd);
      break;
  case LIST:
      lcmd = (struct listcmd*)cmd;
      freecmd(lcmd->left);
      freecmd(lcmd->right);
      break;
  case PIPE:
      pcmd = (struct pipecmd*)cmd;
      freecmd(pcmd->left);
      freecmd(pcmd->right);
      break;
  case BACK:
      bcmd = (struct backcmd*)cmd;
      freecmd(bcmd->cmd);
      break;
  case SUBSHELL:
      sscmd = (struct subshellcmd*)cmd;
      freecmd(sscmd->cmd);
      break;
  default:
      panic("freecmd");
  }
  free(ecmd = (struct execcmd*)cmd);
}

int
main(void)
{
  static char buf[100];
  int fd;

  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    struct cmd *cmd = parsecmd(buf);
    runshellcmd(cmd);
    freecmd(cmd);
  }
  exit(0);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}

struct cmd*
subshellcmd(struct cmd *subcmd)
{
  struct subshellcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = SUBSHELL;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = subshellcmd(cmd);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;
  struct subshellcmd *sscmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  case SUBSHELL:
    sscmd = (struct subshellcmd*)cmd;
    nulterminate(sscmd->cmd);
    break;
  }
  return cmd;
}
