#ifndef _NETQSTAT_H_
#define _NETQSTAT_H_

struct netqstat {
  uint64 tx_enqueued[NCPU];
  uint64 tx_dequeued[NCPU];
  uint64 tx_queued[NCPU];
  uint64 rx_enqueued[NCPU];
  uint64 rx_dequeued[NCPU];
  uint64 rx_dropped[NCPU];
  uint64 rx_queued[NCPU];
};

#endif
