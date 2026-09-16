#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "file.h"
#include "fcntl.h"
#include "buf.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

int
vmafault(struct proc *p, uint64 fault_addr, int access)
{
  int vma_index = -1;
  struct vma saved;
  struct buf *bp = 0;
  uint64 pa = 0;
  uint64 file_offset = 0;
  int shared_page = 0;
  int result = -1;

  uint64 page_va = PGROUNDDOWN(fault_addr);
  acquiresleep(&p->vm_lock);

  for(int i = 0; i < MAX_VAM; i++){
    if(p->vmas[i].addr <= fault_addr && fault_addr < p->vmas[i].addr + p->vmas[i].length && p->vmas[i].type != VMA_UNUSED){
      vma_index = i;
      saved = p->vmas[i];
      break;
    }
  }
  if(vma_index < 0 || (saved.prot & access) == 0 || ((saved.type == VMA_ELF || saved.type == VMA_ANON) && fault_addr >= p->sz)){
    releasesleep(&p->vm_lock);
    return -1;
  }

  pte_t *pte = walk(p->pagetable, page_va, 0);
  if(pte && (*pte & PTE_V)){
    releasesleep(&p->vm_lock);
    return -1;
  }
  if(pte && (*pte & PTE_SWAP)){
    releasesleep(&p->vm_lock);
    return pagein(p, fault_addr);
  }
  releasesleep(&p->vm_lock);

  uint64 delta = page_va - saved.addr;
  file_offset = saved.offset + delta;
  uint64 n = PGSIZE;
  if(saved.type == VMA_FILE){
    if(n > saved.length - delta)
      n = saved.length - delta;

    if(saved.flags == MAP_SHARED){
      shared_page = 1;
      bp = mmapcache_get(saved.file->ip, file_offset);
      if(bp == 0){
        struct buf *newbp = 0;
        ilock(saved.file->ip);
        if((newbp = readb(saved.file->ip, file_offset)) == 0){
          iunlock(saved.file->ip);
          return -1;
        }
        iunlock(saved.file->ip);
        bp = mmapcache_insert(saved.file->ip, file_offset, newbp);
        brelse(newbp);
        if(bp == 0)
          return -1;
      }
      pa = (uint64)bp->data;
    } else {
      void *mem = userpagealloc(p);
      if(mem == 0)
        return -1;
      memset(mem, 0, PGSIZE);
      ilock(saved.file->ip);
      if(readi(saved.file->ip, 0, (uint64)mem, file_offset, n) < 0){
        iunlock(saved.file->ip);
        kfree(mem);
        return -1;
      }
      iunlock(saved.file->ip);
      pa = (uint64)mem;
    }
  } else if (saved.type == VMA_ANON){
    void *mem = userpagealloc(p);
    if(mem == 0)
      return -1;
    memset(mem, 0, PGSIZE);
    pa = (uint64)mem;
  } else if (saved.type == VMA_ELF){
    void *mem = userpagealloc(p);
    if(mem == 0)
      return -1;
    memset(mem, 0, PGSIZE);
    if(saved.file_length > delta){
      if(n > saved.file_length - delta)
        n = saved.file_length - delta;
      ilock(saved.ip);
      if(readi(saved.ip, 0, (uint64)mem, file_offset, n) != n){
        iunlock(saved.ip);
        kfree(mem);
        return -1;
      }
      iunlock(saved.ip);
    }
    pa = (uint64)mem;
  }

  acquiresleep(&p->vm_lock);
  struct vma *current = &p->vmas[vma_index];
  if(current->type != saved.type || current->addr != saved.addr ||
     current->length != saved.length || current->file_length != saved.file_length ||
     current->prot != saved.prot || current->flags != saved.flags ||
     current->file != saved.file || current->ip != saved.ip ||
     current->offset != saved.offset ||
     ((current->type == VMA_ELF || current->type == VMA_ANON) && fault_addr >= p->sz))
    goto discard_locked;

  pte = walk(p->pagetable, page_va, 0);
  if(pte && (*pte & PTE_V)){
    result = 0;
    goto discard_locked;
  }
  if(pte && (*pte & PTE_SWAP)){
    releasesleep(&p->vm_lock);
    if(shared_page)
      mmapcache_put(saved.file->ip, file_offset);
    else
      kfree((void*)pa);
    return pagein(p, fault_addr);
  }

  int flags = PTE_U;
  if(saved.prot & PROT_READ)
    flags |= PTE_R;
  if(saved.prot & PROT_WRITE)
    flags |= PTE_R | PTE_W;
  if(saved.prot & PROT_EXEC)
    flags |= PTE_X;
  if(mappages(p->pagetable, page_va, PGSIZE, pa, flags) < 0){
    goto discard_locked;
  }
  releasesleep(&p->vm_lock);
  return 0;

discard_locked:
  releasesleep(&p->vm_lock);
  if(shared_page)
    mmapcache_put(saved.file->ip, file_offset);
  else if(pa)
    kfree((void*)pa);
  return result;
}

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if(r_scause() == 12 || r_scause() == 13 || r_scause() == 15){
    int access;
    if(r_scause() == 12)
      access = PROT_EXEC;
    else if(r_scause() == 13)
      access = PROT_READ;
    else
      access = PROT_WRITE;
    if(vmafault(p, r_stval(), access) < 0)
      exit(-1);
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}
