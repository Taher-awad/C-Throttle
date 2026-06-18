#include "common.h"

/* ─────────────────────────── HELPERS ─────────────────────────────────────── */

void AddLog(const char *fmt, ...) {
    EnterCriticalSection(&g_log_lock);
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_logs[g_log_idx], sizeof(g_logs[g_log_idx]), fmt, args);
    va_end(args);
    g_log_idx = (g_log_idx + 1) % MAX_LOGS;
    LeaveCriticalSection(&g_log_lock);
}

LONGLONG GetMicroseconds(void) {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1000000LL) / freq.QuadPart;
}

void LowerStr(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* Get exe name (lowercase) from PID */
BOOL PidToName(DWORD pid, char *out, DWORD outLen) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    char path[MAX_PATH] = {0};
    DWORD len = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameA(h, 0, path, &len);
    CloseHandle(h);
    if (!ok) return FALSE;
    char *slash = strrchr(path, '\\');
    strncpy_s(out, outLen, slash ? slash + 1 : path, _TRUNCATE);
    LowerStr(out);
    return TRUE;
}

/* Find app bucket index by exe name, -1 if not a target */
int FindBucketByName(const char *name) {
    for (int i = 0; i < g_bucket_count; i++) {
        if (g_buckets[i].active && strcmp(g_buckets[i].name, name) == 0) return i;
    }
    return -1;
}

void GlobalStats_AddBytes(UINT32 pid, LONGLONG bytes) {
    if (pid == 0) return;
    AcquireSRWLockExclusive(&g_global_stats_lock);
    int free_slot = -1;
    for (int i = 0; i < MAX_GLOBAL_PROCS; i++) {
        if (g_global_stats[i].pid == pid) {
            g_global_stats[i].interval_bytes += bytes;
            if (g_test_active) g_global_stats[i].test_total_bytes += bytes;
            ReleaseSRWLockExclusive(&g_global_stats_lock);
            return;
        }
        if (g_global_stats[i].pid == 0 && free_slot == -1) {
            free_slot = i;
        }
    }
    /* New process */
    if (free_slot != -1) {
        g_global_stats[free_slot].pid = pid;
        PidToName(pid, g_global_stats[free_slot].name, sizeof(g_global_stats[free_slot].name));
        g_global_stats[free_slot].interval_bytes = bytes;
        g_global_stats[free_slot].smoothed_kbps = 0;
        int bucket_idx = FindBucketByName(g_global_stats[free_slot].name);
        g_global_stats[free_slot].is_targeted = (bucket_idx >= 0);
        g_global_stats[free_slot].bucket_idx = bucket_idx;
        g_global_stats[free_slot].test_total_bytes = g_test_active ? bytes : 0;
    }
    ReleaseSRWLockExclusive(&g_global_stats_lock);
}

void GlobalStats_Tick(void) {
    /* Called every UI update tick (e.g. 500ms) */
    AcquireSRWLockExclusive(&g_global_stats_lock);
    for (int i = 0; i < MAX_GLOBAL_PROCS; i++) {
        if (g_global_stats[i].pid != 0) {
            double kbps = (double)g_global_stats[i].interval_bytes / 1024.0 * 2.0; /* 500ms = * 2 */
            g_global_stats[i].smoothed_kbps = (g_global_stats[i].smoothed_kbps * 0.5) + (kbps * 0.5);
            g_global_stats[i].interval_bytes = 0;
            
            /* If it's been silent for a while and speed is near 0, we could reap it, 
               but for now just leave it in the table until restart */
        }
    }
    ReleaseSRWLockExclusive(&g_global_stats_lock);
}
