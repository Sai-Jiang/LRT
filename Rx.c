//
// Created by Sai Jiang on 17/10/22.
//

#include "common.h"

static const int32_t codec = kodoc_on_the_fly;

Receiver * Receiver_Init(uint32_t maxsymbols, uint32_t maxsymbolsize)
{
    Receiver *rx = malloc(sizeof(Receiver));

    iqueue_init(&rx->pkt_queue);
    iqueue_init(&rx->dec_queue);
    iqueue_init(&rx->sym_queue);
    iqueue_init(&rx->src_queue);

    rx->src_cnt = 0;

    rx->dec_factory = kodoc_new_decoder_factory(codec, kodoc_binary8,
                                                maxsymbols, maxsymbolsize);
    rx->maxsymbol = maxsymbols;
    rx->maxsymbolsize = maxsymbolsize;
    rx->blksize = rx->maxsymbol * rx->maxsymbolsize;

    rx->payload_size = kodoc_factory_max_payload_size(rx->dec_factory);
    rx->pktbuf = malloc(sizeof(Packet) + rx->payload_size);
    assert(rx->pktbuf != NULL);

    rx->ExpectedBlockID = rx->ExpectedSymbolID = 0;

    struct sockaddr_in addr;

    rx->DataSock = socket(PF_INET, SOCK_DGRAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htons(INADDR_ANY);
    addr.sin_port = htons(DST_DPORT);
    assert(bind(rx->DataSock, (struct sockaddr *)&addr, sizeof(addr)) >= 0);

    rx->SignalSock = socket(PF_INET, SOCK_DGRAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(PF_INET, SRC_IP, &addr.sin_addr);
    addr.sin_port = htons(SRC_SPORT);
    connect(rx->SignalSock, (struct sockaddr *)&addr, sizeof(addr));

    int flags = fcntl(rx->DataSock, F_GETFL, 0);
    fcntl(rx->DataSock, F_SETFL, flags | O_NONBLOCK);

    return rx;
}

void Receiver_Release(Receiver *rx)
{
    assert(iqueue_is_empty(&rx->pkt_queue));
    assert(iqueue_is_empty(&rx->dec_queue));
    assert(iqueue_is_empty(&rx->sym_queue));
    assert(iqueue_is_empty(&rx->src_queue));

    close(rx->DataSock);
    close(rx->SignalSock);
    kodoc_delete_factory(rx->dec_factory);
    free(rx->pktbuf);
    free(rx);
}

void CheckPkt(Receiver *rx) {
    size_t pktbuflen = sizeof(Packet) + rx->payload_size;

    long EntTS = GetTS();

    while (GetTS() - EntTS <= 1) {
        ssize_t nbytes = recv(rx->DataSock, rx->pktbuf, pktbuflen, 0);
        if (nbytes < 0) break;
        assert(nbytes == sizeof(Packet) + rx->payload_size);

        // Discard the out-of-date packet & Send full-rank feedback
        if (rx->pktbuf->id < rx->ExpectedBlockID) {
            AckMsg ack;
            ack.id = rx->pktbuf->id;
            ack.rank = rx->maxsymbol;
            send(rx->SignalSock, &ack, sizeof(ack), 0);
            continue;
        }

        ChainedPkt *cpkt = malloc(sizeof(ChainedPkt));
        cpkt->pkt = malloc(pktbuflen);
        memcpy(cpkt->pkt, rx->pktbuf, pktbuflen);

        // filter out-of-time packet
        for (iqueue_head *p = rx->pkt_queue.next, *nxt; p != &rx->pkt_queue; p = nxt) {
            nxt = p->next;
            ChainedPkt *entry = iqueue_entry(p, ChainedPkt, qnode);

            if (entry->pkt->id >= rx->ExpectedBlockID) break;

            iqueue_del(p);
            free(entry->pkt);
            free(entry);
        }

        // insert to right pos at the end
        iqueue_head *p, *prev;
        for (p = rx->pkt_queue.prev; p != &rx->pkt_queue; p = prev) {
            prev = p->prev;
            ChainedPkt *entry = iqueue_entry(p, ChainedPkt, qnode);

            if (cpkt->pkt->id >= entry->pkt->id) break;
        }

        cpkt->qnode.prev = p;
        cpkt->qnode.next = p->next;
        p->next->prev = &cpkt->qnode;
        p->next = &cpkt->qnode;
    }

}

void MovPkt2Dec(Receiver *rx)
{
    while (!iqueue_is_empty(&rx->pkt_queue)) {
        IQUEUE_HEAD(sameid);
        uint32_t id = (iqueue_entry(rx->pkt_queue.next, ChainedPkt, qnode))->pkt->id;
        int npkts = 0;

        // detach a list of packets of the same id, at least one
        for (iqueue_head *p = rx->pkt_queue.next, *nxt; p != &rx->pkt_queue; p = nxt) {
            nxt = p->next;
            ChainedPkt *cpkt = iqueue_entry(p, ChainedPkt, qnode);

            if (cpkt->pkt->id == id) {
                iqueue_del(p);
                iqueue_add_tail(p, &sameid);
                npkts++;
            } else {
                break;
            }
        }

        // find the decoder or find the right inserted positioin
        iqueue_head *pos = NULL;
        iqueue_foreach_entry(pos, &rx->dec_queue) {
            DecWrapper *decwrapper = iqueue_entry(pos, DecWrapper, qnode);
            if (id <= decwrapper->id) break;
        }

        DecWrapper *decwrapper = NULL;
        if (pos == &rx->dec_queue || iqueue_entry(pos, DecWrapper, qnode)->id > id) {
            // allocate a new one
            decwrapper = malloc((sizeof(DecWrapper)));
            decwrapper->id = id;
            decwrapper->dec = kodoc_factory_build_coder(rx->dec_factory);
            decwrapper->pblk = malloc(rx->blksize);
            kodoc_set_mutable_symbols(decwrapper->dec, decwrapper->pblk, rx->blksize);
            // insert into the right pos
            decwrapper->qnode.prev = pos->prev;
            decwrapper->qnode.next = pos;
            pos->prev->next = &decwrapper->qnode;
            pos->prev = &decwrapper->qnode;
        } else {
            decwrapper = iqueue_entry(pos, DecWrapper, qnode);
        }

        assert(decwrapper != NULL && decwrapper->id == id);

        // feed the pkts to the decoder
        ChainedPkt *cpkt = NULL;
        for (iqueue_head *p = sameid.next, *nxt; p != &sameid; p = nxt) {
            nxt = p->next;
            cpkt = iqueue_entry(p, ChainedPkt, qnode);

            if (!kodoc_is_complete(decwrapper->dec))
                kodoc_read_payload(decwrapper->dec, cpkt->pkt->data);

            AckMsg ack;
            ack.id = cpkt->pkt->id;
            ack.rank = kodoc_rank(decwrapper->dec);
            send(rx->SignalSock, &ack, sizeof(AckMsg), 0);

            iqueue_del(p);
            free(cpkt->pkt);
            free(cpkt);
        }
    }
}

void GenSym(Receiver *rx)
{
    if (iqueue_is_empty(&rx->dec_queue)) return;

    DecWrapper *decwrapper = iqueue_entry(rx->dec_queue.next, DecWrapper, qnode);

    if (decwrapper->id == rx->ExpectedBlockID) {
        while (kodoc_is_symbol_uncoded(decwrapper->dec, rx->ExpectedSymbolID)) {
            debug("dec[%u] sym[%u] decoded\n", decwrapper->id, rx->ExpectedSymbolID);

            Symbol *sym = malloc(sizeof(Symbol) + rx->maxsymbolsize);
            void *src = decwrapper->pblk + rx->ExpectedSymbolID * rx->maxsymbolsize;
            memcpy(sym->data, src, rx->maxsymbolsize);
            iqueue_add_tail(&sym->qnode, &rx->sym_queue);

            rx->ExpectedSymbolID++;

            if (rx->ExpectedSymbolID == rx->maxsymbol) {
                rx->ExpectedSymbolID = 0;
                rx->ExpectedBlockID++;
                iqueue_del(&decwrapper->qnode);
                kodoc_delete_coder(decwrapper->dec);
                free(decwrapper->pblk);
                free(decwrapper);
                break;
            }
        }
    }
}

void ReSym2Src(Receiver *rx)
{
    static SrcData *psd = NULL;
    void *psrc;
    size_t RestSrcLen;
    static void *pdst = NULL; // default to NULL by 'static' at startup
    static size_t RestDstLen = 0; // default to zero at startup

    while (!iqueue_is_empty(&rx->sym_queue)) {
        Symbol *psym = iqueue_entry(rx->sym_queue.next, Symbol, qnode);
        psrc = psym->data;
        RestSrcLen = rx->maxsymbolsize;

        while (RestSrcLen >= 2) {
            if (pdst == NULL) {
                RestDstLen = *((uint16_t *)psrc);
                assert(RestDstLen == INTENDEDLEN || RestDstLen == 0);
                if (RestDstLen == 0) break;
                else {
                    psd = malloc(sizeof(SrcData) + RestDstLen - sizeof(uint16_t));
                    pdst = psd->data;
                    memset(pdst, 0, RestDstLen);
                }
            }

            debug("RestSrcLen: %zu, RestDstLen: %zu, MaxCopyable: %lu\n",
                   RestSrcLen, RestDstLen, min(RestDstLen, RestSrcLen));
            size_t MaxCopyable = min(RestSrcLen, RestDstLen);
            memcpy(pdst, psrc, MaxCopyable);
            pdst += MaxCopyable; psrc += MaxCopyable;
            RestDstLen -= MaxCopyable; RestSrcLen -= MaxCopyable;


            if (RestDstLen == 0) {
                assert(psd->Len == INTENDEDLEN);
                debug("Add src, cnt: %u, seq: %u, len: %hu\n", ++rx->src_cnt,
                       *(uint32_t *)(psd->rawdata), psd->Len);
                iqueue_add_tail(&psd->qnode, &rx->src_queue);
                psd = pdst = NULL;
                RestDstLen = 0;
            }
        }

        iqueue_del(&psym->qnode);
        free(psym);
    }
}

int Recv(Receiver *rx, void *buf, size_t buflen)
{
    if (iqueue_is_empty(&rx->src_queue)) return 0;

    SrcData *psd = iqueue_entry(rx->src_queue.next, SrcData, qnode);
    assert(psd->Len >= sizeof(psd->Len));
    assert(buflen == psd->Len - sizeof(psd->Len));
    debug("Del src: %u\n", --rx->src_cnt);
    memcpy(buf, psd->rawdata, psd->Len - sizeof(psd->Len)); // copy rawdata
    iqueue_del(&psd->qnode);
    free(psd);

    return (int)buflen;
}

int main()
{
    Receiver *rx = Receiver_Init(MAXSYMBOL, MAXSYMBOLSIZE);

    uint32_t seq = 0;

    UserData_t ud;

    do {
        CheckPkt(rx);
        MovPkt2Dec(rx);
        GenSym(rx);
        ReSym2Src(rx);

        while (Recv(rx, &ud, sizeof(ud)) > 0) {
            printf("[%u]Delay: %ld\n", ud.seq, GetTS() - ud.ts);

            int i;
            for (i = 0; i < PADLEN && ud.buf[i] == ('a' + (ud.seq * 3 / 2) % 26); i++);
            assert(i == PADLEN);
        }
    } while (seq < LOOPCNT ||
            !iqueue_is_empty(&rx->pkt_queue) ||
            !iqueue_is_empty(&rx->dec_queue) ||
            !iqueue_is_empty(&rx->sym_queue) ||
            !iqueue_is_empty(&rx->src_queue));

    Receiver_Release(rx);
}