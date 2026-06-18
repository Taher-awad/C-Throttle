#include "common.h"

/* ──────────────────────────── FLOW THREAD ────────────────────────────────── */

DWORD WINAPI FlowThread(LPVOID param) {
    (void)param;
    printf("[FLOW] Thread started\n");

    WINDIVERT_ADDRESS addr;
    BYTE dummy[1];
    UINT dummyLen;

    while (g_running) {
        if (!WinDivertRecv(g_flow_handle, dummy, sizeof(dummy), &dummyLen, &addr)) {
            if (!g_running) break;
            continue;
        }

        UINT16 local_port = addr.Outbound ? addr.Flow.LocalPort : addr.Flow.RemotePort;
        
        if (addr.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED) {
            DWORD pid = addr.Flow.ProcessId;
            char exeName[64] = {0};
            if (PidToName(pid, exeName, sizeof(exeName))) {
                int app_idx = FindBucketByName(exeName);
                ConnTable_Set(local_port, pid, app_idx);
                if (app_idx >= 0) {
                    AddLog("[FLOW] + %s  port %u", exeName, local_port);
                }
            }
        } else if (addr.Event == WINDIVERT_EVENT_FLOW_DELETED) {
            ConnTable_Clear(local_port);
        }
    }

    printf("[FLOW] Thread exiting\n");
    return 0;
}

/* ─────────────────────────── DISPATCHER THREAD ───────────────────────────── */

DWORD WINAPI DispatcherThread(LPVOID param) {
    (void)param;
    printf("[DISP] Thread started\n");

    BYTE packet[65535];
    UINT packetLen;
    WINDIVERT_ADDRESS addr;

    WINDIVERT_IPHDR *ipHdr = NULL;
    WINDIVERT_IPV6HDR *ip6Hdr = NULL;
    WINDIVERT_TCPHDR *tcpHdr = NULL;
    WINDIVERT_UDPHDR *udpHdr = NULL;

    while (g_running) {
        if (!WinDivertRecv(g_net_handle, packet, sizeof(packet), &packetLen, &addr)) {
            if (!g_running) break;
            continue;
        }

        if (addr.Outbound) {
            WinDivertSend(g_net_handle, packet, packetLen, NULL, &addr);
            continue;
        }

        WinDivertHelperParsePacket(
            packet, packetLen,
            &ipHdr, &ip6Hdr,
            NULL, NULL, NULL,
            &tcpHdr,
            &udpHdr,
            NULL, NULL, NULL, NULL
        );

        UINT16 local_port = 0;
        if (tcpHdr) local_port = ntohs(tcpHdr->DstPort);
        else if (udpHdr) local_port = ntohs(udpHdr->DstPort);

        if (local_port == 0) {
            WinDivertSend(g_net_handle, packet, packetLen, NULL, &addr);
            continue;
        }

        UINT32 pid = 0;
        int app_idx = ConnTable_Get(local_port, &pid);
        
        if (pid != 0) {
            GlobalStats_AddBytes(pid, packetLen);
        }

        if (app_idx < 0 || g_engine_paused || !g_buckets[app_idx].active) {
            WinDivertSend(g_net_handle, packet, packetLen, NULL, &addr);
            continue;
        }

        AppBucket *b = &g_buckets[app_idx];
        
        EnterCriticalSection(&b->lock);
        if (b->q_count < MAX_QUEUE_SIZE) {
            memcpy(b->queue[b->q_tail].data, packet, packetLen);
            b->queue[b->q_tail].len = packetLen;
            b->queue[b->q_tail].addr = addr;
            b->queue[b->q_tail].arrival_us = GetMicroseconds();
            b->q_tail = (b->q_tail + 1) % MAX_QUEUE_SIZE;
            b->q_count++;
            LeaveCriticalSection(&b->lock);
            ReleaseSemaphore(b->q_semaphore, 1, NULL);
        } else {
            /* Queue full! Drop packet to enforce bufferbloat protection.
               This forces TCP/QUIC to natively slow down! */
            b->dropped_bytes += packetLen;
            b->dropped_packets++;
            LeaveCriticalSection(&b->lock);
        }
    }

    printf("[DISP] Thread exiting\n");
    return 0;
}

/* ────────────────────────── APP WORKER THREAD ────────────────────────────── */

DWORD WINAPI AppWorkerThread(LPVOID param) {
    AppBucket *b = (AppBucket *)param;
    printf("[WORK] Thread started for %s\n", b->name);

    while (g_running) {
        if (WaitForSingleObject(b->q_semaphore, 50) == WAIT_OBJECT_0) {
            if (!g_running) break;

            EnterCriticalSection(&b->lock);
            BYTE *pktData = b->queue[b->q_head].data;
            UINT pktLen = b->queue[b->q_head].len;
            WINDIVERT_ADDRESS addr = b->queue[b->q_head].addr;
            LONGLONG arrival = b->queue[b->q_head].arrival_us;
            b->q_head = (b->q_head + 1) % MAX_QUEUE_SIZE;
            b->q_count--;
            LeaveCriticalSection(&b->lock);

            DWORD delay_ms = Bucket_Consume(b, pktLen);

            if (delay_ms > 0) {
                Sleep(delay_ms);
                EnterCriticalSection(&b->lock);
                Bucket_Refill(b);
                b->tokens -= (double)pktLen;
                b->total_bytes += pktLen;
                b->interval_bytes += pktLen;
                LeaveCriticalSection(&b->lock);
            }

            WinDivertSend(g_net_handle, pktData, pktLen, NULL, &addr);
            
            LONGLONG send_time = GetMicroseconds();
            LONGLONG latency = send_time - arrival;
            EnterCriticalSection(&b->lock);
            b->total_latency_us += latency;
            if (latency > b->max_latency_us) {
                b->max_latency_us = latency;
            }
            b->processed_packets++;
            LeaveCriticalSection(&b->lock);
        }
    }

    printf("[WORK] Thread exiting for %s\n", b->name);
    return 0;
}

/* ──────────────────────────── STATS THREAD ───────────────────────────────── */

DWORD WINAPI StatsThread(LPVOID param) {
    (void)param;
    printf("[STAT] Thread started\n");

    int loops = 0;
    while (g_running) {
        Sleep(100);
        loops++;
        if (loops % 5 == 0) { // Every 500ms
            GlobalStats_Tick();
        }

        printf("\033[H");
        
        if (g_engine_paused) {
            printf("\n   *** ENGINE PAUSED ***   \n\n");
        } else if (g_test_active) {
            printf("\n   [ TEST: %.1fs ]   \n\n", (g_test_ticks * 0.1));
        } else {
            printf("\n");
        }

        printf("%-15s %-12s %-16s %-15s\n", "Target", "Limit", "Current Speed", "Total Passed");
        printf("-----------------------------------------------------------------\n");
        
        char tick_log[256] = {0};
        int tick_log_len = 0;
        if (g_test_active && g_test_file) {
            double elapsed_s = (100 - g_test_ticks + 1) * 0.1;
            tick_log_len += snprintf(tick_log + tick_log_len, sizeof(tick_log) - tick_log_len, "[%.1fs]", elapsed_s);
        }

        for (int i = 0; i < g_bucket_count; i++) {
            AppBucket *b = &g_buckets[i];
            EnterCriticalSection(&b->lock);
            LONGLONG total = b->total_bytes;
            
            double current_kbps = (b->interval_bytes * 10.0) / 1024.0;
            
            if (g_test_active && g_test_file) {
                b->sec_bytes += b->interval_bytes;
                tick_log_len += snprintf(tick_log + tick_log_len, sizeof(tick_log) - tick_log_len, " %s: %.1f KB/s%s",
                    b->name, current_kbps, i == g_bucket_count - 1 ? "" : " |");
            }

            b->smoothed_speed = (b->smoothed_speed * 0.8) + (current_kbps * 0.2);
            double display_speed = b->smoothed_speed;
            if (display_speed < 0.1) display_speed = 0.0;
            b->interval_bytes = 0;
            
            LeaveCriticalSection(&b->lock);
            
            printf("%-15s %-8lld KB/s %-11.1f KB/s %-8lld MB\n",
                b->name,
                b->limit / 1024,
                display_speed,
                total / (1024 * 1024)
            );
        }
        
        if (g_test_active && g_test_file) {
            fprintf(g_test_file, "%s\n", tick_log);
            if ((100 - g_test_ticks + 1) % 10 == 0) {
                int sec = (100 - g_test_ticks + 1) / 10;
                fprintf(g_test_file, "> Sec %d Average:", sec);
                for (int i = 0; i < g_bucket_count; i++) {
                    AppBucket *b = &g_buckets[i];
                    EnterCriticalSection(&b->lock);
                    double sec_kbps = (b->sec_bytes * 1.0) / 1024.0;
                    b->sec_bytes = 0;
                    LeaveCriticalSection(&b->lock);
                    fprintf(g_test_file, " %s: %.1f KB/s%s", b->name, sec_kbps, i == g_bucket_count - 1 ? "" : " |");
                }
                fprintf(g_test_file, "\n--------------------------------------------------\n");
                fflush(g_test_file);
            }
        }

        printf("-----------------------------------------------------------------\n");
        printf("Controls: [SPACE] Pause/Resume | [T] 10s Speed Test | [Q] Quit\n\n");
        
        if (g_test_active) {
            g_test_ticks--;
            if (g_test_ticks <= 0) {
                g_test_active = FALSE;
                AddLog(">>> [SYS] 10s Test Complete! Results:");
                if (g_test_file) {
                    fprintf(g_test_file, "==================================================\n");
                    fprintf(g_test_file, "Total 10s Average:\n");
                }
                for (int i = 0; i < g_bucket_count; i++) {
                    AppBucket *b = &g_buckets[i];
                    EnterCriticalSection(&b->lock);
                    LONGLONG transferred = b->total_bytes - b->test_start_bytes;
                    LONGLONG drops_pkts = b->dropped_packets - b->test_start_dropped_packets;
                    LONGLONG drops_bytes = b->dropped_bytes - b->test_start_dropped_bytes;
                    LONGLONG latency = b->total_latency_us - b->test_start_latency_us;
                    LONGLONG processed = b->processed_packets - b->test_start_processed_packets;
                    LONGLONG max_latency = b->max_latency_us;
                    LeaveCriticalSection(&b->lock);
                    
                    double avg_kbps = (transferred / 10.0) / 1024.0;
                    double drops_kb = drops_bytes / 1024.0;
                    double avg_latency_ms = processed > 0 ? (double)latency / processed / 1000.0 : 0.0;
                    double max_latency_ms = max_latency / 1000.0;
                    double drop_percent = processed > 0 ? ((double)drops_pkts / (processed + drops_pkts)) * 100.0 : 0.0;
                    
                    AddLog("  - %s: Avg %.1f KB/s | Drop: %lld pkts (%.1f%%) | Latency: Avg %.2f ms, Max %.2f ms", 
                        b->name, avg_kbps, drops_pkts, drop_percent, avg_latency_ms, max_latency_ms);
                        
                    if (g_test_file) {
                        fprintf(g_test_file, "  - %s: %.1f KB/s | Dropped: %lld pkts (%.1f%%) | Avg Latency: %.2f ms | Max Latency: %.2f ms\n", 
                            b->name, avg_kbps, drops_pkts, drop_percent, avg_latency_ms, max_latency_ms);
                    }
                }
                
                if (g_test_file) {
                    fprintf(g_test_file, "\n--- Global Process Telemetry (Unthrottled Apps) ---\n");
                    AcquireSRWLockShared(&g_global_stats_lock);
                    for (int i = 0; i < MAX_GLOBAL_PROCS; i++) {
                        if (g_global_stats[i].pid != 0) {
                            LONGLONG global_transferred = g_global_stats[i].test_total_bytes;
                            if (global_transferred > 0) {
                                double global_avg_kbps = (global_transferred / 10.0) / 1024.0;
                                fprintf(g_test_file, "  * %s (PID: %u): %.1f KB/s total avg\n",
                                    g_global_stats[i].name, g_global_stats[i].pid, global_avg_kbps);
                            }
                        }
                    }
                    ReleaseSRWLockShared(&g_global_stats_lock);
                }
                if (g_test_file) {
                    fclose(g_test_file);
                    g_test_file = NULL;
                }
            }
        }

        printf("Recent Logs:\n");
        EnterCriticalSection(&g_log_lock);
        for (int i = 0; i < MAX_LOGS; i++) {
            int idx = (g_log_idx + i) % MAX_LOGS;
            if (g_logs[idx][0] != '\0') {
                printf("  %s\n", g_logs[idx]);
            }
        }
        LeaveCriticalSection(&g_log_lock);
    }

    printf("[STAT] Thread exiting\n");
    return 0;
}

void StartSpeedTest(void) {
    if (g_test_active) return;
    
    g_test_active = TRUE;
    g_test_ticks = 100; /* 10s at 100ms per tick */
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    char filename[128];
    snprintf(filename, sizeof(filename), "speedtest_%04d-%02d-%02d_%02d-%02d-%02d.txt",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    
    fopen_s(&g_test_file, filename, "w");
    if (g_test_file) {
        fprintf(g_test_file, "PacketPolice 10s Speed Test\n");
        fprintf(g_test_file, "Date: %04d-%02d-%02d %02d:%02d:%02d\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(g_test_file, "==================================================\n");
    }
    
    AddLog("[SYS] Started 10s Test... Saving to %s", filename);
    
    for (int i = 0; i < g_bucket_count; i++) {
        AppBucket *b = &g_buckets[i];
        EnterCriticalSection(&b->lock);
        b->test_start_bytes = b->total_bytes;
        b->sec_bytes = 0;
        b->test_start_dropped_packets = b->dropped_packets;
        b->test_start_dropped_bytes = b->dropped_bytes;
        b->test_start_latency_us = b->total_latency_us;
        b->test_start_processed_packets = b->processed_packets;
        b->max_latency_us = 0; // Reset max latency tracker for this test
        LeaveCriticalSection(&b->lock);
    }
    
    AcquireSRWLockExclusive(&g_global_stats_lock);
    for (int i = 0; i < MAX_GLOBAL_PROCS; i++) {
        if (g_global_stats[i].pid != 0) {
            g_global_stats[i].test_total_bytes = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_global_stats_lock);
}
