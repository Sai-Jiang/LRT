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

    rx->dec_factory = kodoc_new_decoder_factory(codec, kodoc_binary8,
                                                maxsymbols, maxsymbolsize);
    rx->maxsymbol = maxsymbols;
    rx->maxsymbolsize = maxsymbolsize;
    rx->blksize = rx->maxsymbol * rx->maxsymbolsize;

    rx->payload_size = kodoc_factory_max_payload_size(rx->dec_factory);
    rx->pktbuf = malloc(sizeof(Packet) + rx->payload_size);

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
        ssize_t nbytes = read(rx->DataSock, rx->pktbuf, pktbuflen);
        if (nbytes < 0) break;
        assert(nbytes == sizeof(Packet) + rx->payload_size);

        // Discard the out-of-date packet
        if (rx->pktbuf->id < rx->ExpectedBlockID) break;

        ChainedPkt *cpkt = malloc(sizeof(ChainedPkt));
        cpkt->pkt = malloc(pktbuflen);
        memcpy(cpkt->pkt, rx->pktbuf, pktbuflen);

        // filter out-of-time packet
        for (iqueue_head *p = rx->pkt_queue.next, *nxt; p != &rx->pkt_queue; p = nxt) {
            nxt = p->next;
            ChainedPkt *entry = iqueue_entry(p, ChainedPkt, qnode);

            if (entry->pkt->id >= rx->ExpectedBlockID) break;

            iqueue_is_empty(&entry->qnode);
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

        // figure out whether there is partial decoded symbol
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
}

void ReSym2Src(Receiver *rx)
{
    while (!iqueue_is_empty(&rx->sym_queue)) {
        Symbol *sym = iqueue_entry(rx->sym_queue.next, Symbol, qnode);
        SrcData *sd = malloc(sizeof(SrcData) + rx->maxsymbolsize - 2);
        memcpy(sd->data, sym->data, rx->maxsymbolsize);
        iqueue_add_tail(&sd->qnode, &rx->src_queue);
        iqueue_del(&sym->qnode);
        free(sym);
    }
}

size_t Recv(Receiver *rx, void *buf, size_t buflen)
{
    if (iqueue_is_empty(&rx->src_queue)) return 0;

    SrcData *sd = iqueue_entry(rx->src_queue.next, SrcData, qnode);
    assert(sd->Len >= sizeof(sd->Len) && buflen >= (sd->Len - sizeof(sd->Len)));
    memcpy(buf, sd->rawdata, sd->Len - sizeof(sd->Len));
    iqueue_del(&sd->qnode);
    free(sd);

    return buflen;
}

int main()
{
    Receiver *rx = Receiver_Init(MAXSYMBOL, MAXSYMBOLSIZE);

    uint32_t seq = 0;

    UserData_t ud;

    do {
        CheckPkt(rx);
        MovPkt2Dec(rx);
        ReSym2Src(rx);

        if (Recv(rx, &ud, sizeof(ud)) > 0) {
            printf("[%u]Delay: %ld\n", ud.seq, GetTS() - ud.ts);

            int i;
            for (i = 0; i < PADLEN && ud.buf[i] == ('a' + ud.seq % 26); i++);
            assert(i == PADLEN);
        }
    } while (seq < LOOPCNT ||
            !iqueue_is_empty(&rx->pkt_queue) ||
            !iqueue_is_empty(&rx->dec_queue) ||
            !iqueue_is_empty(&rx->sym_queue) ||
            !iqueue_is_empty(&rx->src_queue));

    Receiver_Release(rx);
}