//
// network system calls.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"
#include "netqstat.h"

#define NSOCKHASH 13

static uint32 rcu_readers[NCPU];
static uint32 hash(uint32, uint16, uint16);
static void rcu_read_lock();
static void rcu_read_unlock();
static void synchronize_rcu();

uint64
sys_netqstat(void)
{
  uint64 addr;
  struct netqstat stats;

  if(argaddr(0, &addr) < 0)
    return -1;
  e1000_queue_stats(&stats);
  if(copyout(myproc()->pagetable, addr, (char *)&stats, sizeof(stats)) < 0)
    return -1;
  return 0;
}

struct sock {
  struct sock *next; // the next socket in the list
  uint32 raddr;      // the remote IPv4 address
  uint16 lport;      // the local UDP port number
  uint16 rport;      // the remote UDP port number
  struct spinlock lock; // protects the rxq
  struct mbufq rxq;  // a queue of packets waiting to be received
  int error;
};

struct socket_bucket{
  struct spinlock lock;
  struct sock *head;
};

static struct socket_bucket sockethash[NSOCKHASH];

void
sockinit(void)
{
  for(int i = 0;i < NSOCKHASH;i++){
    initlock(&sockethash[i].lock, "socktbl");
    sockethash[i].head = 0;
  }
  for(int i = 0;i < NCPU;i++)
    rcu_readers[i] = 0;
}

int
sockalloc(struct file **f, uint32 raddr, uint16 lport, uint16 rport)
{
  struct sock *si, *pos;

  si = 0;
  *f = 0;
  if ((*f = filealloc()) == 0)
    goto bad;
  if ((si = (struct sock*)kalloc()) == 0)
    goto bad;

  // initialize objects
  si->raddr = raddr;
  si->lport = lport;
  si->rport = rport;
  si->error = 0;
  initlock(&si->lock, "sock");
  mbufq_init(&si->rxq);

  // add to list of sockets
  uint32 index = hash(raddr, lport, rport);
  acquire(&sockethash[index].lock);
  pos = sockethash[index].head;
  while (pos) {
    if (pos->raddr == raddr && pos->lport == lport && pos->rport == rport) {
      release(&sockethash[index].lock);
      goto bad;
    }
    pos = pos->next;
  }
  struct sock *head = __atomic_load_n(&sockethash[index].head, __ATOMIC_ACQUIRE);
  si->next = head;
  __atomic_store_n(&sockethash[index].head, si, __ATOMIC_RELEASE);
  (*f)->type = FD_SOCK;
  (*f)->readable = 1;
  (*f)->writable = 1;
  (*f)->sock = si;
  release(&sockethash[index].lock);
  return 0;

bad:
  if (si)
    kfree((char*)si);
  if (*f)
    fileclose(*f);
  return -1;
}

void
sockclose(struct sock *si)
{
  struct sock **pos;
  struct mbuf *m;

  // remove from list of sockets
  uint32 index = hash(si->raddr, si->lport, si->rport);
  acquire(&sockethash[index].lock);
  pos = &sockethash[index].head;
  while (*pos) {
    if (*pos == si){
      struct sock *next = __atomic_load_n(&si->next, __ATOMIC_ACQUIRE);
      __atomic_store_n(pos, next, __ATOMIC_RELEASE);
      break;
    }
    pos = &(*pos)->next;
  }
  release(&sockethash[index].lock);

  synchronize_rcu();

  // free any pending mbufs
  while (!mbufq_empty(&si->rxq)) {
    m = mbufq_pophead(&si->rxq);
    mbuffree(m);
  }

  kfree((char*)si);
}

int
sockread(struct sock *si, uint64 addr, int n)
{
  struct proc *pr = myproc();
  struct mbuf *m;
  int len;

  acquire(&si->lock);
  while (mbufq_empty(&si->rxq) && !pr->killed && !si->error) {
    sleep(&si->rxq, &si->lock);
  }
  if (pr->killed) {
    release(&si->lock);
    return -1;
  }
  if(mbufq_empty(&si->rxq) && si->error){
    si->error = 0;
    release(&si->lock);
    return -1;
  }
  m = mbufq_pophead(&si->rxq);
  release(&si->lock);

  len = m->len;
  if (len > n)
    len = n;
  if (copyout(pr->pagetable, addr, m->head, len) == -1) {
    mbuffree(m);
    return -1;
  }
  mbuffree(m);
  return len;
}

int
sockwrite(struct sock *si, uint64 addr, int n)
{
  struct proc *pr = myproc();
  struct mbuf *m;

  m = mbufalloc(MBUF_DEFAULT_HEADROOM);
  if (!m)
    return -1;

  if (copyin(pr->pagetable, mbufput(m, n), addr, n) == -1) {
    mbuffree(m);
    return -1;
  }
  net_tx_udp(m, si->raddr, si->lport, si->rport);
  return n;
}

// called by protocol handler layer to deliver UDP packets
void
sockrecvudp(struct mbuf *m, uint32 raddr, uint16 lport, uint16 rport)
{
  //
  // Find the socket that handles this mbuf and deliver it, waking
  // any sleeping reader. Free the mbuf if there are no sockets
  // registered to handle it.
  //
  rcu_read_lock();
  struct sock *si;
  uint32 index = hash(raddr, lport, rport);
  si = __atomic_load_n(&sockethash[index].head, __ATOMIC_ACQUIRE);
  while (si) {
    if (si->raddr == raddr && si->lport == lport && si->rport == rport)
      goto found;
    si = __atomic_load_n(&si->next, __ATOMIC_ACQUIRE);
  }
  mbuffree(m);
  rcu_read_unlock();
  return;

found:
  acquire(&si->lock);
  mbufq_pushtail(&si->rxq, m);
  wakeup(&si->rxq);
  release(&si->lock);
  rcu_read_unlock();
}

static uint32
hash(uint32 raddr, uint16 lport, uint16 rport)
{
  uint32 h;

  h = raddr;
  h = h * 31 + lport;
  h = h * 31 + rport;

  return h % NSOCKHASH;
}

static void
rcu_read_lock()
{
  push_off();
  int cid = cpuid();
  __sync_fetch_and_add(&rcu_readers[cid], 1);
  __sync_synchronize();
}

static void
rcu_read_unlock()
{
  __sync_synchronize();
  int cid = cpuid();
  __sync_fetch_and_sub(&rcu_readers[cid], 1);
  pop_off();
}

static void
synchronize_rcu()
{
  for(int i = 0;i < NCPU;i++){
    while(__atomic_load_n(&rcu_readers[i], __ATOMIC_ACQUIRE) > 0)
      yield();
  }
}

int
sockrecverr(uint32 raddr, uint16 lport, uint16 rport, uint8 type, uint8 code)
{
  rcu_read_lock();
  struct sock *si;
  uint32 index = hash(raddr, lport, rport);
  si = __atomic_load_n(&sockethash[index].head, __ATOMIC_ACQUIRE);
  while (si) {
    if (si->raddr == raddr && si->lport == lport && si->rport == rport)
      goto found;
    si = __atomic_load_n(&si->next, __ATOMIC_ACQUIRE);
  }
  rcu_read_unlock();
  return -1;

found:
  acquire(&si->lock);
  si->error = ((int)type << 8) | code;
  wakeup(&si->rxq);
  release(&si->lock);
  rcu_read_unlock();
  return 0;
}
