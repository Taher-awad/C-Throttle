#include "common.h"

/* ────────────────────────── TOKEN BUCKET ─────────────────────────────────── */

/*
 * Refills the token bucket based on time elapsed since last refill.
 * Cap tokens at BUCKET_BURST_MULT * limit.
 */
void Bucket_Refill(AppBucket *b) {
    LONGLONG now_us  = GetMicroseconds();
    double elapsed_s = (now_us - b->last_tick_us) / 1e6;
    b->tokens       += elapsed_s * (double)b->limit;
    double max_tok   = (double)(b->limit * BUCKET_BURST_MULT);
    if (b->tokens > max_tok) b->tokens = max_tok;
    b->last_tick_us  = now_us;
}

/* Returns delay in ms needed before packet can be sent (0 = send now) */
DWORD Bucket_Consume(AppBucket *b, UINT pktLen) {
    EnterCriticalSection(&b->lock);
    Bucket_Refill(b);

    DWORD delay_ms = 0;
    
    if (b->tokens >= (double)pktLen) {
        b->tokens -= (double)pktLen;
        b->total_bytes += pktLen;
        b->interval_bytes += pktLen;
    } else {
        double deficit = (double)pktLen - b->tokens;
        double wait_s  = deficit / (double)b->limit;
        delay_ms       = (DWORD)(wait_s * 1000.0) + 1;
        b->dropped_bytes += pktLen;
    }

    LeaveCriticalSection(&b->lock);
    return delay_ms;
}
