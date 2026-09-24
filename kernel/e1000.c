#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"
#include "netqstat.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

#define TX_SW_LIMIT 64
static struct mbufq txq[NTXQ];
static struct spinlock txq_lock[NTXQ];
static int tx_queued[NTXQ];
static uint64 tx_enqueued[NTXQ];
static uint64 tx_dequeued[NTXQ];

#define RX_SW_LIMIT 64
static struct mbufq rxq[NRXQ];
static struct spinlock rxq_lock[NRXQ];
static int rx_queued[NRXQ];
static uint64 rx_enqueued[NRXQ];
static uint64 rx_dequeued[NRXQ];
static uint64 rx_dropped[NRXQ];

#define TX_HW_LIMIT (TX_RING_SIZE - 1)
static struct spinlock tx_hw_lock;
static struct spinlock rx_hw_lock;
static int tx_inflight;
static int tx_clean;
static int next_txq;
static void tx_reclaim();
static void tx_fill();
static void rx_enqueue(int, struct mbuf*);

#define RX_POLL_BUDGET 8
static int e1000_rx_poll(int budget);
static int rx_polling;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  for(int i = 0;i < NTXQ;i++){
    initlock(&txq_lock[i], "e1000_txq");
    mbufq_init(&txq[i]);
    tx_queued[i] = 0;
    tx_enqueued[i] = 0;
    tx_dequeued[i] = 0;
  }
  for(int i = 0;i < NRXQ;i++){
    initlock(&rxq_lock[i], "e1000_rxq");
    mbufq_init(&rxq[i]);
    rx_queued[i] = 0;
    rx_enqueued[i] = 0;
    rx_dequeued[i] = 0;
    rx_dropped[i] = 0;
  }
  rx_polling = 0;
  initlock(&tx_hw_lock, "e1000_tx_hw");
  initlock(&rx_hw_lock, "e1000_rx_hw");

  regs = xregs;

  // Reset the device
  regs[E1000_IMC] = 0xffffffff; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMC] = 0xffffffff; // redisable interrupts
  regs[E1000_RXCSUM] |= E1000_RXCSUM_IPOFL;
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = E1000_ICR_TXDW | E1000_ICR_RXT0; // enable TX completion and RX interrupts
}

int
e1000_transmit(struct mbuf *m)
{
  push_off();
  int qid = cpuid() % NTXQ;
  acquire(&txq_lock[qid]);
  if(tx_queued[qid] >= TX_SW_LIMIT){
    release(&txq_lock[qid]);
    pop_off();
    return -1;
  }
  mbufq_pushtail(&txq[qid], m);
  tx_queued[qid]++;
  tx_enqueued[qid]++;
  release(&txq_lock[qid]);
  acquire(&tx_hw_lock);
  tx_fill();
  release(&tx_hw_lock);
  pop_off();
  return 0;
}

static int
e1000_recv(int budget)
{
  int qid = cpuid() % NRXQ;
  uint32 index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  int processed = 0;
  while((processed < budget) && (rx_ring[index].status & E1000_RXD_STAT_DD)){
    struct mbuf *m = rx_mbufs[index];
    // m->head = (char*)rx_ring[index].addr;
    m->len = rx_ring[index].length;
    m->rx_status = rx_ring[index].status;
    m->rx_errors = rx_ring[index].errors;
    struct mbuf *new_m = mbufalloc(0);
    if(new_m == 0)
      panic("e1000");
    rx_mbufs[index] = new_m;
    rx_ring[index].addr = (uint64)new_m->head;
    // rx_ring[index].length = 0;
    rx_ring[index].status = 0;
    regs[E1000_RDT] = index;
    index = (index + 1) % RX_RING_SIZE;
    rx_enqueue(qid, m);
    processed++;
  }
  return processed;
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  uint32 icr = regs[E1000_ICR];
  if(icr & E1000_ICR_TXDW){
    acquire(&tx_hw_lock);
    tx_reclaim();
    tx_fill();
    release(&tx_hw_lock);
  }

  if(icr & E1000_ICR_RXT0){
    acquire(&rx_hw_lock);
    if(!rx_polling){
      rx_polling = 1;
      regs[E1000_IMC] = E1000_ICR_RXT0;
    }
    release(&rx_hw_lock);
    e1000_poll();
  }
}

static void
tx_reclaim()
{
  while(tx_inflight > 0 && (tx_ring[tx_clean].status & E1000_TXD_STAT_DD) != 0){
    if(tx_mbufs[tx_clean] != 0){
      mbuffree(tx_mbufs[tx_clean]);
      tx_mbufs[tx_clean] = 0;
    }
    tx_inflight--;
    tx_clean = (tx_clean + 1) % TX_RING_SIZE;
  }
}

static void
tx_fill()
{
  int count = 0;
  while(tx_inflight < TX_HW_LIMIT && count < NTXQ){
    int qid = next_txq;
    acquire(&txq_lock[qid]);
    if(mbufq_empty(&txq[qid])){
      release(&txq_lock[qid]);
      count++;
      next_txq = (qid + 1) % NTXQ;
      continue;
    }
    int index = regs[E1000_TDT];
    if((tx_ring[index].status & E1000_TXD_STAT_DD) == 0){
      release(&txq_lock[qid]);
      break;
    }
    struct mbuf *m = mbufq_pophead(&txq[qid]);
    if(m == 0){
      release(&txq_lock[qid]);
      count++;
      next_txq = (qid + 1) % NTXQ;
      continue;
    }
    tx_ring[index].addr = (uint64)m->head;
    tx_ring[index].length = m->len;
    // tx_ring[index].cso = 0;
    // tx_ring[index].css = 0;
    tx_ring[index].cmd = E1000_TXD_CMD_RS | E1000_TXD_CMD_EOP;
    // if(m->tx_csum){
    //   tx_ring[index].css = m->tx_css;
    //   tx_ring[index].cso = m->tx_cso;
    //   tx_ring[index].cmd |= E1000_TXD_CMD_IC;
    // }
    tx_ring[index].status = 0;
    tx_mbufs[index] = m;
    tx_queued[qid]--;
    tx_dequeued[qid]++;
    next_txq = (qid + 1) % NTXQ;
    release(&txq_lock[qid]);
    tx_inflight++;
    count = 0;
    __sync_synchronize();
    regs[E1000_TDT] = (index + 1) % TX_RING_SIZE;
  }
}

static void
rx_enqueue(int qid, struct mbuf *m)
{
  acquire(&rxq_lock[qid]);
  if(rx_queued[qid] >= RX_SW_LIMIT){
    rx_dropped[qid]++;
    release(&rxq_lock[qid]);
    mbuffree(m);
    return;
  }
  mbufq_pushtail(&rxq[qid], m);
  rx_queued[qid]++;
  rx_enqueued[qid]++;
  release(&rxq_lock[qid]);
}

static int
e1000_rx_poll(int budget)
{
  push_off();
  int qid = cpuid() % NRXQ;
  int processed = 0;
  while(processed < budget){
    acquire(&rxq_lock[qid]);
    if(mbufq_empty(&rxq[qid])){
      release(&rxq_lock[qid]);
      break;
    }
    struct mbuf *m = mbufq_pophead(&rxq[qid]);
    if(m == 0){
      release(&rxq_lock[qid]);
      break;
    }
    rx_queued[qid]--;
    rx_dequeued[qid]++;
    release(&rxq_lock[qid]);
    net_rx(m);
    processed++;
  }
  pop_off();
  return processed;
}

void
e1000_queue_stats(struct netqstat *stats)
{
  memset(stats, 0, sizeof(*stats));
  for(int i = 0; i < NTXQ; i++){
    acquire(&txq_lock[i]);
    stats->tx_enqueued[i] = tx_enqueued[i];
    stats->tx_dequeued[i] = tx_dequeued[i];
    stats->tx_queued[i] = tx_queued[i];
    release(&txq_lock[i]);
  }
  for(int i = 0; i < NRXQ; i++){
    acquire(&rxq_lock[i]);
    stats->rx_enqueued[i] = rx_enqueued[i];
    stats->rx_dequeued[i] = rx_dequeued[i];
    stats->rx_dropped[i] = rx_dropped[i];
    stats->rx_queued[i] = rx_queued[i];
    release(&rxq_lock[i]);
  }
}

static int
rx_work_pending_locked(void)
{
  uint32 index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  if(rx_ring[index].status & E1000_RXD_STAT_DD)
    return 1;
  for(int i = 0;i < NRXQ;i++){
    acquire(&rxq_lock[i]);
    int nonempty = !mbufq_empty(&rxq[i]);
    release(&rxq_lock[i]);
    if(nonempty)
      return 1;
  }
  return 0;
}

void
e1000_poll(void)
{
  acquire(&rx_hw_lock);
  if(!rx_polling){
    release(&rx_hw_lock);
    return;
  }
  e1000_recv(RX_POLL_BUDGET);
  release(&rx_hw_lock);
  e1000_rx_poll(RX_POLL_BUDGET);
  acquire(&rx_hw_lock);
  if(!rx_work_pending_locked()){
    rx_polling = 0;
    regs[E1000_IMS] = E1000_ICR_RXT0;
    __sync_synchronize();
    if(rx_work_pending_locked()){
      rx_polling = 1;
      regs[E1000_IMC] = E1000_ICR_RXT0;
    }
  }
  release(&rx_hw_lock);
}
