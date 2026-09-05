#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* Possible states of a thread: */
#define FREE        0x0
#define RUNNING     0x1
#define RUNNABLE    0x2
#define BLOCKED     0x3

#define STACK_SIZE  8192
#define MAX_THREAD  4

struct thread_context{
  uint64 sp;
  uint64 ra;
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

struct thread {
  char       stack[STACK_SIZE]; /* the thread's stack */
  int        state;             /* FREE, RUNNING, RUNNABLE, BLOCKED */
  void*      chan;
  struct thread_context context;
};
struct thread_mutex {
  int locked;
};
struct thread_cond {
  int unused;
};
struct thread_barrier {
  struct thread_mutex mutex;
  struct thread_cond cond;
  int nthread;     // 每轮需要到达的线程数
  int arrived;     // 当前轮已经到达的数量
  int round;       // 当前轮次
};
struct thread all_thread[MAX_THREAD];
struct thread *current_thread;
struct thread_mutex ready_mutex;
struct thread_cond ready_cond;
struct thread_barrier ready_barrier;
int ready = 0;
extern void thread_switch(uint64, uint64);
              
void 
thread_init(void)
{
  // main() is thread 0, which will make the first invocation to
  // thread_schedule().  it needs a stack so that the first thread_switch() can
  // save thread 0's state.  thread_schedule() won't run the main thread ever
  // again, because its state is set to RUNNING, and thread_schedule() selects
  // a RUNNABLE thread.
  current_thread = &all_thread[0];
  current_thread->state = RUNNING;
}

void 
thread_schedule(void)
{
  struct thread *t, *next_thread;

  /* Find another runnable thread. */
  next_thread = 0;
  t = current_thread + 1;
  for(int i = 0; i < MAX_THREAD; i++){
    if(t >= all_thread + MAX_THREAD)
      t = all_thread;
    if(t->state == RUNNABLE) {
      next_thread = t;
      break;
    }
    t = t + 1;
  }

  if (next_thread == 0) {
    printf("thread_schedule: no runnable threads\n");
    exit(-1);
  }

  if (current_thread != next_thread) {         /* switch threads?  */
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;
    thread_switch((uint64)&t->context, (uint64)&current_thread->context);
  } else
    next_thread = 0;
}

void 
thread_create(void (*func)())
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == FREE) break;
  }
  if(t >= all_thread + MAX_THREAD){
    printf("thread_create: no free thread\n");
    exit(-1);
  }
  memset(&t->context, 0, sizeof(t->context));
  t->chan = 0;
  t->context.ra = (uint64)func;
  t->context.sp = (uint64)t->stack + STACK_SIZE;
  t->state = RUNNABLE;
}

void 
thread_yield(void)
{
  current_thread->state = RUNNABLE;
  thread_schedule();
}

void
thread_block(void *chan)
{
  current_thread->chan = chan;
  current_thread->state = BLOCKED;
  thread_schedule();
  current_thread->chan = 0;
}

void
thread_wakeup_one(void *chan)
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == BLOCKED && t->chan == chan){
      t->state = RUNNABLE;
      break;
    }
  }
}

void
thread_wakeup_all(void *chan)
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == BLOCKED && t->chan == chan){
      t->state = RUNNABLE;
    }
  }
}

void
thread_mutex_init(struct thread_mutex *mutex)
{
  mutex->locked = 0;
}

void
thread_mutex_lock(struct thread_mutex *mutex)
{
  while(mutex->locked){
    thread_block(mutex);
  }
  mutex->locked = 1;
}

void
thread_mutex_unlock(struct thread_mutex *mutex)
{
  mutex->locked = 0;
  thread_wakeup_one(mutex);
}

void
thread_cond_init(struct thread_cond *cond)
{
  cond->unused = 0;
}

void
thread_cond_wait(struct thread_cond *cond, struct thread_mutex *m)
{
  thread_mutex_unlock(m);
  thread_block(cond);
  thread_mutex_lock(m);
}


void
thread_cond_signal(struct thread_cond *cond)
{
  thread_wakeup_one(cond);
}

void
thread_cond_broadcast(struct thread_cond *cond)
{
  thread_wakeup_all(cond);
}

void
barrier_init(int nthread)
{
  ready_barrier.nthread = nthread;
  ready_barrier.arrived = 0;
  ready_barrier.round = 0;
  thread_mutex_init(&ready_barrier.mutex);
  thread_cond_init(&ready_barrier.cond);
}

void
barrier()
{
  thread_mutex_lock(&ready_barrier.mutex);
  int local_round = ready_barrier.round;
  ready_barrier.arrived++;
  if(ready_barrier.arrived == ready_barrier.nthread){
    ready_barrier.arrived = 0;
    ready_barrier.round++;
    thread_cond_broadcast(&ready_barrier.cond);
  } else {
    while(local_round == ready_barrier.round)
      thread_cond_wait(&ready_barrier.cond, &ready_barrier.mutex);
  }
  thread_mutex_unlock(&ready_barrier.mutex);
}

volatile int a_started, b_started, c_started;
volatile int a_n, b_n, c_n;
int shared_count = 0;

void 
challenge_thread_a(void)
{
  int i;
  for(i = 0;i < 100;i++){
    thread_mutex_lock(&ready_mutex);
    int old_count = shared_count;
    thread_yield();
    shared_count = old_count + 1;
    thread_mutex_unlock(&ready_mutex);
  }
  printf("thread_a started\n");
  a_started = 1;

  thread_mutex_lock(&ready_mutex);
  while(ready == 0){
    printf("thread_a: waiting\n");
    thread_cond_wait(&ready_cond, &ready_mutex);
  }
  printf("thread_a: ready\n");
  thread_mutex_unlock(&ready_mutex);

  while(b_started == 0 || c_started == 0)
    thread_yield();
  if(shared_count != 300)
    printf("mutex test failed: %d\n", shared_count);
  else
    printf("mutex test passed: %d\n", shared_count);
  for (i = 0; i < 100; i++) {
    printf("thread_a %d\n", i);
    a_n += 1;
    thread_yield();
  }

  for (i = 0; i < 2000; i++) {
    int t = ready_barrier.round;
    if(i != t)
      printf("barrier_a test failed: %dround\n", i);
    barrier();
  }
  printf("thread_a: exit after %d\n", a_n);

  current_thread->state = FREE;
  thread_schedule();
}

void 
challenge_thread_b(void)
{
  int i;
  for(i = 0;i < 100;i++){
    thread_mutex_lock(&ready_mutex);
    int old_count = shared_count;
    thread_yield();
    shared_count = old_count + 1;
    thread_mutex_unlock(&ready_mutex);
  }
  printf("thread_b started\n");
  b_started = 1;

  thread_mutex_lock(&ready_mutex);
  while(ready == 0){
    printf("thread_b: waiting\n");
    thread_cond_wait(&ready_cond, &ready_mutex);
  }
  printf("thread_b: ready\n");
  thread_mutex_unlock(&ready_mutex);

  while(a_started == 0 || c_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_b %d\n", i);
    b_n += 1;
    thread_yield();
  }

  for (i = 0; i < 2000; i++) {
    int t = ready_barrier.round;
    if(i != t)
      printf("barrier_b test failed: %dround\n", i);
    barrier();
  }
  printf("thread_b: exit after %d\n", b_n);

  current_thread->state = FREE;
  thread_schedule();
}

void 
challenge_thread_c(void)
{
  int i;
  for(i = 0;i < 100;i++){
    thread_mutex_lock(&ready_mutex);
    int old_count = shared_count;
    thread_yield();
    shared_count = old_count + 1;
    thread_mutex_unlock(&ready_mutex);
  }
  printf("thread_c started\n");
  c_started = 1;
  while(a_started == 0 || b_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_c %d\n", i);
    c_n += 1;
    thread_yield();
  }

  thread_mutex_lock(&ready_mutex);
  ready = 1;
  printf("thread_c: broadcast\n");
  thread_cond_broadcast(&ready_cond);
  thread_mutex_unlock(&ready_mutex);

  for (i = 0; i < 2000; i++) {
    int t = ready_barrier.round;
    if(i != t)
      printf("barrier_c test failed: %dround\n", i);
    barrier();
  }
  printf("thread_c: exit after %d\n", c_n);
  current_thread->state = FREE;
  thread_schedule();
}

void
thread_a(void)
{
  int i;
  printf("thread_a started\n");
  a_started = 1;
  while(b_started == 0 || c_started == 0)
    thread_yield();

  for (i = 0; i < 100; i++) {
    printf("thread_a %d\n", i);
    a_n += 1;
    thread_yield();
  }
  printf("thread_a: exit after %d\n", a_n);

  current_thread->state = FREE;
  thread_schedule();
}

void
thread_b(void)
{
  int i;
  printf("thread_b started\n");
  b_started = 1;
  while(a_started == 0 || c_started == 0)
    thread_yield();

  for (i = 0; i < 100; i++) {
    printf("thread_b %d\n", i);
    b_n += 1;
    thread_yield();
  }
  printf("thread_b: exit after %d\n", b_n);

  current_thread->state = FREE;
  thread_schedule();
}

void
thread_c(void)
{
  int i;
  printf("thread_c started\n");
  c_started = 1;
  while(a_started == 0 || b_started == 0)
    thread_yield();

  for (i = 0; i < 100; i++) {
    printf("thread_c %d\n", i);
    c_n += 1;
    thread_yield();
  }
  printf("thread_c: exit after %d\n", c_n);

  current_thread->state = FREE;
  thread_schedule();
}

int 
main(int argc, char *argv[]) 
{
  a_started = b_started = c_started = 0;
  a_n = b_n = c_n = 0;
  thread_init();
  if(argc > 1 && strcmp(argv[1], "challenge") == 0){
    ready = 0;
    shared_count = 0;
    thread_mutex_init(&ready_mutex);
    thread_cond_init(&ready_cond);
    barrier_init(3);
    thread_create(challenge_thread_a);
    thread_create(challenge_thread_b);
    thread_create(challenge_thread_c);
  } else {
    thread_create(thread_a);
    thread_create(thread_b);
    thread_create(thread_c);
  }
  thread_schedule();
  exit(0);
}
