//
// Created by Sai Jiang on 17/10/22.
//
#include "common.h"

static const int32_t codec = kodoc_on_the_fly;

/*
 * 令牌桶算法
 * 用于限流
 * 配合https://en.wikipedia.org/wiki/Token_bucket服用
 *
 * TokenBucketInit初始化
 * PutToken往桶里倒水
 * GetToken从桶里取水
 */
void TokenBucketInit(TokenBucket *tb, double rate)
{
    tb->ts = GetTS();
    tb->CurCapactiy = 0;
    tb->MaxCapacity = 4096;
    tb->LimitedRate = rate; // Unit: Byte/ms
}

void PutToken(TokenBucket *tb)
{
    if (tb->CurCapactiy >= tb->MaxCapacity) return;
    long Now = GetTS();
    assert(Now >= tb->ts);
    uint32_t reload = (uint32_t)((Now - tb->ts) * tb->LimitedRate);
    assert(reload >= 0);
    if (reload > 0) {
        tb->ts = Now;
        tb->CurCapactiy = min(tb->CurCapactiy + reload, tb->MaxCapacity);
    }
}

bool GetToken(TokenBucket *tb, size_t need)
{
    PutToken(tb);

    bool rval = false;

    if (tb->CurCapactiy >= need) {
        tb->CurCapactiy -= need;
        rval = true;
    }
//    if (need > 1200)
//        printf("GetToken: %s\n", rval ? "True" : "False");

    return rval;
}


Transmitter *Transmitter_Init(uint32_t maxsymbols, uint32_t maxsymbolsize)
{
    assert(maxsymbolsize >= 512);

    Transmitter *tx = malloc(sizeof(Transmitter));
    assert(tx != NULL);

    // 所有的数据结构全部用双向循环链表组织
    iqueue_init(&tx->src_queue);

    iqueue_init(&tx->sym_queue);

    iqueue_init(&tx->enc_queue);

    tx->enc_cnt = 0;

    // 编码器初始化
    tx->enc_factory = kodoc_new_encoder_factory(
            codec, kodoc_binary8, maxsymbols, maxsymbolsize);

    tx->maxsymbol = maxsymbols;
    tx->maxsymbolsize = maxsymbolsize;
    tx->blksize = tx->maxsymbol * tx->maxsymbolsize;

    tx->NextBlockID = 0;

    tx->payload_size = kodoc_factory_max_payload_size(tx->enc_factory);
    tx->pktbuf = malloc(sizeof(Packet) + tx->payload_size);
    assert(tx->payload_size < 1500);

    tx->LossRate = 0.2;

    // 套接字初始化
    tx->sock = socket(PF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(PF_INET, DST_IP, &addr.sin_addr);
    addr.sin_port = htons(DST_DPORT);
    connect(tx->sock, (struct sockaddr *) &addr, sizeof(addr));

    return tx;
}
/*
 * 释放
 */
void Transmitter_Release(Transmitter *tx)
{
    assert(iqueue_is_empty(&tx->src_queue));
    assert(iqueue_is_empty(&tx->sym_queue));
    assert(iqueue_is_empty(&tx->enc_queue));

    kodoc_delete_factory(tx->enc_factory);

    free(tx->pktbuf);

    close(tx->sock);

    free(tx);
}

/*
 * 用户发送数据时调用，类似socket send那个函数
 */
size_t Send(Transmitter *tx, void *buf, size_t buflen)
{
    SrcData *inserted = malloc(sizeof(SrcData) + buflen);
    inserted->Len = sizeof(inserted->Len) + buflen;
    memcpy(inserted->rawdata, buf, buflen);
    iqueue_add_tail(&inserted->qnode, &tx->src_queue);

    return buflen;
}

/*
 * 用于把用户数据切割成一个一个symbol
 */
void Div2Sym(Transmitter *tx)
{
    Symbol *psym;
    void *psrc, *pdst;
    size_t RestSrcLen, RestDstLen;

    psym = pdst = NULL;
    RestDstLen = 0;

    while (!iqueue_is_empty(&tx->src_queue)) {
        SrcData *psd = iqueue_entry(tx->src_queue.next, SrcData, qnode);
        psrc = psd->data;
        RestSrcLen = psd->Len;

        while (RestSrcLen > 0) {
            if (pdst == NULL) {
                psym = malloc(sizeof(Symbol) + tx->maxsymbolsize);
                pdst = psym->data;
                RestDstLen = tx->maxsymbolsize;
                memset(pdst, 0, RestDstLen);
            }

            size_t MaxCopyable = min(RestSrcLen, RestDstLen);
            memcpy(pdst, psrc, MaxCopyable);
            pdst += MaxCopyable; psrc += MaxCopyable;
            RestDstLen -= MaxCopyable; RestSrcLen -= MaxCopyable;

            if (RestDstLen == 0 || RestDstLen == 1) {
                iqueue_add_tail(&psym->qnode, &tx->sym_queue);
                psym = pdst = NULL;
                RestDstLen = 0;
            }
        }

        iqueue_del(&psd->qnode);
        free(psd);
    }

    if (psym != NULL) {
        iqueue_add_tail(&psym->qnode, &tx->sym_queue);
        psym = pdst = NULL;
        RestDstLen = 0;
    }
}

/*
 * 将Symbol添加到编码器中
 */
void MovSym2Enc(Transmitter *tx)
{
    while (!iqueue_is_empty(&tx->sym_queue) && tx->enc_cnt < ENCWNDSZ) {
        EncWrapper *encwrapper = NULL;

        // 如果没有可用的编码器，就初始化一个新的并插入队列末尾；
        if (iqueue_is_empty(&tx->enc_queue) ||
                iqueue_entry(tx->enc_queue.prev, EncWrapper, qnode)->lrank == tx->maxsymbol) {
            encwrapper = malloc(sizeof(EncWrapper));
            encwrapper->enc = kodoc_factory_build_coder(tx->enc_factory);
            encwrapper->lrank = encwrapper->rrank = 0;
            encwrapper->id = tx->NextBlockID++;
            encwrapper->pblk = malloc(tx->blksize);
            encwrapper->nmore = 0;
            TokenBucketInit(&encwrapper->tb, 100); // 5ms Gap
            iqueue_add_tail(&encwrapper->qnode, &tx->enc_queue);
            tx->enc_cnt++;
            debug("enc[%u] init, total %u\n", encwrapper->id, tx->enc_cnt);
        } else {
            encwrapper = iqueue_entry(tx->enc_queue.prev, EncWrapper, qnode);
        }

        assert(encwrapper != NULL);

        // 依次添加symbol到编码器中，立即发送source symbol；视条件发送repair symbol；
        Symbol *sym = NULL;
        for (iqueue_head *p = tx->sym_queue.next, *nxt;
             p != &tx->sym_queue && encwrapper->lrank < tx->maxsymbol; p = nxt) {
            nxt = p->next;
            sym = iqueue_entry(p, Symbol, qnode);

            void *pdst = encwrapper->pblk + encwrapper->lrank * tx->maxsymbolsize;
            memcpy(pdst, sym->data, tx->maxsymbolsize);
            kodoc_set_const_symbol(encwrapper->enc, encwrapper->lrank, pdst, tx->maxsymbolsize);
            encwrapper->lrank = kodoc_rank(encwrapper->enc);

            // send source symbol
            tx->pktbuf->id = encwrapper->id;
            kodoc_write_payload(encwrapper->enc, tx->pktbuf->data);
            send(tx->sock, tx->pktbuf, sizeof(Packet) + tx->payload_size, 0);
            encwrapper->nmore += tx->LossRate;

            // send appending repair symbol
            while (encwrapper->nmore >= 1) {
                tx->pktbuf->id = encwrapper->id;
                kodoc_write_payload(encwrapper->enc, tx->pktbuf->data);
                send(tx->sock, tx->pktbuf, sizeof(Packet) + tx->payload_size, 0);
                encwrapper->nmore -= 1;
                encwrapper->nmore += tx->LossRate;
            }

            iqueue_del(&sym->qnode);
            free(sym);
        }
    }
}
/*
 * 用于接收反馈，更新rank
 */
void CheckACK(Transmitter *tx)
{
    AckMsg msg;

    while (true) {
        ssize_t nbytes = recv(tx->sock, &msg, sizeof(msg), MSG_DONTWAIT);
        if (nbytes < 0) break;
        assert(nbytes == sizeof(msg));

        EncWrapper *encwrapper = NULL;
        iqueue_foreach(encwrapper, &tx->enc_queue, EncWrapper, qnode) {
            if (msg.id > encwrapper->id) continue;
            else if (msg.id < encwrapper->id) break;
            else {
                assert(msg.id == encwrapper->id);
                assert(msg.rank > 0 && msg.rank <= tx->maxsymbol);
                encwrapper->rrank = max(encwrapper->rrank, msg.rank);
//                debug("enc[%u] lrank updated: %u\n", encwrapper->id, encwrapper->lrank);
            }
        }
    }
}

/*
 * 用于发送额外的编码包
 * 依次遍历每一个编码器，条件符合就发送；
 */
void Fountain(Transmitter *tx)
{
    EncWrapper *encwrapper = NULL;
    for (iqueue_head *p = tx->enc_queue.next, *nxt; p != &tx->enc_queue; p = nxt) {
        nxt = p->next;
        encwrapper = iqueue_entry(p, EncWrapper, qnode);

        // free the encoder that finished the job
        if (encwrapper->lrank == tx->maxsymbol && encwrapper->rrank == tx->maxsymbol) {
            tx->enc_cnt--;
            debug("enc[%u] free, total %u\n", encwrapper->id, tx->enc_cnt);
            iqueue_del(&encwrapper->qnode);
            free(encwrapper->pblk);
            kodoc_delete_coder(encwrapper->enc);
            free(encwrapper);
        } else if (GetToken(&encwrapper->tb, sizeof(Packet) + tx->payload_size) &&
                encwrapper->lrank > encwrapper->rrank) {
            tx->pktbuf->id = encwrapper->id;
            kodoc_write_payload(encwrapper->enc, tx->pktbuf->data);
            send(tx->sock, tx->pktbuf, sizeof(Packet) + tx->payload_size, 0);
        }
    }
}

/*
 *
 * Div2Sym()
 * MovSym2Enc()
 * CheckACK()
 * Fountain()
 *
 * 构成一个发送周期
 *
 *
 * 暂时没有handshake啥的。。
 */

int main()
{
    Transmitter *tx = Transmitter_Init(MAXSYMBOL, MAXSYMBOLSIZE);

    TokenBucket tb;
    TokenBucketInit(&tb, 300); // equals to 1300Bps

    uint32_t seq = 0;

    UserData_t ud;

    do {
        while (seq < LOOPCNT && GetToken(&tb, sizeof(ud)))  {
            ud.seq = seq++;
            ud.ts = GetTS();
            memset(ud.buf, 'a' + (ud.seq * 3 / 2) % 26, PADLEN);
            Send(tx, &ud, sizeof(ud));
        }

        Div2Sym(tx);
        MovSym2Enc(tx);
        CheckACK(tx);
        Fountain(tx);

        usleep(20);

    } while (seq < LOOPCNT ||
            !iqueue_is_empty(&tx->src_queue) ||
            !iqueue_is_empty(&tx->sym_queue) ||
            !iqueue_is_empty(&tx->enc_queue));

    Transmitter_Release(tx);
}