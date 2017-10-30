//
// Created by Sai Jiang on 17/10/22.
//

#ifndef LLRTP_COMMON_H
#define LLRTP_COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include "GenericQueue.h"
#include "kodoc/kodoc.h"

#define SRC_IP      "127.0.0.1"
#define DST_IP      "127.0.0.1"
#define DST_DPORT   7777
#define SRC_SPORT   8888

#define MAXSYMBOL       (256)
#define MAXSYMBOLSIZE   (1024)

#define LOOPCNT         (65536)

#define INTENDEDLEN     (1500)

#define PADLEN          (INTENDEDLEN - sizeof(uint16_t) - sizeof(uint32_t) - sizeof(long))

// Take care of 'Byte Alignment' !!
typedef struct {
    uint32_t seq;
    long ts;
    uint8_t buf[PADLEN];
} __attribute__((packed)) UserData_t;


#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

#define GetTS() \
        ({ struct timeval tv; gettimeofday(&tv, NULL); \
        tv.tv_sec * 1000 + tv.tv_usec / 1000; })

#define debug(fmt, ...) \
        do { fprintf(stderr, "%s()=> " fmt, __func__, __VA_ARGS__); } while (0)

typedef struct {
    long ts;
    uint32_t CurCapactiy;
    uint32_t MaxCapacity;
    double LimitedRate;
} TokenBucket;

typedef struct {
    iqueue_head qnode;
    uint8_t data[0];
    uint16_t Len;
    uint8_t rawdata[0];
} __attribute__((packed)) SrcData;

typedef struct {
    iqueue_head qnode;
    uint8_t data[0];
} Symbol;

typedef struct {
    iqueue_head qnode;
    uint32_t id;
    kodoc_coder_t enc;
    uint32_t lrank, rrank;
    uint8_t  *pblk;
    TokenBucket tb;
    float nmore;
} EncWrapper;

typedef struct {
    uint32_t id;
    uint8_t data[0];
} Packet;

typedef struct {
    uint32_t id;
    uint32_t rank;
} AckMsg;

typedef struct {
    iqueue_head src_queue;

    iqueue_head sym_queue;

    kodoc_factory_t enc_factory;

    uint32_t maxsymbol, maxsymbolsize, blksize;

    iqueue_head enc_queue;
    int enc_cnt;

    uint32_t NextBlockID;

    Packet *pktbuf;
    uint32_t payload_size;

    int sock;

    float LossRate;
} Transmitter;

typedef struct {
    iqueue_head qnode;
    Packet *pkt;
} ChainedPkt;

typedef struct {
    iqueue_head qnode;
    uint32_t id;
    kodoc_coder_t dec;
    uint8_t  *pblk;
} DecWrapper;

typedef struct {
    Packet *pktbuf;
    uint32_t payload_size;

    uint32_t ExpectedBlockID;
    uint32_t ExpectedSymbolID;

    iqueue_head pkt_queue;

    kodoc_factory_t dec_factory;

    uint32_t maxsymbol, maxsymbolsize, blksize;

    iqueue_head dec_queue;

    iqueue_head sym_queue;

    uint32_t src_cnt;

    iqueue_head src_queue;

    int sock;

    struct sockaddr_in RemoteAddr;
    socklen_t RemoteAddrLen;

} Receiver;

#endif //LLRTP_COMMON_H
