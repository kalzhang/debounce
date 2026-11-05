#include <windows.h>
#include <stdio.h>
#include <string.h>

// --- TCC Compatibility: Manually define shellapi.h types & dynamic functions ---
#ifndef NIM_ADD
#define NIM_ADD     0x00000000
#define NIM_MODIFY  0x00000001
#define NIM_DELETE  0x00000002
#define NIF_MESSAGE 0x00000001
#define NIF_ICON    0x00000002
#define NIF_TIP     0x00000004

typedef struct _NOTIFYICONDATAA {
    DWORD cbSize;
    HWND hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    HICON hIcon;
    CHAR szTip[64];
} NOTIFYICONDATAA, *PNOTIFYICONDATAA;
#define NOTIFYICONDATA NOTIFYICONDATAA
#endif

BOOL Shell_NotifyIconA(DWORD dwMessage, PNOTIFYICONDATAA lpData) {
    typedef BOOL (WINAPI *PFN)(DWORD, PNOTIFYICONDATAA);
    static PFN pfn = NULL;
    if (!pfn) {
        HMODULE h = LoadLibraryA("shell32.dll");
        if (h) pfn = (PFN)GetProcAddress(h, "Shell_NotifyIconA");
    }
    return pfn ? pfn(dwMessage, lpData) : FALSE;
}

HINSTANCE ShellExecuteA(HWND hwnd, LPCSTR lpOp, LPCSTR lpFile, LPCSTR lpParams, LPCSTR lpDir, INT nShow) {
    typedef HINSTANCE (WINAPI *PFN)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
    static PFN pfn = NULL;
    if (!pfn) {
        HMODULE h = LoadLibraryA("shell32.dll");
        if (h) pfn = (PFN)GetProcAddress(h, "ShellExecuteA");
    }
    return pfn ? pfn(hwnd, lpOp, lpFile, lpParams, lpDir, nShow) : NULL;
}
// -------------------------------------------------------------------------------

// Ensure compatibility with TCC's minimalist headers
#ifndef WM_XBUTTONDOWN
#define WM_XBUTTONDOWN          0x020B
#define WM_XBUTTONUP            0x020C
#define XBUTTON1                0x0001
#define XBUTTON2                0x0002
#endif

#ifndef LLMHF_INJECTED
#define LLMHF_INJECTED          0x00000001
#endif
#ifndef LLMHF_LOWER_IL_INJECTED
#define LLMHF_LOWER_IL_INJECTED 0x00000002
#endif

#define WM_TRAYICON     (WM_USER + 1)
#define ID_TRAY_STATUS  1000
#define ID_TRAY_ENABLE  1001
#define ID_TRAY_STARTUP 1002
#define ID_TRAY_EDIT    1003
#define ID_TRAY_RELOAD  1004
#define ID_TRAY_EXIT    1005

#define TASK_NAME "MouseDebounceFilter"

// Button indices: 0=Left, 1=Right, 2=Middle, 3=Mouse4(Back), 4=Mouse5(Forward)
DWORD g_debounceMs[5]    = {45, 45, 45, 45, 45};
DWORD g_lastDownTime[5]  = {0, 0, 0, 0, 0};
DWORD g_lastUpTime[5]    = {0, 0, 0, 0, 0};
BOOL  g_blockNextUp[5]   = {FALSE, FALSE, FALSE, FALSE, FALSE};

HHOOK g_hMouseHook       = NULL;
HICON g_hIconActive      = NULL;
HICON g_hIconInactive    = NULL;
BOOL  g_bEnabled         = TRUE;
BOOL  g_bStartupEnabled  = FALSE;
DWORD g_sessionBlocked   = 0;
DWORD g_savedSession     = 0;
DWORD g_baseTotalBlocked = 0;
UINT  g_uTaskbarRestart  = 0;
char  g_exePath[MAX_PATH];
char  g_iniPath[MAX_PATH + 32];
NOTIFYICONDATA g_nid;

// Generate a 16x16 Mouse Silhouette icon in memory ('.' = transparent, '#' = border/seam, 'X' = body)
HICON CreateMouseIcon(DWORD bodyColor, DWORD borderColor) {
    static const char *mouseArt[16] = {
        ".....######.....",
        "...##XX##XX##...",
        "..#XXXX##XXXX#..",
        "..#XXXX##XXXX#..",
        "..#XXXX##XXXX#..",
        "..#XXXX##XXXX#..",
        "..############..",
        "..#XXXXXXXXXX#..",
        "..#XXXXXXXXXX#..",
        "..#XXXXXXXXXX#..",
        "..#XXXXXXXXXX#..",
        "..#XXXXXXXXXX#..",
        "...#XXXXXXXX#...",
        "...##XXXXXX##...",
        ".....######.....",
        "................"
    };
    DWORD xorMask[16 * 16];
    BYTE  andMask[16 * 4];
    ZeroMemory(xorMask, sizeof(xorMask));
    FillMemory(andMask, sizeof(andMask), 0xFF); // Default all pixels to transparent in legacy mask

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            char c = mouseArt[y][x];
            if (c != '.') {
                xorMask[y * 16 + x] = (c == 'X') ? bodyColor : borderColor;
                andMask[y * 4 + (x >> 3)] &= ~(0x80 >> (x & 7)); // Clear transparency bit
            }
        }
    }
    return CreateIcon(NULL, 16, 16, 1, 32, andMask, (BYTE*)xorMask);
}

void ResetDebounceState() {
    ZeroMemory(g_lastDownTime, sizeof(g_lastDownTime));
    ZeroMemory(g_lastUpTime, sizeof(g_lastUpTime));
    ZeroMemory(g_blockNextUp, sizeof(g_blockNextUp));
}

// Save lifetime blocked count to config.ini only when it has changed
void SaveStats() {
    if (g_sessionBlocked != g_savedSession) {
        DWORD total = g_baseTotalBlocked + (g_sessionBlocked - g_savedSession);
        char buf[32];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)total);
        WritePrivateProfileStringA("Stats", "TotalBlocked", buf, g_iniPath);
        g_baseTotalBlocked = total;
        g_savedSession = g_sessionBlocked;
    }
}

// Dynamically check if running as Administrator
BOOL CheckIsAdmin() {
    BOOL isAdmin = FALSE;
    HMODULE hShell = LoadLibraryA("shell32.dll");
    if (hShell) {
        typedef BOOL (WINAPI *IsUserAnAdmin_t)(void);
        IsUserAnAdmin_t pIsUserAnAdmin = (IsUserAnAdmin_t)GetProcAddress(hShell, "IsUserAnAdmin");
        if (pIsUserAnAdmin) isAdmin = pIsUserAnAdmin();
        FreeLibrary(hShell);
    }
    return isAdmin;
}

// Run a hidden command while continuing to pump the message loop so WH_MOUSE_LL never freezes
DWORD RunSilentCommand(char* cmd) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD startTime = GetTickCount();
        while (TRUE) {
            DWORD elapsed = GetTickCount() - startTime;
            if (elapsed >= 3000) break;

            DWORD waitResult = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 3000 - elapsed, QS_ALLINPUT);
            if (waitResult == WAIT_OBJECT_0) {
                break; // Process finished
            } else if (waitResult == WAIT_OBJECT_0 + 1) {
                MSG msg;
                BOOL bQuit = FALSE;
                while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        PostQuitMessage((int)msg.wParam);
                        bQuit = TRUE;
                        break;
                    }
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
                if (bQuit) break;
            } else {
                break; // Timeout or error
            }
        }

        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return exitCode;
    }
    return 1;
}

BOOL QueryStartupStatus() {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "schtasks /query /tn \"%s\"", TASK_NAME);
    return (RunSilentCommand(cmd) == 0);
}

void ToggleStartup() {
    char cmd[MAX_PATH * 2];
    if (g_bStartupEnabled) {
        snprintf(cmd, sizeof(cmd), "schtasks /delete /tn \"%s\" /f", TASK_NAME);
        if (RunSilentCommand(cmd) == 0) {
            g_bStartupEnabled = FALSE;
        }
    } else {
        // Generate temporary XML to allow running on laptop batteries, remove the 72h time limit,
        // and run at Normal priority (4) instead of Task Scheduler's default Below Normal (7).
        char tempDir[MAX_PATH];
        char xmlPath[MAX_PATH + 32];
        GetTempPathA(MAX_PATH, tempDir);
        snprintf(xmlPath, sizeof(xmlPath), "%sMouseDebounceTask.xml", tempDir);

        FILE *f = fopen(xmlPath, "w");
        if (f) {
            fprintf(f,
                "<?xml version=\"1.0\"?>\n"
                "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
                "  <Triggers><LogonTrigger><Enabled>true</Enabled></LogonTrigger></Triggers>\n"
                "  <Principals><Principal id=\"Author\"><RunLevel>HighestAvailable</RunLevel><LogonType>InteractiveToken</LogonType></Principal></Principals>\n"
                "  <Settings>\n"
                "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
                "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
                "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
                "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
                "    <Priority>4</Priority>\n"
                "  </Settings>\n"
                "  <Actions Context=\"Author\"><Exec><Command>\"");

            // Escape '&' in path if present so XML stays valid
            for (const char *p = g_exePath; *p; p++) {
                if (*p == '&') fputs("&amp;", f);
                else fputc(*p, f);
            }

            fprintf(f, "\"</Command></Exec></Actions>\n</Task>\n");
            fclose(f);

            snprintf(cmd, sizeof(cmd), "schtasks /create /tn \"%s\" /xml \"%s\" /f", TASK_NAME, xmlPath);
            if (RunSilentCommand(cmd) == 0) {
                g_bStartupEnabled = TRUE;
            }
            DeleteFileA(xmlPath);
        }
    }
}

void LoadConfig() {
    // Auto-generate config.ini if it doesn't exist
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(g_iniPath, "w");
        if (f) {
            fprintf(f, "[DebounceMs]\n");
            fprintf(f, "; Time in milliseconds to block rapid duplicate clicks (Set to 0 to disable a button)\n");
            fprintf(f, "Left=45\n");
            fprintf(f, "Right=45\n");
            fprintf(f, "Middle=45\n");
            fprintf(f, "Mouse4=45\n");
            fprintf(f, "Mouse5=45\n\n");
            fprintf(f, "[Stats]\n");
            fprintf(f, "TotalBlocked=0\n");
            fclose(f);
        }
    }
    g_debounceMs[0]    = GetPrivateProfileIntA("DebounceMs", "Left",   45, g_iniPath);
    g_debounceMs[1]    = GetPrivateProfileIntA("DebounceMs", "Right",  45, g_iniPath);
    g_debounceMs[2]    = GetPrivateProfileIntA("DebounceMs", "Middle", 45, g_iniPath);
    g_debounceMs[3]    = GetPrivateProfileIntA("DebounceMs", "Mouse4", 45, g_iniPath);
    g_debounceMs[4]    = GetPrivateProfileIntA("DebounceMs", "Mouse5", 45, g_iniPath);
    g_baseTotalBlocked = GetPrivateProfileIntA("Stats", "TotalBlocked", 0, g_iniPath);
    g_savedSession     = g_sessionBlocked;
    ResetDebounceState();
}

// Ultra-lightweight Low-Level Mouse Hook
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // Fast-path: Immediately ignore mouse movement and disabled states (0% CPU overhead)
    if (nCode == HC_ACTION && g_bEnabled && wParam != WM_MOUSEMOVE) {
        MSLLHOOKSTRUCT *pMouse = (MSLLHOOKSTRUCT *)lParam;

        // Ignore software-injected events (macros, AutoHotkey, Remote Desktop, etc.)
        if (pMouse->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) {
            return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
        }

        int btn = -1;
        BOOL isDown = FALSE;
        BOOL isUp   = FALSE;

        switch (wParam) {
            case WM_LBUTTONDOWN: btn = 0; isDown = TRUE; break;
            case WM_LBUTTONUP:   btn = 0; isUp   = TRUE; break;
            case WM_RBUTTONDOWN: btn = 1; isDown = TRUE; break;
            case WM_RBUTTONUP:   btn = 1; isUp   = TRUE; break;
            case WM_MBUTTONDOWN: btn = 2; isDown = TRUE; break;
            case WM_MBUTTONUP:   btn = 2; isUp   = TRUE; break;
            case WM_XBUTTONDOWN:
                btn = (HIWORD(pMouse->mouseData) == XBUTTON1) ? 3 : 4;
                isDown = TRUE;
                break;
            case WM_XBUTTONUP:
                btn = (HIWORD(pMouse->mouseData) == XBUTTON1) ? 3 : 4;
                isUp = TRUE;
                break;
        }

        if (btn != -1 && g_debounceMs[btn] > 0) {
            // Avoid 0 so 0 strictly means "uninitialized" even on 49.7-day tick wrap-around
            DWORD now = pMouse->time ? pMouse->time : 1;

            if (isDown) {
                DWORD sinceDown = now - g_lastDownTime[btn];
                DWORD sinceUp   = now - g_lastUpTime[btn];

                // Block if too close to previous press OR too close to previous release
                if ((g_lastDownTime[btn] != 0 && sinceDown < g_debounceMs[btn]) ||
                    (g_lastUpTime[btn]   != 0 && sinceUp   < g_debounceMs[btn])) {
                    g_blockNextUp[btn] = TRUE;
                    g_sessionBlocked++;
                    return 1;
                }
                g_lastDownTime[btn] = now;
                g_blockNextUp[btn]  = FALSE;
            } 
            else if (isUp) {
                if (g_blockNextUp[btn]) {
                    // Swallow the corresponding UP event of the blocked click
                    g_blockNextUp[btn] = FALSE;
                    return 1;
                }
                g_lastUpTime[btn] = now;
            }
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

void ShowTrayMenu(HWND hwnd) {
    SaveStats(); // Persist any newly blocked clicks when viewing the menu

    HMENU hMenu = CreatePopupMenu();
    char statusText[96];
    DWORD lifetimeTotal = g_baseTotalBlocked + (g_sessionBlocked - g_savedSession);
    snprintf(statusText, sizeof(statusText), "Blocked: %lu (Total: %lu)",
             (unsigned long)g_sessionBlocked, (unsigned long)lifetimeTotal);

    AppendMenuA(hMenu, MF_STRING | MF_GRAYED, ID_TRAY_STATUS, statusText);
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING | (g_bEnabled ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_ENABLE, "Enabled");
    AppendMenuA(hMenu, MF_STRING | (g_bStartupEnabled ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_STARTUP, "Run on Startup (Admin)");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, ID_TRAY_EDIT, "Edit Config (config.ini)");
    AppendMenuA(hMenu, MF_STRING, ID_TRAY_RELOAD, "Reload Config");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageA(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        return 0;
    } 
    else if (msg == WM_COMMAND) {
        switch (LOWORD(wParam)) {
            case ID_TRAY_ENABLE:
                g_bEnabled = !g_bEnabled;
                ResetDebounceState();
                g_nid.hIcon = g_bEnabled ? g_hIconActive : g_hIconInactive;
                strcpy(g_nid.szTip, g_bEnabled ? "Mouse Debounce Filter (Enabled)" : "Mouse Debounce Filter (Disabled)");
                Shell_NotifyIconA(NIM_MODIFY, &g_nid);
                break;
            case ID_TRAY_STARTUP:
                ToggleStartup();
                break;
            case ID_TRAY_EDIT:
                SaveStats(); // Ensure config.ini has latest stats before opening in editor
                ShellExecuteA(NULL, "open", g_iniPath, NULL, NULL, SW_SHOWNORMAL);
                break;
            case ID_TRAY_RELOAD:
                SaveStats();
                LoadConfig();
                break;
            case ID_TRAY_EXIT:
                SaveStats();
                PostQuitMessage(0);
                break;
        }
        return 0;
    } 
    else if (msg == g_uTaskbarRestart && g_uTaskbarRestart != 0) {
        // Re-add tray icon if explorer.exe crashes and restarts
        Shell_NotifyIconA(NIM_ADD, &g_nid);
        return 0;
    }
    else if (msg == WM_ENDSESSION) {
        // Save stats when Windows shuts down or restarts
        if (wParam) SaveStats();
        return 0;
    }
    else if (msg == WM_DESTROY) {
        SaveStats();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 1. Prevent duplicate instances before UAC prompt
    // Note: ERROR_ACCESS_DENIED means an elevated instance already holds the mutex
    HANDLE hMutex = CreateMutexA(NULL, FALSE, "Local\\MouseDebounceSingletonMutex");
    DWORD lastErr = GetLastError();
    if (lastErr == ERROR_ALREADY_EXISTS || lastErr == ERROR_ACCESS_DENIED) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // 2. Get paths for the EXE and config.ini safely
    GetModuleFileNameA(NULL, g_exePath, MAX_PATH);
    g_exePath[MAX_PATH - 1] = '\0';
    strncpy(g_iniPath, g_exePath, MAX_PATH);
    g_iniPath[MAX_PATH - 1] = '\0';
    char *lastSlash = strrchr(g_iniPath, '\\');
    if (lastSlash) {
        strcpy(lastSlash + 1, "config.ini");
    } else {
        strcpy(g_iniPath, "config.ini");
    }

    // 3. Self-elevate to Administrator if not already elevated (with loop protection)
    if (!CheckIsAdmin()) {
        if (hMutex) CloseHandle(hMutex); // Release mutex so elevated child can acquire it
        if (!lpCmdLine || !strstr(lpCmdLine, "--elevated")) {
            ShellExecuteA(NULL, "runas", g_exePath, "--elevated", NULL, SW_SHOWNORMAL);
        }
        return 0;
    }

    // 4. Boost process priority so 100% CPU loads never delay the mouse hook,
    // and enable DPI awareness so the tray menu renders with crisp fonts
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    typedef BOOL (WINAPI *SetProcessDPIAware_t)(void);
    SetProcessDPIAware_t pSetDPI = (SetProcessDPIAware_t)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware");
    if (pSetDPI) pSetDPI();

    // 5. Load config and cache initial startup state before installing hook
    LoadConfig();
    g_bStartupEnabled = QueryStartupStatus();

    // 6. Create hidden top-level window (receives TaskbarCreated broadcasts unlike HWND_MESSAGE)
    g_uTaskbarRestart = RegisterWindowMessageA("TaskbarCreated");

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = "DebounceHiddenWndClass";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA("DebounceHiddenWndClass", "MouseDebounce", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    // 7. Create Active (Whitish-Grey) and Inactive (Dimmed Grey) Mouse Silhouette Icons
    g_hIconActive   = CreateMouseIcon(0xFFF0F0F0, 0xFF202020);
    g_hIconInactive = CreateMouseIcon(0xFF686868, 0xFF252525);

    // 8. Add System Tray icon
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = g_hIconActive;
    strcpy(g_nid.szTip, "Mouse Debounce Filter (Enabled)");
    Shell_NotifyIconA(NIM_ADD, &g_nid);

    // 9. Install the Low-Level Mouse Hook
    g_hMouseHook = SetWindowsHookExA(WH_MOUSE_LL, LowLevelMouseProc, hInstance, 0);

    // 10. Run native Windows message loop (waits at 0% CPU until an event happens)
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // 11. Clean up on exit
    SaveStats();
    if (g_hMouseHook) UnhookWindowsHookEx(g_hMouseHook);
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
    if (g_hIconActive) DestroyIcon(g_hIconActive);
    if (g_hIconInactive) DestroyIcon(g_hIconInactive);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}