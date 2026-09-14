#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "fcntl.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"


static int
elf_flags_to_prot(uint flags)
{
  int port = PROT_NONE;

  if(flags & ELF_PROG_FLAG_READ)
    port |= PROT_READ;
  if(flags & ELF_PROG_FLAG_WRITE)
    port |= PROT_WRITE;
  if(flags & ELF_PROG_FLAG_EXEC)
    port |= PROT_EXEC;
  return port;
}

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off, vma_count = 0;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();
  struct vma new_vmas[MAX_VAM];
  memset(new_vmas, 0, sizeof(new_vmas));

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(vma_count >= MAX_VAM)
      goto bad;

    struct vma *v = &new_vmas[vma_count++];
    v->addr = ph.vaddr;
    v->length = ph.memsz;
    v->offset = ph.off;
    v->file_length = ph.filesz;
    v->flags = MAP_PRIVATE;
    v->file = 0;
    v->ip = idup(ip);
    v->type = VMA_ELF;
    v->prot = elf_flags_to_prot(ph.flags);

    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    uint64 end = ph.vaddr + ph.memsz;
    if(end > sz)
      sz = end;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate two pages at the next page boundary.
  // Use the second as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz-2*PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  for(int i = 0;i < MAX_VAM;i++){
    struct vma *v = &p->vmas[i];
    if(v->type == VMA_FILE){
      for(uint64 va = v->addr; va < v->addr + v->length; va += PGSIZE){
        pte_t *pte = walk(oldpagetable, va, 0);
        if(pte == 0 || (*pte & PTE_V) == 0)
          continue;
        uint64 pa = PTE2PA(*pte);
        uint64 delta = va - v->addr;
        uint64 file_offset = v->offset + delta;
        // exec 相当于解除所有旧映射，共享修改需要写回
        if(v->flags == MAP_SHARED && (v->prot & PROT_WRITE)){
          uint64 n = PGSIZE;
          if(n > v->length - delta)
            n = v->length - delta;
          begin_op();
          ilock(v->file->ip);
          writei(v->file->ip, 0, pa, file_offset, n);
          iunlock(v->file->ip);
          end_op();
        }
        if(v->flags == MAP_SHARED){
          uvmunmap(oldpagetable, va, 1, 0);
          mmapcache_put(v->file->ip, file_offset);
        } else {
          uvmunmap(oldpagetable, va, 1, 1);
        }
      }
      fileclose(v->file);
    }
  }
  for(int i = 0;i < MAX_VAM;i++)
    if(p->vmas[i].type == VMA_ELF){
      begin_op();
      iput(p->vmas[i].ip);
      end_op();
    }
  memmove(p->vmas, new_vmas, sizeof(new_vmas));

  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  begin_op();
  for(int i = 0;i < MAX_VAM;i++)
    if(new_vmas[i].ip)
      iput(new_vmas[i].ip);
  end_op();
  return -1;
}
