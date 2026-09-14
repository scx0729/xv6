#include "types.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"


#define NMMAPCACHE 256

struct mmap_page {
    int used;
    struct inode *ip;
    uint64 offset;
    struct buf *bp;
    int refcnt;
};

static struct {
    struct spinlock lock;
    struct mmap_page pages[NMMAPCACHE];
}mmapcache;

void
mmapcacheinit(void)
{
    initlock(&mmapcache.lock, "mmapcache");
}

struct buf*
mmapcache_get(struct inode *ip, uint64 offset)
{
    struct buf *bp = 0;
    acquire(&mmapcache.lock);
    for(int i = 0;i < NMMAPCACHE;i++){
        if(mmapcache.pages[i].used && mmapcache.pages[i].ip == ip && mmapcache.pages[i].offset == offset){
            mmapcache.pages[i].refcnt++;
            bp = mmapcache.pages[i].bp;
            break;
        }
    }
    release(&mmapcache.lock);
    return bp;
}

struct buf*
mmapcache_insert(struct inode *ip, uint64 offset, struct buf *newbp)
{
    struct buf *bp = 0;
    acquire(&mmapcache.lock);
    for(int i = 0;i < NMMAPCACHE;i++){
        if(mmapcache.pages[i].used && mmapcache.pages[i].ip == ip && mmapcache.pages[i].offset == offset){
            mmapcache.pages[i].refcnt++;
            bp = mmapcache.pages[i].bp;
            break;
        }
    }
    if(bp == 0){
        for(int i = 0;i < NMMAPCACHE;i++){
            if(!mmapcache.pages[i].used){
                mmapcache.pages[i].used = 1;
                mmapcache.pages[i].refcnt = 1;
                mmapcache.pages[i].ip = ip;
                mmapcache.pages[i].offset = offset;
                mmapcache.pages[i].bp = newbp;
                bpin(newbp);
                bp = newbp;
                break;
            }
        }
    }
    release(&mmapcache.lock);
    return bp;
}

void
mmapcache_put(struct inode *ip, uint64 offset)
{
    struct buf *bp = 0;
    acquire(&mmapcache.lock);
    for(int i = 0;i < NMMAPCACHE;i++){
        if(mmapcache.pages[i].used && mmapcache.pages[i].ip == ip && mmapcache.pages[i].offset == offset){
            if(mmapcache.pages[i].refcnt <= 0)
                panic("mmapcache_put");
            if(--mmapcache.pages[i].refcnt == 0){
                bp = mmapcache.pages[i].bp;
                mmapcache.pages[i].used = 0;
                mmapcache.pages[i].ip = 0;
                mmapcache.pages[i].offset = 0;
                mmapcache.pages[i].bp = 0;
            }
            break;
        }
    }
    release(&mmapcache.lock);
    if(bp)
        bunpin(bp);
}