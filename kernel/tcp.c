#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

enum tcp_state {
  TCP_CLOSED,
  TCP_SYN_SENT,
  TCP_ESTABLISHED,
  TCP_FIN_WAIT_1,
  TCP_FIN_WAIT_2,
  TCP_CLOSE_WAIT,
  TCP_LAST_ACK,
  TCP_TIME_WAIT,
};

struct tcp_sock {
  struct tcp_sock *next;
  uint32 raddr;
  uint16 lport;
  uint16 rport;
  enum tcp_state state;
  uint32 snd_una;
  uint32 snd_nxt;
  uint32 rcv_nxt;
  uint16 remote_window;
  struct spinlock lock;
  struct mbufq rxq;
  int error;
  int eof;
};

static struct {
  struct spinlock lock;
  struct tcp_sock *head;
  uint32 next_isn;
} tcptable;

void
tcpinit(void)
{
  tcptable.head = 0;
  tcptable.next_isn = 1;
  initlock(&tcptable.lock, "tcpsocktable");
}

static struct tcp_sock *
tcp_lookup_locked(uint32 raddr, uint16 lport, uint16 rport)
{
  for(struct tcp_sock *ts = tcptable.head; ts; ts = ts->next){
    if(ts->raddr == raddr && ts->lport == lport && ts->rport == rport)
      return ts;
  }
  return 0;
}

void
tcp_input(struct mbuf *m, uint32 sip, uint16 lport, uint16 rport, uint32 seq, uint32 ack, uint8 flags, uint16 window)
{
  uint32 reply_seq;
  uint32 reply_ack;
  uint32 reply_ip;
  uint16 reply_lport;
  uint16 reply_rport;
  int send_ack = 0;
  int queued = 0;
  acquire(&tcptable.lock);
  struct tcp_sock *ts = tcp_lookup_locked(sip, lport, rport);
  if(ts == 0){
    release(&tcptable.lock);
    mbuffree(m);
    return;
  }
  acquire(&ts->lock);
  release(&tcptable.lock);
  // RST 表示连接被对端强制终止
  if(flags & TCP_RST){
    ts->error = 1;
    ts->eof = 1;
    ts->state = TCP_CLOSED;
    wakeup(ts);
    goto out;
  }
  if(ts->state == TCP_SYN_SENT){
    if((flags & TCP_SYN) == 0 || (flags & TCP_ACK) == 0 || ack != ts->snd_nxt)
      goto out;
    ts->snd_una = ack;
    ts->rcv_nxt = seq + 1;
    ts->remote_window = window;
    ts->state = TCP_ESTABLISHED;
    wakeup(ts);
    reply_seq = ts->snd_nxt;
    reply_ack = ts->rcv_nxt;
    reply_ip = ts->raddr;
    reply_lport = ts->lport;
    reply_rport = ts->rport;
    send_ack = 1;
  } else if (ts->state == TCP_ESTABLISHED) {
    if((flags & TCP_ACK) == 0)
      goto out;
    if(ack > ts->snd_nxt)
      goto out;
    if(ack > ts->snd_una){
      ts->snd_una = ack;
      wakeup(ts);
    }
    ts->remote_window = window;
    if(m->len == 0 && (flags & TCP_FIN) == 0)
      goto out;
    if(seq != ts->rcv_nxt){
      reply_seq = ts->snd_nxt;
      reply_ack = ts->rcv_nxt;
      reply_ip = ts->raddr;
      reply_lport = ts->lport;
      reply_rport = ts->rport;
      send_ack = 1;
      goto out;
    }
    if(m->len > 0){
      ts->rcv_nxt +=  m->len;
      mbufq_pushtail(&ts->rxq, m);
      queued = 1;
    }
    if(flags & TCP_FIN){
      ts->rcv_nxt++;
      ts->eof = 1;
      ts->state = TCP_CLOSE_WAIT;
    }
    wakeup(ts);
    reply_seq = ts->snd_nxt;
    reply_ack = ts->rcv_nxt;
    reply_ip = ts->raddr;
    reply_lport = ts->lport;
    reply_rport = ts->rport;
    send_ack = 1;
  }
out:
  release(&ts->lock);
  if(!queued)
    mbuffree(m);
  if(send_ack){
    struct mbuf *reply = mbufalloc(MBUF_DEFAULT_HEADROOM);
    if(reply != 0)
      net_tx_tcp(reply, reply_ip, reply_lport, reply_rport, reply_seq, reply_ack, TCP_ACK);
  }
}

static void
tcp_remove(struct tcp_sock *target)
{
  struct tcp_sock **pos;
  acquire(&tcptable.lock);
  acquire(&target->lock);
  pos = &tcptable.head;
  while(*pos != 0){
    if(*pos == target){
      *pos = target->next;
      target->next = 0;
      break;
    }
    pos = &(*pos)->next;
  }
  release(&target->lock);
  release(&tcptable.lock);
}

int tcpconnect(struct file **f, uint32 raddr, uint16 lport, uint16 rport)
{
  struct tcp_sock *ts;
  struct mbuf *m = 0;
  int inserted = 0;
  ts = 0;
  *f = 0;
  if ((*f = filealloc()) == 0)
    goto bad;
  if ((ts = (struct tcp_sock*)kalloc()) == 0)
    goto bad;
  memset(ts, 0, sizeof(*ts));
  initlock(&ts->lock, "tcpsock");
  mbufq_init(&ts->rxq);
  m = mbufalloc(MBUF_DEFAULT_HEADROOM);
  if(m == 0)
    goto bad;
  // initialize objects
  acquire(&tcptable.lock);
  struct tcp_sock *tmp = tcp_lookup_locked(raddr, lport, rport);
  if(tmp != 0){
    release(&tcptable.lock);
    goto bad;
  }
  ts->raddr = raddr;
  ts->lport = lport;
  ts->rport = rport;
  ts->state = TCP_SYN_SENT;
  uint32 iss = tcptable.next_isn++;
  ts->snd_una = iss;
  ts->snd_nxt = iss + 1;
  ts->rcv_nxt = 0;
  ts->error = 0;
  ts->eof = 0;
  ts->next = tcptable.head;
  tcptable.head = ts;
  inserted = 1;
  release(&tcptable.lock);
  net_tx_tcp(m, raddr, lport, rport, iss, 0, TCP_SYN);
  m= 0;
  acquire(&ts->lock);
  while(ts->state == TCP_SYN_SENT && !ts->error && !myproc()->killed)
    sleep(ts, &ts->lock);
  if(ts->state != TCP_ESTABLISHED){
    release(&ts->lock);
    goto bad;
  }
  (*f)->type = FD_TCP;
  (*f)->readable = 1;
  (*f)->writable = 1;
  (*f)->tcp = ts;
  release(&ts->lock);
  return 0;
bad:
  if (inserted)
    tcp_remove(ts);
  if (m)
    mbuffree(m);
  if (ts)
    kfree((char*)ts);
  if (*f)
    fileclose(*f);
  return -1;
}
