#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "buf.h"

struct {
    struct spinlock lock;
    struct sleeplock io_lock;
    char used[NSWAP];
}swap;


void swapinit(void)
{
    initlock(&swap.lock, "swap");
    initsleeplock(&swap.io_lock, "swapio");
}

int
swapused(void)
{
    int count = 0;
    acquire(&swap.lock);
    for(int i = 0; i < NSWAP; i++)
        count += swap.used[i] != 0;
    release(&swap.lock);
    return count;
}

int
swapalloc()
{
    acquire(&swap.lock);
    for(int i = 0;i < NSWAP;i++)
        if(swap.used[i] == 0){
            swap.used[i] = 1;
            release(&swap.lock);
            return i;
        }
    release(&swap.lock);
    return -1;
}

int
swapfree(int slot)
{
    if(slot < 0 || slot >= NSWAP)
        return -1;
    acquire(&swap.lock);
    if(swap.used[slot] == 0){
        release(&swap.lock);
        return -1;
    }
    swap.used[slot] = 0;
    release(&swap.lock);
    return 0;
}

int
swapwrite(int slot, uint64 pa)
{
    if(slot < 0 || slot >= NSWAP || pa == 0 || pa % PGSIZE != 0)
        return -1;
    acquiresleep(&swap.io_lock);
    int blockno = FSSIZE + slot;
    struct buf* bp = bread(ROOTDEV, blockno);
    if(bp == 0){
        releasesleep(&swap.io_lock);
        return -1;
    }
    memmove(bp->data, (void*)pa, PGSIZE);
    bwrite(bp);
    brelse(bp);
    releasesleep(&swap.io_lock);
    return 0;
}

int
swapread(int slot, uint64 pa)
{
    if(slot < 0 || slot >= NSWAP || pa == 0 || pa % PGSIZE != 0)
        return -1;
    acquiresleep(&swap.io_lock);
    int blockno = FSSIZE + slot;
    struct buf* bp = bread(ROOTDEV, blockno);
    if(bp == 0){
        releasesleep(&swap.io_lock);
        return -1;
    }
    memmove((void*)pa, bp->data, PGSIZE);
    brelse(bp);
    releasesleep(&swap.io_lock);
    return 0;
}

int
swapcopy(int from_slot, int to_slot)
{
  if(from_slot < 0 || from_slot >= NSWAP || to_slot < 0 || to_slot >= NSWAP)
    return -1;
  if(from_slot == to_slot)
    return 0;

  // The caller owns both slots for the duration of this operation:
  // from_slot is owned by the source PTE and to_slot was just allocated
  // for the destination PTE.  Therefore no used[] check is needed here.
  // The ownership rule also guarantees that neither slot is freed while
  // the copy is in progress.
  acquiresleep(&swap.io_lock);

  struct buf *from = bread(ROOTDEV, FSSIZE + from_slot);
  if(from == 0){
    releasesleep(&swap.io_lock);
    return -1;
  }
  struct buf *to = bread(ROOTDEV, FSSIZE + to_slot);
  if(to == 0){
    brelse(from);
    releasesleep(&swap.io_lock);
    return -1;
  }

  memmove(to->data, from->data, PGSIZE);
  bwrite(to);
  brelse(to);
  brelse(from);
  releasesleep(&swap.io_lock);
  return 0;
}
