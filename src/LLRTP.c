//
// Created by Sai Jiang on 17/11/3.
//
#include "LLRTP.h"

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

    return rval;
}
