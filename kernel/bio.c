// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct bucket{
  struct spinlock lock;
  struct buf head;
};

struct {
  struct buf buf[NBUF];
  struct bucket bucket[NBUCKET];
  struct spinlock evict_lock;
  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
} bcache;

#define REF_EVICTING ((uint)-1)

static int
bhash(uint dev, uint blockno)
{
  return (dev + blockno * 31) % NBUCKET;
}

void
binit(void)
{
  struct buf *b;
  initlock(&bcache.evict_lock, "bcache.evict");
  for(int i = 0;i < NBUCKET;i++){
    initlock(&bcache.bucket[i].lock, "bcache");
    // Create linked list of buffers
    bcache.bucket[i].head.prev = &bcache.bucket[i].head;
    bcache.bucket[i].head.next = &bcache.bucket[i].head;
  }

  for(int i = 0;i < NBUF;i++){
    b = &bcache.buf[i];
    int index = i % NBUCKET;
    b->next = bcache.bucket[index].head.next;
    b->prev = &bcache.bucket[index].head;
    initsleeplock(&b->lock, "buffer");
    bcache.bucket[index].head.next->prev = b;
    bcache.bucket[index].head.next = b;
  }

}

int CAS(uint *x, uint old, uint new)
{
  if(__sync_bool_compare_and_swap(x, old, new))
    return 1;
  return 0;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int index = bhash(dev, blockno);

  // Is the block already cached?
  for(int i = 0;i < NBUF;i++){
    b = &bcache.buf[i];
    if(b->dev == dev && b->blockno == blockno){
      uint dev = b->dev, blockno = b->blockno;
      uint refcnt = 0;

      int pinned = 0;
      for(;;){
        refcnt = __sync_fetch_and_add(&b->refcnt, 0);
        if(refcnt == REF_EVICTING)
          break;
        if(CAS(&b->refcnt, refcnt, refcnt + 1)){
          pinned = 1;
          break;
        }
      }
      if(!pinned)
        continue;

      if(b->dev != dev || b->blockno != blockno){
        __sync_fetch_and_sub(&b->refcnt, 1);
        continue;
      }
      acquiresleep(&b->lock);
      return b;
    }
  }

  acquire(&bcache.evict_lock);
  acquire(&bcache.bucket[index].lock);
  for(b = bcache.bucket[index].head.next; b != &bcache.bucket[index].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      __sync_fetch_and_add(&b->refcnt, 1);
      release(&bcache.bucket[index].lock);
      release(&bcache.evict_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket[index].lock);
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(int i = 0; i < NBUCKET;i++){
    acquire(&bcache.bucket[i].lock);
    for(b = bcache.bucket[i].head.prev; b != &bcache.bucket[i].head; b = b->prev){
      if(__sync_fetch_and_add(&b->refcnt, 0) == 0){
        if(i != index)
          acquire(&bcache.bucket[index].lock);
        if(!CAS(&b->refcnt, 0, REF_EVICTING)){
          if(i != index)
            release(&bcache.bucket[index].lock);
          continue;
        }
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        struct buf *f = b->prev;
        f->next = b->next;
        b->next->prev = f;
        // 加入新bucket链表
        b->next = bcache.bucket[index].head.next;
        b->prev = &bcache.bucket[index].head;
        bcache.bucket[index].head.next->prev = b;
        bcache.bucket[index].head.next = b;
        if(!CAS(&b->refcnt, REF_EVICTING, 1))
          panic("bget");
        if(i != index)
          release(&bcache.bucket[index].lock);
        release(&bcache.bucket[i].lock);
        release(&bcache.evict_lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.bucket[i].lock);
  }
  release(&bcache.evict_lock);
  // for(b = bcache.bucket[index].head.prev; b != &bcache.bucket[index].head; b = b->prev){
  //   if(b->refcnt == 0) {
  //     b->dev = dev;
  //     b->blockno = blockno;
  //     b->valid = 0;
  //     b->refcnt = 1;
  //     release(&bcache.bucket[index].lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  int index = bhash(b->dev, b->blockno);
  releasesleep(&b->lock);

  uint refcnt = __sync_fetch_and_sub(&b->refcnt, 1);
  acquire(&bcache.bucket[index].lock);
  if (refcnt == 1) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.bucket[index].head.next;
    b->prev = &bcache.bucket[index].head;
    bcache.bucket[index].head.next->prev = b;
    bcache.bucket[index].head.next = b;
  }

  release(&bcache.bucket[index].lock);
}

void
bpin(struct buf *b) {
  uint refcnt = __sync_fetch_and_add(&b->refcnt, 0);
  while(!CAS(&b->refcnt, refcnt, refcnt + 1))
    refcnt = __sync_fetch_and_add(&b->refcnt, 0);
}

void
bunpin(struct buf *b) {
  uint refcnt = __sync_fetch_and_add(&b->refcnt, 0);
  while(!CAS(&b->refcnt, refcnt, refcnt - 1))
    refcnt = __sync_fetch_and_add(&b->refcnt, 0);
}


