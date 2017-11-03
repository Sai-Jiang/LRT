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
#include <pthread.h>
#include "kodoc/kodoc.h"


//#define DST_IP      "104.207.145.15"
#define DST_IP      "127.0.0.1"
#define DST_DPORT   7777

#define MAXSYMBOL       (256)
#define MAXSYMBOLSIZE   (1024)

#define ENCWNDSZ        (5)

#define LOOPCNT         (65536)

#define INTENDEDLEN     (1600)

#define PADLEN          (INTENDEDLEN - sizeof(uint16_t) - sizeof(uint32_t) - sizeof(long))


//---------------------------------------------------------------------
// queue type
//---------------------------------------------------------------------
struct IQUEUEHEAD {
    struct IQUEUEHEAD *next, *prev;
};

typedef struct IQUEUEHEAD iqueue_head;

//---------------------------------------------------------------------
// queue init
//---------------------------------------------------------------------
#define IQUEUE_HEAD_INIT(name) { &(name), &(name) }
#define IQUEUE_HEAD(name) \
	struct IQUEUEHEAD name = IQUEUE_HEAD_INIT(name)

#define IQUEUE_INIT(ptr) ( \
	(ptr)->next = (ptr), (ptr)->prev = (ptr))

#define IOFFSETOF(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)

#define ICONTAINEROF(ptr, type, member) ( \
		(type*)( ((char*)((type*)ptr)) - IOFFSETOF(type, member)) )

#define IQUEUE_ENTRY(ptr, type, member) ICONTAINEROF(ptr, type, member)


//---------------------------------------------------------------------
// queue operation
//---------------------------------------------------------------------

#define IQUEUE_ADD(node, head) ( \
	(node)->prev = (head), (node)->next = (head)->next, \
	(head)->next->prev = (node), (head)->next = (node))

#define IQUEUE_ADD_TAIL(node, head) ( \
	(node)->prev = (head)->prev, (node)->next = (head), \
	(head)->prev->next = (node), (head)->prev = (node))

#define IQUEUE_DEL_BETWEEN(p, n) ((n)->prev = (p), (p)->next = (n))

#define IQUEUE_DEL(entry) (\
	(entry)->next->prev = (entry)->prev, \
	(entry)->prev->next = (entry)->next, \
	(entry)->next = 0, (entry)->prev = 0)

#define IQUEUE_DEL_INIT(entry) do { \
	IQUEUE_DEL(entry); IQUEUE_INIT(entry); } while (0)

#define IQUEUE_IS_EMPTY(entry) ((entry) == (entry)->next)

#define iqueue_init		IQUEUE_INIT
#define iqueue_entry	IQUEUE_ENTRY
#define iqueue_add		IQUEUE_ADD
#define iqueue_add_tail	IQUEUE_ADD_TAIL
#define iqueue_del		IQUEUE_DEL
#define iqueue_del_init	IQUEUE_DEL_INIT
#define iqueue_is_empty IQUEUE_IS_EMPTY

#define IQUEUE_FOREACH(iterator, head, TYPE, MEMBER) \
	for ((iterator) = iqueue_entry((head)->next, TYPE, MEMBER); \
		&((iterator)->MEMBER) != (head); \
		(iterator) = iqueue_entry((iterator)->MEMBER.next, TYPE, MEMBER))

// iqueue_foreach can't work with iqueue_del !!!
#define iqueue_foreach(iterator, head, TYPE, MEMBER) \
	IQUEUE_FOREACH(iterator, head, TYPE, MEMBER)

#define IQUEUE_FOREACH_REVERSE(iterator, head, TYPE, MEMBER) \
	for ((iterator) = iqueue_entry((head)->prev, TYPE, MEMBER); \
		&((iterator)->MEMBER) != (head); \
		(iterator) = iqueue_entry((iterator)->MEMBER.prev, TYPE, MEMBER))

#define iqueue_foreach_reverse(iterator, head, TYPE, MEMBER) \
	IQUEUE_FOREACH_REVERSE(iterator, head, TYPE, MEMBER)

#define iqueue_foreach_entry(pos, head) \
	for( (pos) = (head)->next; (pos) != (head) ; (pos) = (pos)->next )

#define __iqueue_splice(list, head) do {	\
		iqueue_head *first = (list)->next, *last = (list)->prev; \
		iqueue_head *at = (head)->next; \
		(first)->prev = (head), (head)->next = (first);		\
		(last)->next = (at), (at)->prev = (last); }	while (0)

#define iqueue_splice(list, head) do { \
	if (!iqueue_is_empty(list)) __iqueue_splice(list, head); } while (0)

#define iqueue_splice_init(list, head) do {	\
	iqueue_splice(list, head);	iqueue_init(list); } while (0)



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

typedef enum {
    INITED,
    RELEASED,
} State_t;

typedef struct {
    iqueue_head src_queue;
    pthread_mutex_t mlock;

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

    State_t state;

    pthread_t tid;
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
    pthread_mutex_t mlock;

    int sock;

    struct sockaddr_in RemoteAddr;
    socklen_t RemoteAddrLen;

    State_t state;

    pthread_t tid;
} Receiver;

void TokenBucketInit(TokenBucket *tb, double rate);

void PutToken(TokenBucket *tb);

bool GetToken(TokenBucket *tb, size_t need);

Transmitter *Transmitter_Init(uint32_t maxsymbols, uint32_t maxsymbolsize);

void Transmitter_Release(Transmitter *tx);

Receiver * Receiver_Init(uint32_t maxsymbols, uint32_t maxsymbolsize);

void Receiver_Release(Receiver *rx);

ssize_t Send(Transmitter *tx, void *buf, size_t buflen);

ssize_t Recv(Receiver *rx, void *buf, size_t buflen);

#endif //LLRTP_COMMON_H