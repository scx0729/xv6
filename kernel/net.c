//
// networking protocol support (IP, UDP, ARP, etc.).
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "net.h"
#include "defs.h"

static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15); // qemu's idea of the guest IP
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint8 broadcast_mac[ETHADDR_LEN] = { 0xFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF };
static int net_tx_arp(uint16 op, uint8 dmac[ETHADDR_LEN], uint32 dip);
static uint64 getticks();

#define ARP_CACHE_SIZE 16
#define ARP_PENDING_LIMIT 16
#define ARP_RETRY_TICKS 10
#define ARP_MAX_RETRIES 3
#ifdef ARP_CACHE_TEST
#define ARP_ENTRY_TTL 30
#else
#define ARP_ENTRY_TTL 600
#endif
enum {
  ARP_EMPTY,
  ARP_PENDING,
  ARP_RESOLVED,
};

struct arp_entry {
  int state;
  uint32 ip;
  uint8 mac[ETHADDR_LEN];

  struct mbufq pending;
  int pending_count;

  uint64 last_used;
  uint64 requested_at;
  uint64 resolved_at;
  int retries;
};
static struct {
  struct spinlock lock;
  struct arp_entry entries[ARP_CACHE_SIZE];
  uint64 sequence;
} arpcache;

static struct arp_entry* arp_find_locked(uint32);
static struct arp_entry* arp_alloc_locked(uint32);
static int arp_resolve_or_queue(uint32, struct mbuf*, uint8*);
static int arp_update(uint32, uint8*);

void
arp_init(void)
{
  initlock(&arpcache.lock, "arpcachelock");
  for(int i = 0;i < ARP_CACHE_SIZE;i++){
    arpcache.entries[i].state = ARP_EMPTY;
    mbufq_init(&arpcache.entries[i].pending);
  }
}

static uint64
getticks()
{
  acquire(&tickslock);
  uint now = ticks;
  release(&tickslock);
  return now;
}

// Strips data from the start of the buffer and returns a pointer to it.
// Returns 0 if less than the full requested length is available.
char *
mbufpull(struct mbuf *m, unsigned int len)
{
  char *tmp = m->head;
  if (m->len < len)
    return 0;
  m->len -= len;
  m->head += len;
  return tmp;
}

// Prepends data to the beginning of the buffer and returns a pointer to it.
char *
mbufpush(struct mbuf *m, unsigned int len)
{
  m->head -= len;
  if (m->head < m->buf)
    panic("mbufpush");
  m->len += len;
  return m->head;
}

// Appends data to the end of the buffer and returns a pointer to it.
char *
mbufput(struct mbuf *m, unsigned int len)
{
  char *tmp = m->head + m->len;
  m->len += len;
  if (m->len > MBUF_SIZE)
    panic("mbufput");
  return tmp;
}

// Strips data from the end of the buffer and returns a pointer to it.
// Returns 0 if less than the full requested length is available.
char *
mbuftrim(struct mbuf *m, unsigned int len)
{
  if (len > m->len)
    return 0;
  m->len -= len;
  return m->head + m->len;
}

// Allocates a packet buffer.
struct mbuf *
mbufalloc(unsigned int headroom)
{
  struct mbuf *m;
 
  if (headroom > MBUF_SIZE)
    return 0;
  m = kalloc();
  if (m == 0)
    return 0;
  m->next = 0;
  m->head = (char *)m->buf + headroom;
  m->len = 0;
  memset(m->buf, 0, sizeof(m->buf));
  return m;
}

// Frees a packet buffer.
void
mbuffree(struct mbuf *m)
{
  kfree(m);
}

// Pushes an mbuf to the end of the queue.
void
mbufq_pushtail(struct mbufq *q, struct mbuf *m)
{
  m->next = 0;
  if (!q->head){
    q->head = q->tail = m;
    return;
  }
  q->tail->next = m;
  q->tail = m;
}

// Pops an mbuf from the start of the queue.
struct mbuf *
mbufq_pophead(struct mbufq *q)
{
  struct mbuf *head = q->head;
  if (!head)
    return 0;
  q->head = head->next;
  return head;
}

// Returns one (nonzero) if the queue is empty.
int
mbufq_empty(struct mbufq *q)
{
  return q->head == 0;
}

// Intializes a queue of mbufs.
void
mbufq_init(struct mbufq *q)
{
  q->head = 0;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

// sends an ethernet packet
static void
net_tx_eth(struct mbuf *m, uint16 ethtype, uint8 *destination_mac)
{
  struct eth *ethhdr;

  ethhdr = mbufpushhdr(m, *ethhdr);
  memmove(ethhdr->shost, local_mac, ETHADDR_LEN);
  // In a real networking stack, dhost would be set to the address discovered
  // through ARP. Because we don't support enough of the ARP protocol, set it
  // to broadcast instead.
  memmove(ethhdr->dhost, destination_mac, ETHADDR_LEN);
  ethhdr->type = htons(ethtype);
  if (e1000_transmit(m)) {
    mbuffree(m);
  }
}

// sends an IP packet
static void
net_tx_ip(struct mbuf *m, uint8 proto, uint32 dip)
{
  struct ip *iphdr;

  // push the IP header
  iphdr = mbufpushhdr(m, *iphdr);
  memset(iphdr, 0, sizeof(*iphdr));
  iphdr->ip_vhl = (4 << 4) | (20 >> 2);
  iphdr->ip_p = proto;
  iphdr->ip_src = htonl(local_ip);
  iphdr->ip_dst = htonl(dip);
  iphdr->ip_len = htons(m->len);
  iphdr->ip_ttl = 100;
  iphdr->ip_sum = in_cksum((unsigned char *)iphdr, sizeof(*iphdr));

  // now on to the ethernet layer
  uint32 next_hop;
  uint32 gateway_ip = MAKE_IP_ADDR(10, 0, 2, 2);
  if((dip & 0xffffff00) == (local_ip & 0xffffff00))
    next_hop = dip;
  else
    next_hop = gateway_ip;
  uint8 mac[ETHADDR_LEN];
  int result = arp_resolve_or_queue(next_hop, m, mac);
  if(result == 1)
    net_tx_eth(m, ETHTYPE_IP, mac);
  else if (result == 2)
    net_tx_arp(ARP_OP_REQUEST, broadcast_mac, next_hop);
  else if (result == -1)
    mbuffree(m);
}

// sends a UDP packet
void
net_tx_udp(struct mbuf *m, uint32 dip, uint16 sport, uint16 dport)
{
  struct udp *udphdr;

  // put the UDP header
  udphdr = mbufpushhdr(m, *udphdr);
  udphdr->sport = htons(sport);
  udphdr->dport = htons(dport);
  udphdr->ulen = htons(m->len);
  udphdr->sum = 0; // zero means no checksum is provided

  // now on to the IP layer
  net_tx_ip(m, IPPROTO_UDP, dip);
}

// sends an ARP packet
static int
net_tx_arp(uint16 op, uint8 dmac[ETHADDR_LEN], uint32 dip)
{
  struct mbuf *m;
  struct arp *arphdr;

  m = mbufalloc(MBUF_DEFAULT_HEADROOM);
  if (!m)
    return -1;

  // generic part of ARP header
  arphdr = mbufputhdr(m, *arphdr);
  arphdr->hrd = htons(ARP_HRD_ETHER);
  arphdr->pro = htons(ETHTYPE_IP);
  arphdr->hln = ETHADDR_LEN;
  arphdr->pln = sizeof(uint32);
  arphdr->op = htons(op);

  // ethernet + IP part of ARP header
  memmove(arphdr->sha, local_mac, ETHADDR_LEN);
  arphdr->sip = htonl(local_ip);
  arphdr->tip = htonl(dip);

  // header is ready, send the packet
  if(op == ARP_OP_REQUEST){
    memset(arphdr->tha, 0, sizeof(arphdr->tha));
    net_tx_eth(m, ETHTYPE_ARP, broadcast_mac);
  } else if(op == ARP_OP_REPLY) {
    memmove(arphdr->tha, dmac, ETHADDR_LEN);
    net_tx_eth(m, ETHTYPE_ARP, dmac);
  } else {
    mbuffree(m);
    return -1;
  }
  return 0;
}

// receives an ARP packet
static void
net_rx_arp(struct mbuf *m)
{
  struct arp *arphdr;
  uint8 smac[ETHADDR_LEN];
  uint32 sip, tip;

  arphdr = mbufpullhdr(m, *arphdr);
  if (!arphdr)
    goto done;

  // validate the ARP header
  if (ntohs(arphdr->hrd) != ARP_HRD_ETHER ||
      ntohs(arphdr->pro) != ETHTYPE_IP ||
      arphdr->hln != ETHADDR_LEN ||
      arphdr->pln != sizeof(uint32)) {
    goto done;
  }

  // only requests are supported so far
  // check if our IP was solicited
  tip = ntohl(arphdr->tip); // target IP address
  if ((ntohs(arphdr->op) != ARP_OP_REQUEST && ntohs(arphdr->op) != ARP_OP_REPLY) || tip != local_ip)
    goto done;

  // handle the ARP request and reply
  memmove(smac, arphdr->sha, ETHADDR_LEN); // sender's ethernet address
  sip = ntohl(arphdr->sip); // sender's IP address (qemu's slirp)
  arp_update(sip, smac);
  if(ntohs(arphdr->op) == ARP_OP_REQUEST)
    net_tx_arp(ARP_OP_REPLY, smac, sip);

done:
  mbuffree(m);
}

// receives a UDP packet
static void
net_rx_udp(struct mbuf *m, uint16 len, struct ip *iphdr)
{
  struct udp *udphdr;
  uint32 sip;
  uint16 sport, dport;


  udphdr = mbufpullhdr(m, *udphdr);
  if (!udphdr)
    goto fail;

  // TODO: validate UDP checksum

  // validate lengths reported in headers
  if (ntohs(udphdr->ulen) != len)
    goto fail;
  len -= sizeof(*udphdr);
  if (len > m->len)
    goto fail;
  // minimum packet size could be larger than the payload
  mbuftrim(m, m->len - len);

  // parse the necessary fields
  sip = ntohl(iphdr->ip_src);
  sport = ntohs(udphdr->sport);
  dport = ntohs(udphdr->dport);
  sockrecvudp(m, sip, dport, sport);
  return;

fail:
  mbuffree(m);
}

// receives an IP packet
static void
net_rx_ip(struct mbuf *m)
{
  struct ip *iphdr;
  uint16 len;

  iphdr = mbufpullhdr(m, *iphdr);
  if (!iphdr)
	  goto fail;

  // check IP version and header len
  if (iphdr->ip_vhl != ((4 << 4) | (20 >> 2)))
    goto fail;
  // validate IP checksum
  if (in_cksum((unsigned char *)iphdr, sizeof(*iphdr)))
    goto fail;
  // can't support fragmented IP packets
  if (htons(iphdr->ip_off) != 0)
    goto fail;
  // is the packet addressed to us?
  if (htonl(iphdr->ip_dst) != local_ip)
    goto fail;
  // can only support UDP
  if (iphdr->ip_p != IPPROTO_UDP)
    goto fail;

  len = ntohs(iphdr->ip_len) - sizeof(*iphdr);
  net_rx_udp(m, len, iphdr);
  return;

fail:
  mbuffree(m);
}

// called by e1000 driver's interrupt handler to deliver a packet to the
// networking stack
void net_rx(struct mbuf *m)
{
  struct eth *ethhdr;
  uint16 type;

  ethhdr = mbufpullhdr(m, *ethhdr);
  if (!ethhdr) {
    mbuffree(m);
    return;
  }

  type = ntohs(ethhdr->type);
  if (type == ETHTYPE_IP)
    net_rx_ip(m);
  else if (type == ETHTYPE_ARP)
    net_rx_arp(m);
  else
    mbuffree(m);
}

static struct arp_entry*
arp_find_locked(uint32 ip)
{
  struct arp_entry *entry = 0;
  for(int i = 0;i < ARP_CACHE_SIZE;i++){
    struct arp_entry* arp = &arpcache.entries[i];
    if(arp->state != ARP_EMPTY && arp->ip == ip){
      entry = arp;
      break;
    }
  }
  return entry;
}

static struct arp_entry*
arp_alloc_locked(uint32 ip)
{
  struct arp_entry *entry = 0;
  for(int i = 0;i < ARP_CACHE_SIZE;i++){
    struct arp_entry* arp = &arpcache.entries[i];
    if(arp->state == ARP_EMPTY){
      entry = arp;
      break;
    }
  }
  if(!entry){
    for(int i = 0;i < ARP_CACHE_SIZE;i++){
      struct arp_entry* arp = &arpcache.entries[i];
      if(arp->state == ARP_RESOLVED && (entry == 0 || arp->last_used < entry->last_used))
        entry = arp;
    }
  }
  if(entry){
    entry->state = ARP_PENDING;
    entry->ip = ip;
    memset(entry->mac, 0, sizeof(entry->mac));
    mbufq_init(&entry->pending);
    entry->pending_count = 0;
    entry->last_used = ++arpcache.sequence;
    entry->requested_at = 0;
    entry->retries = 0;
    entry->resolved_at = 0;
  }
  return entry;
}

static int
arp_resolve_or_queue(uint32 ip, struct mbuf *m, uint8 *mac)
{
  uint64 now = getticks();
  acquire(&arpcache.lock);
  struct arp_entry *arp = arp_find_locked(ip);
  if(arp != 0){
    if(arp->state == ARP_RESOLVED){
      memmove(mac, arp->mac, ETHADDR_LEN);
      arp->last_used = ++arpcache.sequence;
      release(&arpcache.lock);
      return 1;
    } else if (arp->state == ARP_PENDING){
      if(arp->pending_count >= ARP_PENDING_LIMIT){
        release(&arpcache.lock);
        return -1;
      }
      mbufq_pushtail(&arp->pending, m);
      arp->pending_count++;
      release(&arpcache.lock);
      return 0;
    }
  }
  struct arp_entry *entry = arp_alloc_locked(ip);
  if(entry == 0){
    release(&arpcache.lock);
    return -1;
  }
  mbufq_pushtail(&entry->pending, m);
  entry->pending_count++;
  entry->requested_at = now;
  entry->retries = 1;
  release(&arpcache.lock);
  return 2;
}

static int
arp_update(uint32 sip, uint8 *sha)
{
  uint64 now = getticks();
  acquire(&arpcache.lock);
  struct arp_entry *arp = arp_find_locked(sip);
  if(arp != 0){
    if(arp->state == ARP_RESOLVED){
      memmove(arp->mac, sha, ETHADDR_LEN);
      arp->last_used = ++arpcache.sequence;
      arp->requested_at = 0;
      arp->retries = 0;
      arp->resolved_at = now;
      release(&arpcache.lock);
      return 0;
    } else if (arp->state == ARP_PENDING){
      struct mbufq q;
      uint8 resolved_mac[ETHADDR_LEN];
      arp->state = ARP_RESOLVED;
      memmove(arp->mac, sha, ETHADDR_LEN);
      memmove(resolved_mac, arp->mac, ETHADDR_LEN);
      arp->last_used = ++arpcache.sequence;
      q = arp->pending;
      mbufq_init(&arp->pending);
      arp->pending_count = 0;
      arp->requested_at = 0;
      arp->retries = 0;
      arp->resolved_at = now;
      release(&arpcache.lock);
      while(!mbufq_empty(&q)) {
        struct mbuf *m = mbufq_pophead(&q);
        net_tx_eth(m, ETHTYPE_IP, resolved_mac);
      }
      return 0;
    }
  }
  struct arp_entry *entry = arp_alloc_locked(sip);
  if(entry == 0){
    release(&arpcache.lock);
    return -1;
  }
  memmove(entry->mac, sha, ETHADDR_LEN);
  entry->state = ARP_RESOLVED;
  entry->last_used = ++arpcache.sequence;
  entry->resolved_at = now;
  release(&arpcache.lock);
  return 0;
}

void
arp_tick(uint64 now)
{
  acquire(&arpcache.lock);
  struct mbufq q[ARP_CACHE_SIZE];
  uint32 retry_ip[ARP_CACHE_SIZE];
  int retry_cnt = 0;
  for(int i = 0;i < ARP_CACHE_SIZE;i++){
    struct arp_entry *entry = &arpcache.entries[i];
    mbufq_init(&q[i]);
    if(entry->state == ARP_PENDING){
      if(now - entry->requested_at >= ARP_RETRY_TICKS){
        if(entry->retries >= ARP_MAX_RETRIES){
          q[i] = entry->pending;
          mbufq_init(&entry->pending);
          entry->pending_count = 0;
          entry->requested_at = 0;
          entry->retries = 0;
          entry->state = ARP_EMPTY;
        } else {
          entry->retries++;
          entry->requested_at = now;
          retry_ip[retry_cnt++] = entry->ip;
        }
      }
    } else if (entry->state == ARP_RESOLVED){
      if(now - entry->resolved_at >= ARP_ENTRY_TTL){
        entry->state = ARP_EMPTY;
        entry->resolved_at = 0;
      }
    }
  }
  release(&arpcache.lock);
  for(int i = 0;i < retry_cnt;i++)
    net_tx_arp(ARP_OP_REQUEST, broadcast_mac, retry_ip[i]);
  for(int i = 0;i < ARP_CACHE_SIZE;i++){
    while(!mbufq_empty(&q[i])){
      struct mbuf *m = mbufq_pophead(&q[i]);
      mbuffree(m);
    }
  }
}
