#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz)
    return -1;
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  int err = copyinstr(p->pagetable, buf, addr, max);
  if(err < 0)
    return err;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
int
argint(int n, int *ip)
{
  *ip = argraw(n);
  return 0;
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
int
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
  return 0;
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  if(argaddr(n, &addr) < 0)
    return -1;
  return fetchstr(addr, buf, max);
}

extern uint64 sys_chdir(void);
extern uint64 sys_close(void);
extern uint64 sys_dup(void);
extern uint64 sys_exec(void);
extern uint64 sys_exit(void);
extern uint64 sys_fork(void);
extern uint64 sys_fstat(void);
extern uint64 sys_getpid(void);
extern uint64 sys_kill(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_mknod(void);
extern uint64 sys_open(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_sleep(void);
extern uint64 sys_unlink(void);
extern uint64 sys_wait(void);
extern uint64 sys_write(void);
extern uint64 sys_uptime(void);
extern uint64 sys_trace(void);
extern uint64 sys_sysinfo(void);

enum argtype {
  ARG_NONE,
  ARG_INT,
  ARG_ADDR,
  ARG_STR,
};

struct syscall_info {
  char *name;
  int nargs;
  enum argtype args[6];
};

static int
append_char(char *buf, int size, int *len, char c)
{
  if(size <= 0 || *len < 0 || *len >= size - 1)
    return -1;
  buf[(*len)++] = c;
  buf[*len] = 0;
  return 0;
}

static int
append_str(char *buf, int size, int *len, char *s)
{
  if(s == 0)
    s = "(null)";
  while(*s){
    if(append_char(buf, size, len, *s++) < 0)
      return -1;
  }
  return 0;
}

static int
append_int(char *buf, int size, int *len, int value)
{
  char digits[16];
  int n = 0;
  uint x;

  if(value < 0){
    if(append_char(buf, size, len, '-') < 0)
      return -1;
    x = -(uint)value;
  } else {
    x = value;
  }

  do {
    digits[n++] = '0' + x % 10;
  } while((x /= 10) != 0);

  while(n > 0){
    if(append_char(buf, size, len, digits[--n]) < 0)
      return -1;
  }
  return 0;
}

static int
append_addr(char *buf, int size, int *len, uint64 addr)
{
  static char digits[] = "0123456789abcdef";

  if(append_str(buf, size, len, "0x") < 0)
    return -1;
  for(int shift = 60; shift >= 0; shift -= 4){
    if(append_char(buf, size, len, digits[(addr >> shift) & 0xf]) < 0)
      return -1;
  }
  return 0;
}

static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_sleep]   sys_sleep,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
[SYS_mknod]   sys_mknod,
[SYS_unlink]  sys_unlink,
[SYS_link]    sys_link,
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
[SYS_trace]   sys_trace,
[SYS_sysinfo] sys_sysinfo,
};

static struct syscall_info syscall_info[] = {
[SYS_fork]    = {"fork",    0, {ARG_NONE}},
[SYS_exit]    = {"exit",    1, {ARG_INT}},
[SYS_wait]    = {"wait",    1, {ARG_ADDR}},
[SYS_pipe]    = {"pipe",    1, {ARG_ADDR}},
[SYS_read]    = {"read",    3, {ARG_INT, ARG_ADDR, ARG_INT}},
[SYS_kill]    = {"kill",    1, {ARG_INT}},
[SYS_exec]    = {"exec",    2, {ARG_STR, ARG_ADDR}},
[SYS_fstat]   = {"fstat",   2, {ARG_INT, ARG_ADDR}},
[SYS_chdir]   = {"chdir",   1, {ARG_STR}},
[SYS_dup]     = {"dup",     1, {ARG_INT}},
[SYS_getpid]  = {"getpid",  0, {ARG_NONE}},
[SYS_sbrk]    = {"sbrk",    1, {ARG_INT}},
[SYS_sleep]   = {"sleep",   1, {ARG_INT}},
[SYS_uptime]  = {"uptime",  0, {ARG_NONE}},
[SYS_open]    = {"open",    2, {ARG_STR, ARG_INT}},
[SYS_write]   = {"write",   3, {ARG_INT, ARG_ADDR, ARG_INT}},
[SYS_mknod]   = {"mknod",   3, {ARG_STR, ARG_INT, ARG_INT}},
[SYS_unlink]  = {"unlink",  1, {ARG_STR}},
[SYS_link]    = {"link",    2, {ARG_STR, ARG_STR}},
[SYS_mkdir]   = {"mkdir",   1, {ARG_STR}},
[SYS_close]   = {"close",   1, {ARG_INT}},
[SYS_trace]   = {"trace",   1, {ARG_INT}},
[SYS_sysinfo] = {"sysinfo", 1, {ARG_ADDR}},
};

static void
format_args(int num, char *buf, int size)
{
  int len = 0;

  if(size <= 0)
    return;
  buf[0] = 0;

  for(int i = 0; i < syscall_info[num].nargs; i++){
    if(i > 0 && append_str(buf, size, &len, ", ") < 0)
      return;

    if(syscall_info[num].args[i] == ARG_INT){
      int value;
      argint(i, &value);
      if(append_int(buf, size, &len, value) < 0)
        return;
    } else if(syscall_info[num].args[i] == ARG_ADDR){
      uint64 addr;
      argaddr(i, &addr);
      if(append_addr(buf, size, &len, addr) < 0)
        return;
    } else if(syscall_info[num].args[i] == ARG_STR){
      char str[MAXPATH];
      if(argstr(i, str, sizeof(str)) < 0)
        safestrcpy(str, "<invalid>", sizeof(str));
      if(append_char(buf, size, &len, '"') < 0 ||
         append_str(buf, size, &len, str) < 0 ||
         append_char(buf, size, &len, '"') < 0)
        return;
    }
  }
}


void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    char argbuf[3 * MAXPATH];
    int trace_before = p->trace_mask & (1 << num);

    if(trace_before || num == SYS_trace)
      format_args(num, argbuf, sizeof(argbuf));

    if(trace_before && num == SYS_exit)
      printf("%d: syscall exit(%s)\n", p->pid, argbuf);

    p->trapframe->a0 = syscalls[num]();
    if(p->trace_mask & (1 << num))
      printf("%d: syscall %s -> %d args=(%s)\n", p->pid,
             syscall_info[num].name, (int)p->trapframe->a0, argbuf);
  } else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
