#include "common.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "Comctl32.lib")

#define ID_LISTVIEW     1001
#define ID_BTN_LIMIT    1002
#define ID_BTN_UNLIMIT  1003
#define ID_EDIT_LIMIT   1004
#define ID_SPIN_LIMIT   1005
#define ID_BTN_TEST     1006
#define ID_LBL_LIMIT    1007

HWND hListView;
HWND hEditLimit;
HWND hSpinLimit;
HWND hLblLimit;
HWND hBtnLimit;
HWND hBtnUnlimit;
HWND hBtnTest;

HBRUSH hbrDarkBg = NULL;
HBRUSH hbrListBg = NULL;
HFONT hFontUI = NULL;

extern int AddDynamicBucket(const char *name, LONGLONG limit_bytes);
extern void RemoveDynamicBucket(const char *name);

// DWM Dark Mode constant for Windows 10 1809+ and Windows 11
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

void SetGlobalFonts() {
    hFontUI = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

void ApplyFontToChildren(HWND hwnd) {
    EnumChildWindows(hwnd, (WNDENUMPROC)SendMessageA, (LPARAM)WM_SETFONT);
}

BOOL CALLBACK SetFontProc(HWND hwnd, LPARAM lParam) {
    SendMessage(hwnd, WM_SETFONT, (WPARAM)hFontUI, TRUE);
    return TRUE;
}

void GuiUpdateList(void) {
    AcquireSRWLockShared(&g_global_stats_lock);
    
    SendMessage(hListView, WM_SETREDRAW, FALSE, 0);
    
    char selName[64] = {0};
    int selIdx = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
    if (selIdx != -1) {
        ListView_GetItemText(hListView, selIdx, 0, selName, sizeof(selName));
    }
    
    ListView_DeleteAllItems(hListView);
    
    int newSelIdx = -1;
    for (int i = 0; i < MAX_GLOBAL_PROCS; i++) {
        if (g_global_stats[i].pid != 0) {
            if (g_global_stats[i].smoothed_kbps < 0.1 && !g_global_stats[i].is_targeted) continue;
            
            LVITEMA lvi = {0};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = i;
            lvi.iSubItem = 0;
            lvi.pszText = g_global_stats[i].name;
            int idx = ListView_InsertItem(hListView, &lvi);
            
            char pidStr[32];
            sprintf(pidStr, "%u", g_global_stats[i].pid);
            ListView_SetItemText(hListView, idx, 1, pidStr);
            
            char speedStr[64];
            sprintf(speedStr, "%.1f KB/s", g_global_stats[i].smoothed_kbps);
            ListView_SetItemText(hListView, idx, 2, speedStr);
            
            ListView_SetItemText(hListView, idx, 3, g_global_stats[i].is_targeted ? "Yes" : "No");
            
            if (strcmp(g_global_stats[i].name, selName) == 0) {
                newSelIdx = idx;
            }
        }
    }
    
    if (newSelIdx != -1) {
        ListView_SetItemState(hListView, newSelIdx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    
    SendMessage(hListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hListView, NULL, TRUE);
    UpdateWindow(hListView);
    
    ReleaseSRWLockShared(&g_global_stats_lock);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE: {
            // Enable dark title bar
            int dark_mode = 1;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_mode, sizeof(dark_mode));

            SetGlobalFonts();
            hbrDarkBg = CreateSolidBrush(RGB(30, 30, 30));
            hbrListBg = CreateSolidBrush(RGB(37, 37, 38));

            INITCOMMONCONTROLSEX icex;
            icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
            icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS;
            InitCommonControlsEx(&icex);
            
            // List View
            hListView = CreateWindowExA(0, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                20, 20, 640, 310, hwnd, (HMENU)ID_LISTVIEW, NULL, NULL);
                
            ListView_SetExtendedListViewStyle(hListView, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ListView_SetBkColor(hListView, RGB(37, 37, 38));
            ListView_SetTextBkColor(hListView, RGB(37, 37, 38));
            ListView_SetTextColor(hListView, RGB(212, 212, 212));
            SetWindowTheme(hListView, L"DarkMode_Explorer", NULL);
            
            LVCOLUMNA lvc = {0};
            lvc.mask = LVCF_TEXT | LVCF_WIDTH;
            lvc.pszText = "Process Name"; lvc.cx = 240;
            ListView_InsertColumn(hListView, 0, &lvc);
            lvc.pszText = "PID"; lvc.cx = 90;
            ListView_InsertColumn(hListView, 1, &lvc);
            lvc.pszText = "Download Speed"; lvc.cx = 160;
            ListView_InsertColumn(hListView, 2, &lvc);
            lvc.pszText = "Throttled?"; lvc.cx = 120;
            ListView_InsertColumn(hListView, 3, &lvc);
            
            // Controls Layout
            int yPos = 350;
            
            hBtnLimit = CreateWindowExA(0, "BUTTON", "Limit Selected App", WS_CHILD | WS_VISIBLE,
                20, yPos, 160, 32, hwnd, (HMENU)ID_BTN_LIMIT, NULL, NULL);
            SetWindowTheme(hBtnLimit, L"DarkMode_Explorer", NULL);
                
            hLblLimit = CreateWindowExA(0, "STATIC", "Limit (KB/s):", WS_CHILD | WS_VISIBLE,
                195, yPos + 7, 85, 20, hwnd, (HMENU)ID_LBL_LIMIT, NULL, NULL);
                
            hEditLimit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "512", WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
                280, yPos + 5, 80, 24, hwnd, (HMENU)ID_EDIT_LIMIT, NULL, NULL);
                
            hSpinLimit = CreateWindowExA(0, UPDOWN_CLASSA, "", WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                0, 0, 0, 0, hwnd, (HMENU)ID_SPIN_LIMIT, NULL, NULL);
                
            SendMessage(hSpinLimit, UDM_SETBUDDY, (WPARAM)hEditLimit, 0);
            SendMessage(hSpinLimit, UDM_SETRANGE32, 0, 1000000); 
            SendMessage(hSpinLimit, UDM_SETPOS32, 0, 512);
            
            hBtnUnlimit = CreateWindowExA(0, "BUTTON", "Remove Limit", WS_CHILD | WS_VISIBLE,
                380, yPos, 120, 32, hwnd, (HMENU)ID_BTN_UNLIMIT, NULL, NULL);
            SetWindowTheme(hBtnUnlimit, L"DarkMode_Explorer", NULL);
                
            hBtnTest = CreateWindowExA(0, "BUTTON", "Run 10s Speed Test", WS_CHILD | WS_VISIBLE,
                510, yPos, 150, 32, hwnd, (HMENU)ID_BTN_TEST, NULL, NULL);
            SetWindowTheme(hBtnTest, L"DarkMode_Explorer", NULL);
                
            EnumChildWindows(hwnd, SetFontProc, 0);
                
            SetTimer(hwnd, 1, 500, NULL);
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            SetTextColor(hdcStatic, RGB(212, 212, 212));
            SetBkColor(hdcStatic, RGB(30, 30, 30));
            return (LRESULT)hbrDarkBg;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = (HDC)wParam;
            SetTextColor(hdcEdit, RGB(212, 212, 212));
            SetBkColor(hdcEdit, RGB(37, 37, 38));
            return (LRESULT)hbrListBg;
        }
        case WM_TIMER:
            GuiUpdateList();
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_BTN_LIMIT || LOWORD(wParam) == ID_BTN_UNLIMIT) {
                int sel = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
                if (sel != -1) {
                    char name[64];
                    ListView_GetItemText(hListView, sel, 0, name, sizeof(name));
                    if (LOWORD(wParam) == ID_BTN_LIMIT) {
                        int limit_kb = (int)SendMessage(hSpinLimit, UDM_GETPOS32, 0, 0);
                        AddDynamicBucket(name, (LONGLONG)limit_kb * 1024);
                    } else {
                        RemoveDynamicBucket(name);
                    }
                    GuiUpdateList();
                }
            } else if (LOWORD(wParam) == ID_BTN_TEST) {
                StartSpeedTest();
            }
            break;
        case WM_DESTROY:
            if (hbrDarkBg) DeleteObject(hbrDarkBg);
            if (hbrListBg) DeleteObject(hbrListBg);
            if (hFontUI) DeleteObject(hFontUI);
            PostQuitMessage(0);
            break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void RunGUI(void) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "CThrottleGUI";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    // Dark background for the main window class
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    
    RegisterClassA(&wc);
    
    HWND hwnd = CreateWindowExA(0, "CThrottleGUI", "C-Throttle",
        WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX, 
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 440,
        NULL, NULL, GetModuleHandle(NULL), NULL);
        
    ShowWindow(hwnd, SW_SHOW);
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    g_running = FALSE;
}
