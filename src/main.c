#include "common.h"

/* ──────────────────────────── GLOBALS (definitions) ───────────────────────── */

LONGLONG g_limit_bytes_per_sec = 524288;
const char *DEFAULT_TARGETS[] = {
    NULL
};

AppBucket    g_buckets[MAX_TARGETS];
int          g_bucket_count = 0;

GlobalProcessStat g_global_stats[MAX_GLOBAL_PROCS];
SRWLOCK g_global_stats_lock;

ConnEntry    g_conn_table[MAX_CONN_TABLE];
SRWLOCK g_conn_lock;

HANDLE g_flow_handle = INVALID_HANDLE_VALUE;
HANDLE g_net_handle  = INVALID_HANDLE_VALUE;

volatile BOOL g_running = TRUE;
volatile BOOL g_engine_paused = FALSE;
volatile BOOL g_test_active = FALSE;
int           g_test_ticks = 0;
FILE*         g_test_file = NULL;

char g_logs[MAX_LOGS][128];
int  g_log_idx = 0;
CRITICAL_SECTION g_log_lock;

/* ──────────────────────────────── MAIN ───────────────────────────────────── */

int main(int argc, char *argv[]) {
    printf("==============================================\n");
    printf("  BrowserThrottle v1.0\n");
    printf("  Per-app bandwidth limiter via WinDivert\n");
    printf("==============================================\n\n");

    /* Parse optional limit argument (KB/s) */
    if (argc >= 2) {
        LONGLONG kbps = (LONGLONG)atoll(argv[1]);
        if (kbps > 0) {
            g_limit_bytes_per_sec = kbps * 1024;
            printf("[*] Limit set to %lld KB/s (%lld MB/s) per app\n",
                kbps, kbps / 1024);
        }
    } else {
        printf("[*] Default limit: 512 KB/s (0.5 MB/s) per app\n");
        printf("[*] Usage: throttle.exe [limit_kbps]\n");
    }

    /* Setup console for ANSI colors */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    /* Init logs lock */
    InitializeCriticalSection(&g_log_lock);
    memset(g_logs, 0, sizeof(g_logs));

    /* Init connection table lock */
    InitializeSRWLock(&g_conn_lock);
    memset(g_conn_table, 0, sizeof(g_conn_table));

    /* Init global stats */
    InitializeSRWLock(&g_global_stats_lock);
    memset(g_global_stats, 0, sizeof(g_global_stats));

    /* Init app buckets */
    LONGLONG now_us = GetMicroseconds();
    for (int i = 0; DEFAULT_TARGETS[i] && i < MAX_TARGETS; i++) {
        AppBucket *b = &g_buckets[i];
        strncpy_s(b->name, sizeof(b->name), DEFAULT_TARGETS[i], _TRUNCATE);
        b->active = TRUE;
        b->limit        = g_limit_bytes_per_sec;
        b->tokens       = (double)(g_limit_bytes_per_sec * BUCKET_BURST_MULT);
        b->last_tick_us = now_us;
        b->total_bytes  = 0;
        b->dropped_bytes = 0;
        b->interval_bytes = 0;
        b->smoothed_speed = 0.0;
        b->test_start_bytes = 0;
        b->sec_bytes = 0;
        b->dropped_packets = 0;
        b->processed_packets = 0;
        b->total_latency_us = 0;
        b->q_head = 0;
        b->q_tail = 0;
        b->q_count = 0;
        for (int j = 0; j < MAX_QUEUE_SIZE; j++) {
            b->queue[j].data = malloc(65536);
        }
        b->q_semaphore = CreateSemaphore(NULL, 0, MAX_QUEUE_SIZE, NULL);
        InitializeCriticalSection(&b->lock);
        g_bucket_count++;
        printf("[*] Target: %s  →  %lld KB/s\n", b->name, b->limit / 1024);
    }
    printf("\n");

    printf("[*] Scanning OS for pre-existing connections...\n");
    PopulateExistingConnections();

    /* ── Open FLOW layer handle ── */
    g_flow_handle = WinDivertOpen(
        "tcp or udp",
        WINDIVERT_LAYER_FLOW,
        100,
        WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY
    );
    if (g_flow_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[-] WinDivertOpen (FLOW) failed: %lu\n", GetLastError());
        fprintf(stderr, "    → Run as Administrator\n");
        return 1;
    }
    printf("[+] FLOW layer opened\n");

    /* ── Open NETWORK layer handle (for packet interception) ── */
    g_net_handle = WinDivertOpen(
        "inbound and (tcp.PayloadLength > 0 or udp.PayloadLength > 0)",
        WINDIVERT_LAYER_NETWORK,
        0,
        0
    );
    if (g_net_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[-] WinDivertOpen (NETWORK) failed: %lu\n", GetLastError());
        WinDivertClose(g_flow_handle);
        return 1;
    }
    printf("[+] NETWORK layer opened\n");

    printf("\n[*] Intercepting packets natively (TCP + UDP)... (Ctrl+C to stop)\n\n");

    /* ── Spin up worker threads ── */
    HANDLE threads[64]; // Max threads we might need
    int t_idx = 0;
    
    threads[t_idx++] = CreateThread(NULL, 0, FlowThread,    NULL, 0, NULL);
    threads[t_idx++] = CreateThread(NULL, 0, StatsThread,   NULL, 0, NULL);
    threads[t_idx++] = CreateThread(NULL, 0, DispatcherThread, NULL, 0, NULL);
    
    for (int i = 0; i < g_bucket_count; i++) {
        g_buckets[i].hThread = CreateThread(NULL, 0, AppWorkerThread, &g_buckets[i], 0, NULL);
        threads[t_idx++] = g_buckets[i].hThread;
    }

    extern void RunGUI(void);
    RunGUI();

    // RunGUI blocks until the window is closed
    printf("[SYS] GUI Closed. Shutting down...\n");

    /* Cleanup */
    g_running = FALSE;
    if (g_flow_handle != INVALID_HANDLE_VALUE) WinDivertClose(g_flow_handle);
    if (g_net_handle  != INVALID_HANDLE_VALUE) WinDivertClose(g_net_handle);

    for (int i = 0; i < g_bucket_count; i++) {
        DeleteCriticalSection(&g_buckets[i].lock);
        for (int j = 0; j < MAX_QUEUE_SIZE; j++) {
            free(g_buckets[i].queue[j].data);
        }
    }
    DeleteCriticalSection(&g_log_lock);

    WaitForMultipleObjects(t_idx, threads, TRUE, 5000);

    return 0;
}

int AddDynamicBucket(const char *name, LONGLONG limit_bytes) {
    int idx = FindBucketByName(name);
    if (idx >= 0) {
        // Just update limit if already exists
        g_buckets[idx].limit = limit_bytes;
        g_buckets[idx].active = TRUE;
        ConnTable_UpdateAppIdxByName(name, idx);
        return idx;
    }
    
    if (g_bucket_count >= MAX_TARGETS) return -1;
    
    AppBucket *b = &g_buckets[g_bucket_count];
    memset(b, 0, sizeof(AppBucket));
    strncpy_s(b->name, sizeof(b->name), name, _TRUNCATE);
    b->active = TRUE;
    b->limit = limit_bytes;
    b->tokens = (double)(limit_bytes * BUCKET_BURST_MULT);
    b->last_tick_us = GetMicroseconds();
    
    for (int j = 0; j < MAX_QUEUE_SIZE; j++) {
        b->queue[j].data = malloc(65536);
    }
    b->q_semaphore = CreateSemaphore(NULL, 0, MAX_QUEUE_SIZE, NULL);
    InitializeCriticalSection(&b->lock);
    
    b->hThread = CreateThread(NULL, 0, AppWorkerThread, b, 0, NULL);
    
    int new_idx = g_bucket_count++;
    
    // Update global stats flag
    AcquireSRWLockExclusive(&g_global_stats_lock);
    for (int i = 0; i < MAX_GLOBAL_PROCS; i++) {
        if (g_global_stats[i].pid != 0 && strcmp(g_global_stats[i].name, name) == 0) {
            g_global_stats[i].is_targeted = TRUE;
            g_global_stats[i].bucket_idx = new_idx;
        }
    }
    ReleaseSRWLockExclusive(&g_global_stats_lock);
    
    ConnTable_UpdateAppIdxByName(name, new_idx);
    
    return new_idx;
}

void RemoveDynamicBucket(const char *name) {
    int idx = FindBucketByName(name);
    if (idx >= 0) {
        g_buckets[idx].active = FALSE;
        
        AcquireSRWLockExclusive(&g_global_stats_lock);
        for (int i = 0; i < MAX_GLOBAL_PROCS; i++) {
            if (g_global_stats[i].pid != 0 && strcmp(g_global_stats[i].name, name) == 0) {
                g_global_stats[i].is_targeted = FALSE;
                g_global_stats[i].bucket_idx = -1;
            }
        }
        ReleaseSRWLockExclusive(&g_global_stats_lock);
        
        ConnTable_UpdateAppIdxByName(name, -1);
    }
}
