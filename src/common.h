#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windivert.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <conio.h>
#include <stdarg.h>
#include <iphlpapi.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "WinDivert.lib")
#pragma comment(lib, "Iphlpapi.lib")

/* ─────────────────────────────── CONFIG ──────────────────────────────────── */

#define MAX_TARGETS         64
#define MAX_CONN_TABLE      8192    /* max concurrent tracked connections      */
#define BUCKET_BURST_MULT   2       /* burst = limit * this (headroom)         */
#define MAX_LOGS            10
#define MAX_QUEUE_SIZE      256     /* max packets queued per app (bufferbloat protection) */

/* ────────────────────────────── STRUCTURES ───────────────────────────────── */

typedef struct {
    char        name[64];           /* exe name, lowercase                    */
    BOOL        active;             /* is this bucket active?                 */
    LONGLONG    limit;              /* bytes/sec for this target              */
    double      tokens;             /* current token bucket level             */
    LONGLONG    last_tick_us;       /* last refill timestamp (microseconds)   */
    LONGLONG    total_bytes;        /* lifetime bytes throttled               */
    LONGLONG    dropped_bytes;      /* bytes delayed/dropped                  */
    LONGLONG    interval_bytes;     /* bytes passed in current interval       */
    double      smoothed_speed;     /* EWMA speed for UI                      */
    LONGLONG    test_start_bytes;   /* bytes passed when test started         */
    LONGLONG    sec_bytes;          /* bytes passed in current second         */
    LONGLONG    dropped_packets;    /* total dropped packets                  */
    LONGLONG    processed_packets;  /* total processed packets                */
    LONGLONG    total_latency_us;   /* cumulative processing latency          */
    
    /* 10s test snapshot fields */
    LONGLONG    test_start_dropped_packets;
    LONGLONG    test_start_dropped_bytes;
    LONGLONG    test_start_latency_us;
    LONGLONG    test_start_processed_packets;
    LONGLONG    max_latency_us;             /* max processing latency seen            */
    LONGLONG    test_start_max_latency_us;  /* max latency at test start              */
    
    CRITICAL_SECTION lock;
    
    /* App-specific Queue */
    struct {
        BYTE *data;
        UINT len;
        WINDIVERT_ADDRESS addr;
        LONGLONG arrival_us;        /* timestamp when intercepted             */
    } queue[MAX_QUEUE_SIZE];
    int q_head;
    int q_tail;
    int q_count;
    HANDLE q_semaphore;             /* Signaled when packets are in queue     */
    HANDLE hThread;                 /* The dedicated worker thread for this app */
} AppBucket;

/* connection table entry: maps local port → app index and pid */
typedef struct {
    UINT16  local_port;
    UINT32  pid;                    /* parent process ID */
    int     app_idx;                /* index into g_buckets[], -1 = not target */
    BOOL    valid;
} ConnEntry;

/* global process tracking */
typedef struct {
    UINT32   pid;
    char     name[64];              /* exe name */
    LONGLONG interval_bytes;        /* bytes passed in current UI interval */
    double   smoothed_kbps;         /* smoothed speed in KB/s */
    BOOL     is_targeted;           /* whether it is being throttled */
    int      bucket_idx;            /* if targeted, which bucket */
    LONGLONG test_total_bytes;      /* cumulative bytes during test */
} GlobalProcessStat;

#define MAX_GLOBAL_PROCS 1024
extern GlobalProcessStat g_global_stats[MAX_GLOBAL_PROCS];
extern SRWLOCK g_global_stats_lock;

/* ──────────────────────────── GLOBALS (extern) ───────────────────────────── */

extern LONGLONG g_limit_bytes_per_sec;
extern const char *DEFAULT_TARGETS[];

extern AppBucket    g_buckets[MAX_TARGETS];
extern int          g_bucket_count;

extern ConnEntry    g_conn_table[MAX_CONN_TABLE];
extern SRWLOCK g_conn_lock;

extern HANDLE g_flow_handle;
extern HANDLE g_net_handle;

extern volatile BOOL g_running;
extern volatile BOOL g_engine_paused;
extern volatile BOOL g_test_active;
extern int           g_test_ticks;
extern FILE*         g_test_file;

extern char g_logs[MAX_LOGS][128];
extern int  g_log_idx;
extern CRITICAL_SECTION g_log_lock;

/* ─────────────────────────── FUNCTION PROTOS ─────────────────────────────── */

/* utils.c */
void AddLog(const char *fmt, ...);
extern void     GlobalStats_AddBytes(UINT32 pid, LONGLONG bytes);
extern void     GlobalStats_Tick(void);
void LowerStr(char *s);
BOOL PidToName(DWORD pid, char *out, DWORD outLen);
int FindBucketByName(const char *name);

/* bucket.c */
void Bucket_Refill(AppBucket *b);
DWORD Bucket_Consume(AppBucket *b, UINT pktLen);

/* conntable.c */
void ConnTable_Set(UINT16 port, UINT32 pid, int app_idx);
int ConnTable_Get(UINT16 port, UINT32 *out_pid);
void ConnTable_UpdateAppIdxByName(const char *name, int new_app_idx);
void ConnTable_Clear(UINT16 port);
void PopulateExistingConnections(void);

/* engine.c */
DWORD WINAPI FlowThread(LPVOID param);
DWORD WINAPI DispatcherThread(LPVOID param);
DWORD WINAPI AppWorkerThread(LPVOID param);
DWORD WINAPI StatsThread(LPVOID param);
void StartSpeedTest(void);
