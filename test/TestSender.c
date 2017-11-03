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
    Transmitter *tx = Transmitter_Init(MAXSYMBOL, MAXSYMBOLSIZE);

    TokenBucket tb;
    TokenBucketInit(&tb, 30000); // equals to 1300Bps

    for (uint32_t seq = 0; seq < LOOPCNT; ) {
        if (GetToken(&tb, INTENDEDLEN) == false) {
            usleep(100);
            continue;
        }

        UserData_t ud;
        ud.ts = GetTS();
        ud.seq = seq++;
        memset(ud.buf, ('a' + (ud.seq * 3 / 2) % 26), PADLEN);
        Send(tx, &ud, sizeof(ud));
    }

    Transmitter_Release(tx);
}