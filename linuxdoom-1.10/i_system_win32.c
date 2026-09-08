#define boolean win_boolean
#include <windows.h>
#include <dbghelp.h>
#undef boolean
#include "doomstat.h" 
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"

static FILE *doom_logfile = NULL;


extern boolean bot_active;
extern int lookdir;

static LONG WINAPI I_CrashHandler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_DATATYPE_MISALIGNMENT)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static LONG handling = 0;
    if (InterlockedCompareExchange(&handling, 1, 0) != 0)
    {
        ExitProcess(code);
    }

    // Release cursor clip and show cursor so user is not stuck
    ClipCursor(NULL);
    ShowCursor(TRUE);

    if (!doom_logfile)
        doom_logfile = fopen("doom2.log", "a");

    FILE *f = doom_logfile ? doom_logfile : stderr;

    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char timebuf[64] = "unknown";
    if (lt) strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", lt);

    fprintf(f, "\n===================================================================\n");
    fprintf(f, " FATAL CRASH INTERCEPTED AT %s\n", timebuf);
    fprintf(f, "===================================================================\n");

    const char *excName = "UNKNOWN_EXCEPTION";
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION: excName = "EXCEPTION_ACCESS_VIOLATION"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: excName = "EXCEPTION_INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_STACK_OVERFLOW: excName = "EXCEPTION_STACK_OVERFLOW"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: excName = "EXCEPTION_ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_DATATYPE_MISALIGNMENT: excName = "EXCEPTION_DATATYPE_MISALIGNMENT"; break;
    }

    fprintf(f, "Exception: %s (0x%08lX)\n", excName, (unsigned long)code);
    fprintf(f, "Fault Address: 0x%p\n", ep->ExceptionRecord->ExceptionAddress);

    if (code == EXCEPTION_ACCESS_VIOLATION)
    {
        ULONG_PTR accessType = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR targetAddr = ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(f, "Violation: %s invalid address 0x%p%s\n",
                accessType == 0 ? "Read access to" : (accessType == 1 ? "Write access to" : "Execute access at"),
                (void*)targetAddr,
                targetAddr < 0x10000 ? " (NULL or near-zero pointer dereference)" : "");
    }

    // Dump Game Context
    fprintf(f, "\nGame Context at Crash:\n");
    fprintf(f, "  Gamestate: %d, Map: MAP%02d (Episode %d), Gametic: %d\n",
            gamestate, gamemap, gameepisode, gametic);
    fprintf(f, "  Bot Mode: %s\n", bot_active ? "ACTIVE (AI playing)" : "OFF (Manual control)");
    if (consoleplayer >= 0 && consoleplayer < MAXPLAYERS && players[consoleplayer].mo)
    {
        player_t *p = &players[consoleplayer];
        fprintf(f, "  Player HP: %d, Armor: %d, ReadyWeapon: %d\n",
                p->health, p->armorpoints, p->readyweapon);
        fprintf(f, "  Position: (%d, %d, %d), Lookdir: %d\n",
                p->mo->x >> FRACBITS, p->mo->y >> FRACBITS, p->mo->z >> FRACBITS, lookdir);
    }

    // CPU Registers
    CONTEXT ctx = *ep->ContextRecord;
#if defined(_WIN64) || defined(__x86_64__)
    fprintf(f, "\nCPU Registers:\n");
    fprintf(f, "  RIP: 0x%016llX  RSP: 0x%016llX  RBP: 0x%016llX\n",
            (unsigned long long)ctx.Rip, (unsigned long long)ctx.Rsp, (unsigned long long)ctx.Rbp);
    fprintf(f, "  RAX: 0x%016llX  RBX: 0x%016llX  RCX: 0x%016llX  RDX: 0x%016llX\n",
            (unsigned long long)ctx.Rax, (unsigned long long)ctx.Rbx, (unsigned long long)ctx.Rcx, (unsigned long long)ctx.Rdx);
    fprintf(f, "  RSI: 0x%016llX  RDI: 0x%016llX  R8:  0x%016llX  R9:  0x%016llX\n",
            (unsigned long long)ctx.Rsi, (unsigned long long)ctx.Rdi, (unsigned long long)ctx.R8, (unsigned long long)ctx.R9);
    fprintf(f, "  R10: 0x%016llX  R11: 0x%016llX  R12: 0x%016llX  R13: 0x%016llX\n",
            (unsigned long long)ctx.R10, (unsigned long long)ctx.R11, (unsigned long long)ctx.R12, (unsigned long long)ctx.R13);
    fprintf(f, "  R14: 0x%016llX  R15: 0x%016llX\n",
            (unsigned long long)ctx.R14, (unsigned long long)ctx.R15);
#else
    fprintf(f, "\nCPU Registers:\n");
    fprintf(f, "  EIP: 0x%08lX  ESP: 0x%08lX  EBP: 0x%08lX\n",
            (unsigned long)ctx.Eip, (unsigned long)ctx.Esp, (unsigned long)ctx.Ebp);
    fprintf(f, "  EAX: 0x%08lX  EBX: 0x%08lX  ECX: 0x%08lX  EDX: 0x%08lX\n",
            (unsigned long)ctx.Eax, (unsigned long)ctx.Ebx, (unsigned long)ctx.Ecx, (unsigned long)ctx.Edx);
#endif

    // Call Stack Backtrace
    fprintf(f, "\nCall Stack Backtrace:\n");
    char myExe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, myExe, sizeof(myExe));

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    STACKFRAME64 stackFrame;
    memset(&stackFrame, 0, sizeof(stackFrame));
#if defined(_WIN64) || defined(__x86_64__)
    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    stackFrame.AddrPC.Offset = ctx.Rip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = ctx.Rbp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = ctx.Rsp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#else
    DWORD machineType = IMAGE_FILE_MACHINE_I386;
    stackFrame.AddrPC.Offset = ctx.Eip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = ctx.Ebp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = ctx.Esp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#endif

    int frameNum = 0;
    while (StackWalk64(machineType, process, thread, &stackFrame, &ctx,
                       NULL, NULL, NULL, NULL))
    {
        if (stackFrame.AddrPC.Offset == 0)
            break;

        DWORD64 address = stackFrame.AddrPC.Offset;
        HMODULE hMod = NULL;
        char modName[MAX_PATH] = "unknown";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)address, &hMod) && hMod)
        {
            GetModuleFileNameA(hMod, modName, sizeof(modName));
            char *lastSlash = strrchr(modName, '\\');
            if (lastSlash) memmove(modName, lastSlash + 1, strlen(lastSlash));
        }

        char func[256] = "??";
        char fileline[256] = "??:0";

        if (hMod == GetModuleHandleA(NULL))
        {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "addr2line -e \"%s\" -f -C 0x%llx", myExe, (unsigned long long)address);
            FILE *p = _popen(cmd, "r");
            if (p)
            {
                if (fgets(func, sizeof(func), p)) {
                    char *nl = strchr(func, '\n'); if (nl) *nl = 0;
                }
                if (fgets(fileline, sizeof(fileline), p)) {
                    char *nl = strchr(fileline, '\n'); if (nl) *nl = 0;
                }
                _pclose(p);
            }
        }

        fprintf(f, "  #%02d 0x%016llX in %s [%s] (%s)\n",
               frameNum++, (unsigned long long)address, func, fileline, modName);
    }

    // MiniDump
    HANDLE hDump = CreateFileA("doom2_crash.dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDump != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(process, GetCurrentProcessId(), hDump, MiniDumpNormal, &mei, NULL, NULL);
        CloseHandle(hDump);
        fprintf(f, "\nCrash minidump written to doom2_crash.dmp\n");
    }

    fprintf(f, "===================================================================\n\n");
    fflush(f);
    if (doom_logfile) { fclose(doom_logfile); doom_logfile = NULL; }

    char alertMsg[1024];
    snprintf(alertMsg, sizeof(alertMsg),
             "DOOM II Win32 encountered a fatal crash!\n\n"
             "Exception: %s (0x%08lX)\n"
             "Fault Address: 0x%p\n\n"
             "Detailed call stack trace and game state have been written to doom2.log\n"
             "A crash minidump was saved to doom2_crash.dmp",
             excName, (unsigned long)code, ep->ExceptionRecord->ExceptionAddress);
    MessageBoxA(NULL, alertMsg, "DOOM II - Crash Report", MB_OK | MB_ICONERROR);

    ExitProcess(code);
    return EXCEPTION_EXECUTE_HANDLER;
}

void I_InitLog(void)
{
    AddVectoredExceptionHandler(1, I_CrashHandler);
    doom_logfile = fopen("doom2.log", "w");
    if (!doom_logfile) return;
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char timebuf[64] = "unknown";
    if (lt) strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", lt);
    fprintf(doom_logfile, "===================================================================\n");
    fprintf(doom_logfile, " DOOM II Win32 Port v1.10 (Antigravity Enhanced)\n");
    fprintf(doom_logfile, " Started: %s\n", timebuf);
    fprintf(doom_logfile, " Build:   %s %s\n", __DATE__, __TIME__);
    fprintf(doom_logfile, "===================================================================\n\n");
    fflush(doom_logfile);
}

void I_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (doom_logfile) {
        va_start(ap, fmt);
        vfprintf(doom_logfile, fmt, ap);
        va_end(ap);
        fflush(doom_logfile);
    }
}

int mb_used = 6;
ticcmd_t emptycmd;
ticcmd_t *I_BaseTiccmd(void) { return &emptycmd; }
int I_GetHeapSize(void) { return mb_used * 1024 * 1024; }
byte *I_ZoneBase(int *size) { *size = I_GetHeapSize(); return (byte*)malloc(*size); }
int I_GetTime(void) { return (int)(GetTickCount64() * TICRATE / 1000); }
void I_Tactile(int on, int off, int total) { (void)on;(void)off;(void)total; }
void I_Init(void) { I_InitSound(); }
void I_WaitVBL(int count) { Sleep((DWORD)(count * 1000 / 70)); }
void I_BeginRead(void) { }
void I_EndRead(void) { }
byte *I_AllocLow(int length) { byte *p=(byte*)calloc(1,length); return p; }
void I_Quit(void)
{
    I_Log("\nDOOM II Win32: exiting cleanly.\n");
    if (doom_logfile) { fclose(doom_logfile); doom_logfile = NULL; }
    D_QuitNetGame();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults();
    I_ShutdownGraphics();
    exit(0);
}
void I_Error(char *error, ...)
{
    va_list ap; char msg[1024]; va_start(ap,error); vsnprintf(msg,sizeof(msg),error,ap); va_end(ap);
    fprintf(stderr, "DOOM error: %s\n", msg);
    if (doom_logfile) {
        fprintf(doom_logfile, "\nFATAL ERROR: %s\n", msg);
        fclose(doom_logfile); doom_logfile = NULL;
    }
    MessageBoxA(NULL, msg, "DOOM error", MB_OK | MB_ICONERROR);
    I_ShutdownGraphics(); exit(-1);
}
