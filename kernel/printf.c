//
// formatted console output -- printf, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

volatile int panicked = 0;

// lock to avoid interleaving concurrent printf's.
static struct {
  struct spinlock lock;
  int locking;
} pr;

struct ksym {
  uint64 start;
  uint64 end;
  char *name;
};

static struct ksym symbols[] = {
  #include "ksyms.inc"
};

struct kline {
  uint64 addr;
  char *file;
  int line;
};

static struct kline lines[] = {
  #include "klines.inc"
};

static char digits[] = "0123456789abcdef";

static void
printint(int xx, int base, int sign)
{
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}

static void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the console. only understands %d, %x, %p, %s.
void
printf(char *fmt, ...)
{
  va_list ap;
  int i, c, locking;
  char *s;

  locking = pr.locking;
  if(locking)
    acquire(&pr.lock);

  if (fmt == 0)
    panic("null fmt");

  va_start(ap, fmt);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, int), 16, 1);
      break;
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 's':
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  if(locking)
    release(&pr.lock);
}

void
panic(char *s)
{
  pr.locking = 0;
  printf("panic: ");
  printf(s);
  printf("\n");
  backtrace();
  panicked = 1; // freeze uart output from other CPUs
  for(;;)
    ;
}

void
printfinit(void)
{
  initlock(&pr.lock, "pr");
  pr.locking = 1;
}

static char *
find_function(uint64 ra)
{
  for(int i = 0;i < NELEM(symbols);i++){
    if(ra - 1 >= symbols[i].start && ra - 1 < symbols[i].end)
      return symbols[i].name;
  }
  return "unknown";
}

static struct kline *
find_line(uint64 ra)
{
  struct kline *best = 0;
  for(int i = 0; i < NELEM(lines); i++){
    if(lines[i].addr <= ra &&
        (best == 0 || lines[i].addr >= best->addr))
      best = &lines[i];
  }
  return best;
}

void
backtrace(void)
{
  uint64 fp = r_fp();
  uint64 up = PGROUNDUP(fp);
  uint64 down = PGROUNDDOWN(fp);
  printf("backtrace:\n");
  while(fp >= down + 16 && fp < up){
    uint64 ra = *(uint64 *)(fp - 8);
    uint64 s0 = *(uint64 *)(fp - 16);
    char *name = find_function(ra);
    struct kline *line = find_line(ra);
    if(line != 0)
      printf("%p %s %s:%d\n", ra, name, line->file, line->line);
    else
      printf("%p %s unknown:0\n", ra, name);
    fp = s0;
  }
  
}
