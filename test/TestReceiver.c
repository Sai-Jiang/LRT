//
// Created by Sai Jiang on 17/11/3.
//
#include "LLRTP.h"

typedef struct {
    uint32_t seq;
    long ts;
    uint8_t buf[PADLEN];
} __attribute__((packed)) UserData_t;

int main()
{
    Receiver *rx = Receiver_Init(MAXSYMBOL, MAXSYMBOLSIZE);

    for (uint32_t seq = 0; seq < LOOPCNT; ) {
        UserData_t ud;

        if (Recv(rx, &ud, sizeof(ud)) <= 0) continue;

        seq++;
        printf("[%u]Delay: %ld\n", ud.seq, GetTS() - ud.ts);

        for (int i = 0; i < PADLEN; i++)
            assert(ud.buf[i] == ('a' + (ud.seq * 3 / 2) % 26));
    }

    Receiver_Release(rx);
}