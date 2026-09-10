#include "types.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"


#define NMMAPCACHE 256

struct mmap_page {
    int used;
    struct inode *ip;
    uint64 offset;
    uint64 pa;
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

uint64
mmapcache_get(struct inode *ip, uint64 offset)
{
    uint64 pa = 0;
    acquire(&mmapcache.lock);
    for(int i = 0;i < NMMAPCACHE;i++){
        if(mmapcache.pages[i].used && mmapcache.pages[i].ip == ip && mmapcache.pages[i].offset == offset){
            mmapcache.pages[i].refcnt++;
            pa = mmapcache.pages[i].pa;
            break;
        }
    }
    release(&mmapcache.lock);
    return pa;
}

uint64
mmapcache_insert(struct inode *ip, uint64 offset, uint64 newpa)
{
    uint64 pa = 0;
    acquire(&mmapcache.lock);
    for(int i = 0;i < NMMAPCACHE;i++){
        if(mmapcache.pages[i].used && mmapcache.pages[i].ip == ip && mmapcache.pages[i].offset == offset){
            mmapcache.pages[i].refcnt++;
            pa = mmapcache.pages[i].pa;
            break;
        }
    }
    if(pa == 0){
        for(int i = 0;i < NMMAPCACHE;i++){
            if(!mmapcache.pages[i].used){
                mmapcache.pages[i].used = 1;
                mmapcache.pages[i].refcnt = 1;
                mmapcache.pages[i].ip = ip;
                mmapcache.pages[i].offset = offset;
                mmapcache.pages[i].pa = newpa;
                pa = newpa;
                break;
            }
        }
    }
    release(&mmapcache.lock);
    return pa;
}

void
mmapcache_put(uint64 pa)
{
    int flag = 0;
    acquire(&mmapcache.lock);
    for(int i = 0;i < NMMAPCACHE;i++){
        if(mmapcache.pages[i].used && mmapcache.pages[i].pa == pa){
            if(mmapcache.pages[i].refcnt <= 0)
                panic("mmapcache_put");
            if(--mmapcache.pages[i].refcnt == 0){
                mmapcache.pages[i].used = 0;
                mmapcache.pages[i].ip = 0;
                mmapcache.pages[i].offset = 0;
                mmapcache.pages[i].pa = 0;
                flag = 1;
            }
            break;
        }
    }
    release(&mmapcache.lock);
    if(flag)
        kfree((void*)pa);
}