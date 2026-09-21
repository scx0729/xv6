#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/netqstat.h"
#include "user/user.h"

#define NWORKERS 6
#define NPACKETS 8

static void
warmup(void)
{
  uint32 dst = (10 << 24) | (0 << 16) | (2 << 8) | 2;
  char *msg = "net queue warmup";
  char reply[32];
  int fd = connect(dst, 3999, NET_TESTS_PORT);

  if(fd < 0 || write(fd, msg, strlen(msg)) != strlen(msg)){
    fprintf(2, "netqtest: warmup send failed\n");
    exit(1);
  }
  int n = read(fd, reply, sizeof(reply) - 1);
  if(n < 0){
    fprintf(2, "netqtest: warmup read failed\n");
    exit(1);
  }
  reply[n] = 0;
  close(fd);
  if(strcmp(reply, "this is the host!") != 0){
    fprintf(2, "netqtest: bad warmup reply\n");
    exit(1);
  }
}

static void
worker(int id)
{
  uint32 dst = (10 << 24) | (0 << 16) | (2 << 8) | 2;
  char *msg = "net queue test";
  int fd = connect(dst, 4000 + id, NET_TESTS_PORT);

  if(fd < 0){
    fprintf(2, "netqtest: connect failed\n");
    exit(1);
  }
  for(int i = 0; i < NPACKETS; i++){
    if(write(fd, msg, strlen(msg)) != strlen(msg)){
      fprintf(2, "netqtest: write failed\n");
      exit(1);
    }
  }
  for(int i = 0; i < NPACKETS; i++){
    char reply[32];
    int n = read(fd, reply, sizeof(reply) - 1);
    if(n < 0){
      fprintf(2, "netqtest: read failed\n");
      exit(1);
    }
    reply[n] = 0;
    if(strcmp(reply, "this is the host!") != 0){
      fprintf(2, "netqtest: bad reply\n");
      exit(1);
    }
  }
  close(fd);
  exit(0);
}

int
main(void)
{
  struct netqstat before, after;
  uint64 tx_total = 0;
  uint64 rx_total = 0;
  uint64 rx_drops = 0;
  int active_txq = 0;

  warmup();
  if(netqstat(&before) < 0){
    fprintf(2, "netqtest: initial netqstat failed\n");
    exit(1);
  }

  for(int i = 0; i < NWORKERS; i++){
    int pid = fork();
    if(pid < 0){
      fprintf(2, "netqtest: fork failed\n");
      exit(1);
    }
    if(pid == 0)
      worker(i);
  }

  for(int i = 0; i < NWORKERS; i++){
    int status;
    if(wait(&status) < 0 || status != 0){
      fprintf(2, "netqtest: worker failed\n");
      exit(1);
    }
  }

  sleep(5);
  if(netqstat(&after) < 0){
    fprintf(2, "netqtest: final netqstat failed\n");
    exit(1);
  }

  for(int i = 0; i < NCPU; i++){
    uint64 tx_in = after.tx_enqueued[i] - before.tx_enqueued[i];
    uint64 tx_out = after.tx_dequeued[i] - before.tx_dequeued[i];
    uint64 rx_in = after.rx_enqueued[i] - before.rx_enqueued[i];
    uint64 rx_out = after.rx_dequeued[i] - before.rx_dequeued[i];

    if(tx_in != 0){
      active_txq++;
      printf("txq %d: enqueued %d dequeued %d\n", i, tx_in, tx_out);
    }
    if(rx_in != 0)
      printf("rxq %d: enqueued %d dequeued %d\n", i, rx_in, rx_out);
    if(before.tx_queued[i] + tx_in != after.tx_queued[i] + tx_out){
      fprintf(2, "netqtest: TX queue %d accounting mismatch\n", i);
      exit(1);
    }
    if(before.rx_queued[i] + rx_in != after.rx_queued[i] + rx_out){
      fprintf(2, "netqtest: RX queue %d accounting mismatch\n", i);
      exit(1);
    }
    if(after.tx_queued[i] != 0 || after.rx_queued[i] != 0){
      fprintf(2, "netqtest: queue %d did not drain\n", i);
      exit(1);
    }
    tx_total += tx_in;
    rx_total += rx_in;
    rx_drops += after.rx_dropped[i] - before.rx_dropped[i];
  }

  if(active_txq < 2){
    fprintf(2, "netqtest: only %d TX queue was active\n", active_txq);
    exit(1);
  }
  if(tx_total < NWORKERS * NPACKETS || rx_total < NWORKERS * NPACKETS){
    fprintf(2, "netqtest: too few packets: tx %d rx %d\n", tx_total, rx_total);
    exit(1);
  }
  if(rx_drops != 0){
    fprintf(2, "netqtest: RX dropped %d packets\n", rx_drops);
    exit(1);
  }

  printf("netqtest: OK (%d TX queues, tx %d, rx %d)\n",
         active_txq, tx_total, rx_total);
  exit(0);
}
