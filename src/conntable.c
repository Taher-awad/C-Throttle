#include "common.h"

/* ────────────────────────── CONNECTION TABLE ─────────────────────────────── */

void ConnTable_Set(UINT16 port, UINT32 pid, int app_idx) {
    AcquireSRWLockExclusive(&g_conn_lock);
    UINT32 slot = port % MAX_CONN_TABLE;
    /* linear probe */
    for (int i = 0; i < 16; i++) {
        if (!g_conn_table[slot].valid || g_conn_table[slot].local_port == port) {
            g_conn_table[slot].local_port = port;
            g_conn_table[slot].pid = pid;
            g_conn_table[slot].app_idx = app_idx;
            g_conn_table[slot].valid = TRUE;
            ReleaseSRWLockExclusive(&g_conn_lock);
            return;
        }
        slot = (slot + 1) % MAX_CONN_TABLE;
    }
    ReleaseSRWLockExclusive(&g_conn_lock);
}

int ConnTable_Get(UINT16 port, UINT32 *out_pid) {
    int result = -1;
    if (out_pid) *out_pid = 0;
    AcquireSRWLockShared(&g_conn_lock);
    UINT32 slot = port % MAX_CONN_TABLE;
    for (int i = 0; i < 16; i++) {
        if (g_conn_table[slot].valid && g_conn_table[slot].local_port == port) {
            result = g_conn_table[slot].app_idx;
            if (out_pid) *out_pid = g_conn_table[slot].pid;
            break;
        }
        if (!g_conn_table[slot].valid) break;
        slot = (slot + 1) % MAX_CONN_TABLE;
    }
    ReleaseSRWLockShared(&g_conn_lock);
    return result;
}

void ConnTable_Clear(UINT16 port) {
    AcquireSRWLockExclusive(&g_conn_lock);
    UINT32 slot = port % MAX_CONN_TABLE;
    for (int i = 0; i < 16; i++) {
        if (g_conn_table[slot].valid && g_conn_table[slot].local_port == port) {
            g_conn_table[slot].valid = FALSE;
            break;
        }
        slot = (slot + 1) % MAX_CONN_TABLE;
    }
    ReleaseSRWLockExclusive(&g_conn_lock);
}

void ConnTable_UpdateAppIdxByName(const char *name, int new_app_idx) {
    /* 1. Find all PIDs matching the name from g_global_stats */
    UINT32 matching_pids[256];
    int num_pids = 0;
    
    AcquireSRWLockShared(&g_global_stats_lock);
    for (int i = 0; i < MAX_GLOBAL_PROCS; i++) {
        if (g_global_stats[i].pid != 0 && strcmp(g_global_stats[i].name, name) == 0) {
            if (num_pids < 256) matching_pids[num_pids++] = g_global_stats[i].pid;
        }
    }
    ReleaseSRWLockShared(&g_global_stats_lock);
    
    if (num_pids == 0) return;
    
    /* 2. Update all matching connections */
    AcquireSRWLockExclusive(&g_conn_lock);
    for (int i = 0; i < MAX_CONN_TABLE; i++) {
        if (g_conn_table[i].valid) {
            for (int j = 0; j < num_pids; j++) {
                if (g_conn_table[i].pid == matching_pids[j]) {
                    g_conn_table[i].app_idx = new_app_idx;
                    break;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_conn_lock);
}

/* ────────────────────────── PRE-EXISTING CONNS ───────────────────────────── */

void PopulateExistingConnections(void) {
    DWORD size = 0;
    
    /* IPv4 TCP */
    GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size > 0) {
        PMIB_TCPTABLE_OWNER_PID table = (PMIB_TCPTABLE_OWNER_PID)malloc(size);
        if (table && GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                char exeName[64] = {0};
                if (PidToName(table->table[i].dwOwningPid, exeName, (DWORD)sizeof(exeName))) {
                    int app_idx = FindBucketByName(exeName);
                    UINT16 port = ntohs((UINT16)table->table[i].dwLocalPort);
                    ConnTable_Set(port, table->table[i].dwOwningPid, app_idx);
                    if (app_idx >= 0) AddLog("[SYS] Found pre-existing TCPv4 port %u for %s", port, exeName);
                }
            }
        }
        if (table) free(table);
    }

    /* IPv6 TCP */
    size = 0;
    GetExtendedTcpTable(NULL, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size > 0) {
        PMIB_TCP6TABLE_OWNER_PID table = (PMIB_TCP6TABLE_OWNER_PID)malloc(size);
        if (table && GetExtendedTcpTable(table, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                char exeName[64] = {0};
                if (PidToName(table->table[i].dwOwningPid, exeName, (DWORD)sizeof(exeName))) {
                    int app_idx = FindBucketByName(exeName);
                    UINT16 port = ntohs((UINT16)table->table[i].dwLocalPort);
                    ConnTable_Set(port, table->table[i].dwOwningPid, app_idx);
                    if (app_idx >= 0) AddLog("[SYS] Found pre-existing TCPv6 port %u for %s", port, exeName);
                }
            }
        }
        if (table) free(table);
    }

    /* IPv4 UDP */
    size = 0;
    GetExtendedUdpTable(NULL, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (size > 0) {
        PMIB_UDPTABLE_OWNER_PID table = (PMIB_UDPTABLE_OWNER_PID)malloc(size);
        if (table && GetExtendedUdpTable(table, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                char exeName[64] = {0};
                if (PidToName(table->table[i].dwOwningPid, exeName, (DWORD)sizeof(exeName))) {
                    int app_idx = FindBucketByName(exeName);
                    UINT16 port = ntohs((UINT16)table->table[i].dwLocalPort);
                    ConnTable_Set(port, table->table[i].dwOwningPid, app_idx);
                    if (app_idx >= 0) AddLog("[SYS] Found pre-existing UDPv4 port %u for %s", port, exeName);
                }
            }
        }
        if (table) free(table);
    }

    /* IPv6 UDP */
    size = 0;
    GetExtendedUdpTable(NULL, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
    if (size > 0) {
        PMIB_UDP6TABLE_OWNER_PID table = (PMIB_UDP6TABLE_OWNER_PID)malloc(size);
        if (table && GetExtendedUdpTable(table, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                char exeName[64] = {0};
                if (PidToName(table->table[i].dwOwningPid, exeName, (DWORD)sizeof(exeName))) {
                    int app_idx = FindBucketByName(exeName);
                    UINT16 port = ntohs((UINT16)table->table[i].dwLocalPort);
                    ConnTable_Set(port, table->table[i].dwOwningPid, app_idx);
                    if (app_idx >= 0) AddLog("[SYS] Found pre-existing UDPv6 port %u for %s", port, exeName);
                }
            }
        }
        if (table) free(table);
    }
}
