/**
 @file debugger.cpp

 @brief Implements the debugger class.
 */

#include "debugger.h"
#include "console.h"
#include "memory.h"
#include "threading.h"
#include "command.h"
#include "database.h"
#include "addrinfo.h"
#include "watch.h"
#include "thread.h"
#include "plugin_loader.h"
#include "breakpoint.h"
#include "symbolinfo.h"
#include "variable.h"
#include "x64_dbg.h"
#include "exception.h"
#include "error.h"
#include "module.h"
#include "commandline.h"
#include "stackinfo.h"
#include "stringformat.h"
#include "TraceRecord.h"
#include "historycontext.h"
#include "taskthread.h"
#include "animate.h"
#include "simplescript.h"

struct TraceCondition
{
    ExpressionParser condition;
    duint steps;
    duint maxSteps;

    explicit TraceCondition(String expression, duint maxCount)
        : condition(expression), steps(0), maxSteps(maxCount) {}

    inline bool ContinueTrace()
    {
        steps++;
        if(steps >= maxSteps)
            return false;
        duint value = 1;
        return condition.Calculate(value, valuesignedcalc(), true) && !value;
    }
};

static PROCESS_INFORMATION g_pi = {0, 0, 0, 0};
static char szBaseFileName[MAX_PATH] = "";
static TraceCondition* traceCondition = nullptr;
static bool bFileIsDll = false;
static duint pDebuggedBase = 0;
static duint pCreateProcessBase = 0;
static duint pDebuggedEntry = 0;
static bool isStepping = false;
static bool isPausedByUser = false;
static bool isDetachedByUser = false;
static bool bIsAttached = false;
static bool bSkipExceptions = false;
static bool bBreakOnNextDll = false;
static bool bFreezeStack = false;
static int ecount = 0;
static std::vector<ExceptionRange> ignoredExceptionRange;
static HANDLE hEvent = 0;
static HANDLE hProcess = 0;
static HANDLE hMemMapThread = 0;
static bool bStopMemMapThread = false;
static HANDLE hTimeWastedCounterThread = 0;
static bool bStopTimeWastedCounterThread = false;
static HANDLE hDumpRefreshThread = 0;
static bool bStopDumpRefreshThread = false;
static String lastDebugText;
static duint timeWastedDebugging = 0;
static EXCEPTION_DEBUG_INFO lastExceptionInfo = { 0 };
static char szDebuggeeInitializationScript[MAX_PATH] = "";
char szFileName[MAX_PATH] = "";
char szSymbolCachePath[MAX_PATH] = "";
char sqlitedb[deflen] = "";
std::vector<std::pair<duint, duint>> RunToUserCodeBreakpoints;
PROCESS_INFORMATION* fdProcessInfo = &g_pi;
HANDLE hActiveThread;
HANDLE hProcessToken;
bool bUndecorateSymbolNames = true;
bool bEnableSourceDebugging = true;
bool bTraceRecordEnabledDuringTrace = true;
bool bSkipInt3Stepping = false;
duint DbgEvents = 0;

static duint dbgcleartracecondition()
{
    duint steps = 0;
    if(traceCondition)
    {
        steps = traceCondition->steps;
        delete traceCondition;
    }
    traceCondition = nullptr;
    return steps;
}

static void dbgClearRtuBreakpoints()
{
    EXCLUSIVE_ACQUIRE(LockRunToUserCode);
    for(auto i : RunToUserCodeBreakpoints)
    {
        BREAKPOINT bp;
        if(!BpGet(i.first, BPMEMORY, nullptr, &bp))
            RemoveMemoryBPX(i.first, i.second);
    }
    RunToUserCodeBreakpoints.clear();
}

bool dbgsettracecondition(String expression, duint maxSteps)
{
    if(dbgtraceactive())
        return false;
    traceCondition = new TraceCondition(expression, maxSteps);
    if(traceCondition->condition.IsValidExpression())
        return true;
    dbgcleartracecondition();
    return false;
}

bool dbgtraceactive()
{
    return traceCondition != nullptr;
}

static DWORD WINAPI memMapThread(void* ptr)
{
    while(!bStopMemMapThread)
    {
        while(!DbgIsDebugging())
        {
            if(bStopMemMapThread)
                break;
            Sleep(10);
        }
        if(bStopMemMapThread)
            break;
        MemUpdateMapAsync();
        Sleep(2000);
    }

    return 0;
}

static DWORD WINAPI timeWastedCounterThread(void* ptr)
{
    if(!BridgeSettingGetUint("Engine", "TimeWastedDebugging", &timeWastedDebugging))
        timeWastedDebugging = 0;
    GuiUpdateTimeWastedCounter();
    while(!bStopTimeWastedCounterThread)
    {
        while(!DbgIsDebugging())
        {
            if(bStopTimeWastedCounterThread)
                break;
            Sleep(10);
        }
        if(bStopTimeWastedCounterThread)
            break;
        timeWastedDebugging++;
        GuiUpdateTimeWastedCounter();
        Sleep(1000);
    }
    BridgeSettingSetUint("Engine", "TimeWastedDebugging", timeWastedDebugging);
    return 0;
}

static DWORD WINAPI dumpRefreshThread(void* ptr)
{
    while(!bStopDumpRefreshThread)
    {
        while(!DbgIsDebugging())
        {
            if(bStopDumpRefreshThread)
                break;
            Sleep(10);
        }
        if(bStopDumpRefreshThread)
            break;
        GuiUpdateDumpView();
        Sleep(200);
    }
    return 0;
}

/**
\brief Called when the debugger pauses.
*/
void cbDebuggerPaused()
{
    // Clear tracing conditions
    dbgcleartracecondition();
    dbgClearRtuBreakpoints();
    // Trace record is not handled by this function currently.
    // Signal thread switch warning
    if(settingboolget("Engine", "HardcoreThreadSwitchWarning"))
    {
        static DWORD PrevThreadId = 0;
        if(PrevThreadId == 0)
            PrevThreadId = fdProcessInfo->dwThreadId; // Initialize to Main Thread
        DWORD currentThreadId = ThreadGetId(hActiveThread);
        if(currentThreadId != PrevThreadId)
        {
            dprintf(QT_TRANSLATE_NOOP("DBG", "Thread switched from %X to %X !\n"), PrevThreadId, currentThreadId);
            PrevThreadId = currentThreadId;
        }
    }
    // Watchdog
    cbWatchdog(0, nullptr);
}

void dbginit()
{
    hTimeWastedCounterThread = CreateThread(nullptr, 0, timeWastedCounterThread, nullptr, 0, nullptr);
    hMemMapThread = CreateThread(nullptr, 0, memMapThread, nullptr, 0, nullptr);
    hDumpRefreshThread = CreateThread(nullptr, 0, dumpRefreshThread, nullptr, 0, nullptr);
}

void dbgstop()
{
    bStopTimeWastedCounterThread = true;
    bStopMemMapThread = true;
    bStopDumpRefreshThread = true;
    WaitForThreadTermination(hTimeWastedCounterThread);
    WaitForThreadTermination(hMemMapThread);
    WaitForThreadTermination(hDumpRefreshThread);
}

duint dbgdebuggedbase()
{
    return pDebuggedBase;
}

duint dbggettimewastedcounter()
{
    return timeWastedDebugging;
}

bool dbgisrunning()
{
    return !waitislocked(WAITID_RUN);
}

bool dbgisdll()
{
    return bFileIsDll;
}

void dbgsetattachevent(HANDLE handle)
{
    hEvent = handle;
}

void dbgsetskipexceptions(bool skip)
{
    bSkipExceptions = skip;
}

void dbgsetstepping(bool stepping)
{
    isStepping = stepping;
}

void dbgsetispausedbyuser(bool b)
{
    isPausedByUser = b;
}

void dbgsetisdetachedbyuser(bool b)
{
    isDetachedByUser = b;
}

void dbgsetfreezestack(bool freeze)
{
    bFreezeStack = freeze;
}

void dbgclearignoredexceptions()
{
    ignoredExceptionRange.clear();
}

void dbgaddignoredexception(ExceptionRange range)
{
    ignoredExceptionRange.push_back(range);
}

bool dbgisignoredexception(unsigned int exception)
{
    for(unsigned int i = 0; i < ignoredExceptionRange.size(); i++)
    {
        unsigned int curStart = ignoredExceptionRange.at(i).start;
        unsigned int curEnd = ignoredExceptionRange.at(i).end;
        if(exception >= curStart && exception <= curEnd)
            return true;
    }
    return false;
}

bool dbgcmdnew(const char* name, CBCOMMAND cbCommand, bool debugonly)
{
    if(!cmdnew(name, cbCommand, debugonly))
        return false;
    GuiAutoCompleteAddCmd(name);
    return true;
}

bool dbgcmddel(const char* name)
{
    if(!cmddel(name))
        return false;
    GuiAutoCompleteDelCmd(name);
    return true;
}

duint dbggetdbgevents()
{
    return InterlockedExchange(&DbgEvents, 0);
}

static DWORD WINAPI updateCallStackThread(duint ptr)
{
    stackupdatecallstack(ptr);
    GuiUpdateCallStack();
    return 0;
}

void updateCallStackAsync(duint ptr)
{
    static TaskThread_<decltype(&updateCallStackThread), duint> updateCallStackTask(&updateCallStackThread);
    updateCallStackTask.WakeUp(ptr);
}

DWORD WINAPI updateSEHChainThread()
{
    GuiUpdateSEHChain();
    stackupdateseh();
    GuiUpdateDumpView();
    return 0;
}

void updateSEHChainAsync()
{
    static auto updateSEHChainTask = MakeTaskThread(&updateSEHChainThread);
    updateSEHChainTask.WakeUp();
}

void DebugUpdateGui(duint disasm_addr, bool stack)
{
    if(GuiIsUpdateDisabled())
        return;
    duint cip = GetContextDataEx(hActiveThread, UE_CIP);
    //Check if the addresses are in the memory map and force update if they are not
    if(!MemIsValidReadPtr(disasm_addr, true) || !MemIsValidReadPtr(cip, true))
        MemUpdateMap();
    else
        MemUpdateMapAsync();
    if(MemIsValidReadPtr(disasm_addr))
    {
        if(bEnableSourceDebugging)
        {
            char szSourceFile[MAX_STRING_SIZE] = "";
            int line = 0;
            if(SymGetSourceLine(cip, szSourceFile, &line))
                GuiLoadSourceFile(szSourceFile, line);
        }
        GuiDisasmAt(disasm_addr, cip);
    }
    duint csp = GetContextDataEx(hActiveThread, UE_CSP);
    if(stack)
        DebugUpdateStack(csp, csp);
    static duint cacheCsp = 0;
    if(csp != cacheCsp)
    {
        InterlockedExchange(&cacheCsp, csp);
        updateCallStackAsync(csp);
        updateSEHChainAsync();
    }
    char modname[MAX_MODULE_SIZE] = "";
    char modtext[MAX_MODULE_SIZE * 2] = "";
    if(!ModNameFromAddr(disasm_addr, modname, true))
        *modname = 0;
    else
        sprintf_s(modtext, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "Module: %s - ")), modname);
    char threadswitch[256] = "";
    DWORD currentThreadId = ThreadGetId(hActiveThread);
    {
        static DWORD PrevThreadId = 0;
        if(PrevThreadId == 0)
            PrevThreadId = fdProcessInfo->dwThreadId; // Initialize to Main Thread
        if(currentThreadId != PrevThreadId)
        {
            char threadName2[MAX_THREAD_NAME_SIZE] = "";
            if(!ThreadGetName(PrevThreadId, threadName2) || threadName2[0] == 0)
                sprintf_s(threadName2, "%X", PrevThreadId);
            sprintf_s(threadswitch, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", " (switched from %s)")), threadName2);
            PrevThreadId = currentThreadId;
        }
    }
    char title[1024] = "";
    char threadName1[MAX_THREAD_NAME_SIZE] = "";
    if(!ThreadGetName(currentThreadId, threadName1) || threadName1[0] == 0)
        sprintf_s(threadName1, "%X", currentThreadId);
    sprintf_s(title, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "File: %s - PID: %X - %sThread: %s%s")), szBaseFileName, fdProcessInfo->dwProcessId, modtext, threadName1, threadswitch);
    GuiUpdateWindowTitle(title);
    GuiUpdateAllViews();
    GuiFocusView(GUI_DISASSEMBLY);
}

void DebugUpdateGuiSetState(duint disasm_addr, bool stack, DBGSTATE state = paused)
{
    GuiSetDebugState(state);
    DebugUpdateGui(disasm_addr, stack);
}
void DebugUpdateGuiSetStateAsync(duint disasm_addr, bool stack, DBGSTATE state)
{
    // call paused routine to clean up various tracing states.
    if(state == DBGSTATE::paused)
        cbDebuggerPaused();
    //correctly orders the GuiSetDebugState DebugUpdateGui to prevent drawing inconsistencies
    static TaskThread_<decltype(&DebugUpdateGuiSetState), duint, bool, DBGSTATE> DebugUpdateGuiSetStateTask(&DebugUpdateGuiSetState);
    DebugUpdateGuiSetStateTask.WakeUp(disasm_addr, stack, state);
}

void DebugUpdateGuiAsync(duint disasm_addr, bool stack)
{
    static TaskThread_<decltype(&DebugUpdateGui), duint, bool> DebugUpdateGuiTask(&DebugUpdateGui);
    DebugUpdateGuiTask.WakeUp(disasm_addr, stack);
}

void DebugUpdateBreakpointsViewAsync()
{
    static TaskThread_<decltype(&GuiUpdateBreakpointsView)> BreakpointsUpdateGuiTask(&GuiUpdateBreakpointsView);
    BreakpointsUpdateGuiTask.WakeUp();
}


void DebugUpdateStack(duint dumpAddr, duint csp, bool forceDump)
{
    if(GuiIsUpdateDisabled())
        return;
    if(!forceDump && bFreezeStack)
    {
        SELECTIONDATA selection;
        if(GuiSelectionGet(GUI_STACK, &selection))
            dumpAddr = selection.start;
    }
    GuiStackDumpAt(dumpAddr, csp);
    GuiUpdateArgumentWidget();
}

static void printSoftBpInfo(const BREAKPOINT & bp)
{
    auto bptype = "INT3";
    int titantype = bp.titantype;
    if((titantype & UE_BREAKPOINT_TYPE_UD2) == UE_BREAKPOINT_TYPE_UD2)
        bptype = "UD2";
    else if((titantype & UE_BREAKPOINT_TYPE_LONG_INT3) == UE_BREAKPOINT_TYPE_LONG_INT3)
        bptype = "LONG INT3";
    auto symbolicname = SymGetSymbolicName(bp.addr);
    if(symbolicname.length())
    {
        if(*bp.name)
            dprintf(QT_TRANSLATE_NOOP("DBG", "%s breakpoint \"%s\" at %s (%p)!\n"), bptype, bp.name, symbolicname.c_str(), bp.addr);
        else
            dprintf(QT_TRANSLATE_NOOP("DBG", "%s breakpoint at %s (%p)!\n"), bptype, symbolicname.c_str(), bp.addr);
    }
    else
    {
        if(*bp.name)
            dprintf(QT_TRANSLATE_NOOP("DBG", "%s breakpoint \"%s\" at %p!\n"), bptype, bp.name, bp.addr);
        else
            dprintf(QT_TRANSLATE_NOOP("DBG", "%s breakpoint at %p!\n"), bptype, bp.addr);
    }
}

static void printHwBpInfo(const BREAKPOINT & bp)
{
    const char* bpsize = "";
    switch(TITANGETSIZE(bp.titantype))   //size
    {
    case UE_HARDWARE_SIZE_1:
        bpsize = "byte, ";
        break;
    case UE_HARDWARE_SIZE_2:
        bpsize = "word, ";
        break;
    case UE_HARDWARE_SIZE_4:
        bpsize = "dword, ";
        break;
#ifdef _WIN64
    case UE_HARDWARE_SIZE_8:
        bpsize = "qword, ";
        break;
#endif //_WIN64
    }
    char* bptype;
    switch(TITANGETTYPE(bp.titantype))   //type
    {
    case UE_HARDWARE_EXECUTE:
        bptype = _strdup(GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "execute")));
        bpsize = "";
        break;
    case UE_HARDWARE_READWRITE:
        bptype = _strdup(GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "read/write")));
        break;
    case UE_HARDWARE_WRITE:
        bptype = _strdup(GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "write")));
        break;
    default:
        bptype = _strdup(" ");
    }
    auto symbolicname = SymGetSymbolicName(bp.addr);
    if(symbolicname.length())
    {
        if(*bp.name)
            dprintf(QT_TRANSLATE_NOOP("DBG", "Hardware breakpoint (%s%s) \"%s\" at %s (%p)!\n"), bpsize, bptype, bp.name, symbolicname.c_str(), bp.addr);
        else
            dprintf(QT_TRANSLATE_NOOP("DBG", "Hardware breakpoint (%s%s) at %s (%p)!\n"), bpsize, bptype, symbolicname.c_str(), bp.addr);
    }
    else
    {
        if(*bp.name)
            dprintf(QT_TRANSLATE_NOOP("DBG", "Hardware breakpoint (%s%s) \"%s\" at %p!\n"), bpsize, bptype, bp.name, bp.addr);
        else
            dprintf(QT_TRANSLATE_NOOP("DBG", "Hardware breakpoint (%s%s) at %p!\n"), bpsize, bptype, bp.addr);
    }
    free(bptype);
}

static void printMemBpInfo(const BREAKPOINT & bp, const void* ExceptionAddress)
{
    char* bptype;
    switch(bp.titantype)
    {
    case UE_MEMORY_READ:
        bptype = _strdup(GuiTranslateText(QT_TRANSLATE_NOOP("DBG", " (read)")));
        break;
    case UE_MEMORY_WRITE:
        bptype = _strdup(GuiTranslateText(QT_TRANSLATE_NOOP("DBG", " (write)")));
        break;
    case UE_MEMORY_EXECUTE:
        bptype = _strdup(GuiTranslateText(QT_TRANSLATE_NOOP("DBG", " (execute)")));
        break;
    case UE_MEMORY:
        bptype = _strdup(GuiTranslateText(QT_TRANSLATE_NOOP("DBG", " (read/write/execute)")));
        break;
    default:
        bptype = _strdup("");
    }
    auto symbolicname = SymGetSymbolicName(bp.addr);
    if(symbolicname.length())
    {
        if(*bp.name)
            dprintf(QT_TRANSLATE_NOOP("DBG", "Memory breakpoint%s \"%s\" at %s (%p, %p)!\n"), bptype, bp.name, symbolicname.c_str(), bp.addr, ExceptionAddress);
        else
            dprintf(QT_TRANSLATE_NOOP("DBG", "Memory breakpoint%s at %s (%p, %p)!\n"), bptype, symbolicname.c_str(), bp.addr, ExceptionAddress);
    }
    else
    {
        if(*bp.name)
            dprintf(QT_TRANSLATE_NOOP("DBG", "Memory breakpoint%s \"%s\" at %p (%p)!\n"), bptype, bp.name, bp.addr, ExceptionAddress);
        else
            dprintf(QT_TRANSLATE_NOOP("DBG", "Memory breakpoint%s at %p (%p)!\n"), bptype, bp.addr, ExceptionAddress);
    }
    free(bptype);
}

static bool getConditionValue(const char* expression)
{
    auto word = *(uint16*)expression;
    if(word == '0')  // short circuit for condition "0\0"
        return false;
    if(word == '1')  //short circuit for condition "1\0"
        return true;
    duint value;
    if(valfromstring(expression, &value))
        return value != 0;
    return true;
}

void GuiSetDebugStateAsync(DBGSTATE state)
{
    static TaskThread_<decltype(&GuiSetDebugState), DBGSTATE> GuiSetDebugStateTask(&GuiSetDebugState);
    GuiSetDebugStateTask.WakeUp(state);
}

void cbPauseBreakpoint()
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    auto CIP = GetContextDataEx(hActiveThread, UE_CIP);
    DeleteBPX(CIP);
    DebugUpdateGuiSetStateAsync(CIP, true);
    _dbg_animatestop(); // Stop animating when paused
    // Trace record
    _dbg_dbgtraceexecute(CIP);
    //lock
    lock(WAITID_RUN);
    // Plugin callback
    PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
    plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
    SetForegroundWindow(GuiGetWindowHandle());
    wait(WAITID_RUN);
}

static void handleBreakCondition(const BREAKPOINT & bp, const void* ExceptionAddress, duint CIP, bool doBreak)
{
    if(doBreak)
    {
        if(bp.singleshoot)
            BpDelete(bp.addr, bp.type);
        if(!bp.silent)
        {
            switch(bp.type)
            {
            case BPNORMAL:
                printSoftBpInfo(bp);
                break;
            case BPHARDWARE:
                printHwBpInfo(bp);
                break;
            case BPMEMORY:
                printMemBpInfo(bp, ExceptionAddress);
                break;
            default:
                break;
            }
        }
        DebugUpdateGuiSetStateAsync(CIP, true);
        // Plugin callback
        PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
        plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
        _dbg_animatestop(); // Stop animating when a breakpoint is hit
    }
}

static void cbGenericBreakpoint(BP_TYPE bptype, void* ExceptionAddress = nullptr)
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    auto CIP = GetContextDataEx(hActiveThread, UE_CIP);
    BREAKPOINT* bpPtr = nullptr;
    SHARED_ACQUIRE(LockBreakpoints);
    switch(bptype)
    {
    case BPNORMAL:
        bpPtr = BpInfoFromAddr(bptype, CIP);
        break;
    case BPHARDWARE:
        bpPtr = BpInfoFromAddr(bptype, duint(ExceptionAddress));
        break;
    case BPMEMORY:
        bpPtr = BpInfoFromAddr(bptype, MemFindBaseAddr(duint(ExceptionAddress), nullptr, true));
    default:
        break;
    }
    if(!(bpPtr && bpPtr->enabled))  //invalid / disabled breakpoint hit (most likely a bug)
    {
        SHARED_RELEASE();
        dputs(QT_TRANSLATE_NOOP("DBG", "Breakpoint reached not in list!"));
        DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), true);
        //lock
        lock(WAITID_RUN);
        // Plugin callback
        PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
        plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
        SetForegroundWindow(GuiGetWindowHandle());
        bSkipExceptions = false;
        wait(WAITID_RUN);
        return;
    }

    // increment hit count
    InterlockedIncrement(&bpPtr->hitcount);

    auto bp = *bpPtr;
    SHARED_RELEASE();
    bp.addr += ModBaseFromAddr(CIP);
    bp.active = true; //a breakpoint that has been hit is active

    varset("$breakpointcounter", bp.hitcount, false); //save the breakpoint counter as a variable

    //get condition values
    bool breakCondition;
    bool logCondition;
    bool commandCondition;
    if(*bp.breakCondition)
        breakCondition = getConditionValue(bp.breakCondition);
    else
        breakCondition = true; //break if no condition is set
    if(bp.fastResume && !breakCondition)  // fast resume: ignore GUI/Script/Plugin/Other if the debugger would not break
        return;
    if(*bp.logCondition)
        logCondition = getConditionValue(bp.logCondition);
    else
        logCondition = true; //log if no condition is set
    if(*bp.commandCondition)
        commandCondition = getConditionValue(bp.commandCondition);
    else
        commandCondition = breakCondition; //if no condition is set, execute the command when the debugger would break

    lock(WAITID_RUN);
    handleBreakCondition(bp, ExceptionAddress, CIP, breakCondition);

    PLUG_CB_BREAKPOINT bpInfo;
    BRIDGEBP bridgebp;
    memset(&bridgebp, 0, sizeof(bridgebp));
    bpInfo.breakpoint = &bridgebp;
    BpToBridge(&bp, &bridgebp);
    plugincbcall(CB_BREAKPOINT, &bpInfo);

    // Trace record
    _dbg_dbgtraceexecute(CIP);

    // Watchdog
    cbWatchdog(0, nullptr);

    if(*bp.logText && logCondition)  //log
    {
        dprintf_untranslated("%s\n", stringformatinline(bp.logText).c_str());
    }
    if(*bp.commandText && commandCondition)  //command
    {
        //TODO: commands like run/step etc will fuck up your shit
        varset("$breakpointcondition", breakCondition ? 1 : 0, false);
        varset("$breakpointlogcondition", logCondition, false);
        _dbg_dbgcmddirectexec(bp.commandText);
        duint script_breakcondition;
        int size;
        VAR_TYPE type;
        if(varget("$breakpointcondition", &script_breakcondition, &size, &type))
        {
            if(script_breakcondition != 0)
            {
                handleBreakCondition(bp, ExceptionAddress, CIP, !breakCondition);
                breakCondition = true;
            }
            else
                breakCondition = false;
        }
    }
    if(breakCondition)  //break the debugger
    {
        SetForegroundWindow(GuiGetWindowHandle());
        bSkipExceptions = false;
    }
    else //resume immediately
        unlock(WAITID_RUN);

    //wait until the user resumes
    wait(WAITID_RUN);
}

void cbUserBreakpoint()
{
    cbGenericBreakpoint(BPNORMAL);
}

void cbHardwareBreakpoint(void* ExceptionAddress)
{
    cbGenericBreakpoint(BPHARDWARE, ExceptionAddress);
}

void cbMemoryBreakpoint(void* ExceptionAddress)
{
    cbGenericBreakpoint(BPMEMORY, ExceptionAddress);
}

void cbRunToUserCodeBreakpoint(void* ExceptionAddress)
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    auto CIP = GetContextDataEx(hActiveThread, UE_CIP);
    auto symbolicname = SymGetSymbolicName(CIP);
    dprintf(QT_TRANSLATE_NOOP("DBG", "User code reached at %s (%p)!"), symbolicname.c_str(), CIP);
    // lock
    lock(WAITID_RUN);
    // Trace record
    _dbg_dbgtraceexecute(CIP);
    // Update GUI
    DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), true);
    // Plugin callback
    PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
    plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
    SetForegroundWindow(GuiGetWindowHandle());
    bSkipExceptions = false;
    wait(WAITID_RUN);
}

void cbLibrarianBreakpoint(void* lpData)
{
    bBreakOnNextDll = true;
}

static BOOL CALLBACK SymRegisterCallbackProc64(HANDLE hProcess, ULONG ActionCode, ULONG64 CallbackData, ULONG64 UserContext)
{
    UNREFERENCED_PARAMETER(hProcess);
    UNREFERENCED_PARAMETER(UserContext);
    PIMAGEHLP_CBA_EVENT evt;
    switch(ActionCode)
    {
    case CBA_EVENT:
    {
        evt = (PIMAGEHLP_CBA_EVENT)CallbackData;
        auto strText = StringUtils::Utf16ToUtf8((const wchar_t*)evt->desc);
        const char* text = strText.c_str();
        if(strstr(text, "Successfully received a response from the server."))
            break;
        if(strstr(text, "Waiting for the server to respond to a request."))
            break;
        int len = (int)strlen(text);
        bool suspress = false;
        for(int i = 0; i < len; i++)
            if(text[i] == 0x08)
            {
                suspress = true;
                break;
            }
        int percent = 0;
        static bool zerobar = false;
        if(zerobar)
        {
            zerobar = false;
            GuiSymbolSetProgress(0);
        }
        if(strstr(text, " bytes -  "))
        {
            Memory<char*> newtext(len + 1, "SymRegisterCallbackProc64:newtext");
            strcpy_s(newtext(), len + 1, text);
            strstr(newtext(), " bytes -  ")[8] = 0;
            GuiSymbolLogAdd(newtext());
            suspress = true;
        }
        else if(strstr(text, " copied         "))
        {
            GuiSymbolSetProgress(100);
            GuiSymbolLogAdd(" downloaded!\n");
            suspress = true;
            zerobar = true;
        }
        else if(sscanf(text, "%*s %d percent", &percent) == 1 || sscanf(text, "%d percent", &percent) == 1)
        {
            GuiSymbolSetProgress(percent);
            suspress = true;
        }

        if(!suspress)
            GuiSymbolLogAdd(text);
    }
    break;

    case CBA_DEBUG_INFO:
    {
        GuiSymbolLogAdd((const char*)CallbackData);
    }
    break;

    default:
    {
        return FALSE;
    }
    }
    return TRUE;
}

bool cbSetModuleBreakpoints(const BREAKPOINT* bp)
{
    if(!bp->enabled)
        return true;
    switch(bp->type)
    {
    case BPNORMAL:
    {
        unsigned short oldbytes;
        if(MemRead(bp->addr, &oldbytes, sizeof(oldbytes)))
        {
            if(oldbytes != bp->oldbytes)
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "Breakpoint %p has been disabled because the bytes don't match! Expected: %02X %02X, Found: %02X %02X\n"),
                        bp->addr,
                        ((unsigned char*)&bp->oldbytes)[0], ((unsigned char*)&bp->oldbytes)[1],
                        ((unsigned char*)&oldbytes)[0], ((unsigned char*)&oldbytes)[1]);
                BpEnable(bp->addr, BPNORMAL, false);
            }
            else if(!SetBPX(bp->addr, bp->titantype, (void*)cbUserBreakpoint))
                dprintf(QT_TRANSLATE_NOOP("DBG", "Could not set breakpoint %p! (SetBPX)\n"), bp->addr);
        }
        else
            dprintf(QT_TRANSLATE_NOOP("DBG", "MemRead failed on breakpoint address%p!\n"), bp->addr);
    }
    break;

    case BPMEMORY:
    {
        duint size = 0;
        MemFindBaseAddr(bp->addr, &size);
        if(!SetMemoryBPXEx(bp->addr, size, bp->titantype, !bp->singleshoot, (void*)cbMemoryBreakpoint))
            dprintf(QT_TRANSLATE_NOOP("DBG", "Could not set memory breakpoint %p! (SetMemoryBPXEx)\n"), bp->addr);
    }
    break;

    case BPHARDWARE:
    {
        DWORD drx = 0;
        if(!GetUnusedHardwareBreakPointRegister(&drx))
        {
            dputs(QT_TRANSLATE_NOOP("DBG", "You can only set 4 hardware breakpoints"));
            return false;
        }
        int titantype = bp->titantype;
        TITANSETDRX(titantype, drx);
        BpSetTitanType(bp->addr, BPHARDWARE, titantype);
        if(!SetHardwareBreakPoint(bp->addr, drx, TITANGETTYPE(bp->titantype), TITANGETSIZE(bp->titantype), (void*)cbHardwareBreakpoint))
            dprintf(QT_TRANSLATE_NOOP("DBG", "Could not set hardware breakpoint %p! (SetHardwareBreakPoint)\n"), bp->addr);
        else
            dprintf(QT_TRANSLATE_NOOP("DBG", "Set hardware breakpoint on %p!\n"), bp->addr);
    }
    break;

    default:
        break;
    }
    return true;
}

EXCEPTION_DEBUG_INFO getLastExceptionInfo()
{
    return lastExceptionInfo;
}

static bool cbRemoveModuleBreakpoints(const BREAKPOINT* bp)
{
    if(!bp->enabled)
        return true;
    switch(bp->type)
    {
    case BPNORMAL:
        if(!DeleteBPX(bp->addr))
            dprintf(QT_TRANSLATE_NOOP("DBG", "Could not delete breakpoint %p! (DeleteBPX)\n"), bp->addr);
        break;
    case BPMEMORY:
        if(!RemoveMemoryBPX(bp->addr, 0))
            dprintf(QT_TRANSLATE_NOOP("DBG", "Could not delete memory breakpoint %p! (RemoveMemoryBPX)\n"), bp->addr);
        break;
    case BPHARDWARE:
        if(!DeleteHardwareBreakPoint(TITANGETDRX(bp->titantype)))
            dprintf(QT_TRANSLATE_NOOP("DBG", "Could not delete hardware breakpoint %p! (DeleteHardwareBreakPoint)\n"), bp->addr);
        break;
    default:
        break;
    }
    return true;
}

void cbStep()
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    isStepping = false;
    duint CIP = GetContextDataEx(hActiveThread, UE_CIP);
    DebugUpdateGuiSetStateAsync(CIP, true);
    // Trace record
    _dbg_dbgtraceexecute(CIP);
    // Plugin interaction
    PLUG_CB_STEPPED stepInfo;
    stepInfo.reserved = 0;
    //lock
    lock(WAITID_RUN);
    // Plugin callback
    PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
    plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
    SetForegroundWindow(GuiGetWindowHandle());
    bSkipExceptions = false;
    plugincbcall(CB_STEPPED, &stepInfo);
    wait(WAITID_RUN);
}

static void cbRtrFinalStep()
{
    dbgcleartracecondition();
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    duint CIP = GetContextDataEx(hActiveThread, UE_CIP);
    // Trace record
    _dbg_dbgtraceexecute(CIP);
    DebugUpdateGuiSetStateAsync(CIP, true);
    //lock
    lock(WAITID_RUN);
    // Plugin callback
    PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
    plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
    SetForegroundWindow(GuiGetWindowHandle());
    bSkipExceptions = false;
    wait(WAITID_RUN);
}

void cbRtrStep()
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    unsigned char ch = 0x90;
    duint cip = GetContextDataEx(hActiveThread, UE_CIP);
    MemRead(cip, &ch, 1);
    if(bTraceRecordEnabledDuringTrace)
        _dbg_dbgtraceexecute(cip);
    if(ch == 0xC3 || ch == 0xC2)
        cbRtrFinalStep();
    else if(ch == 0x26 || ch == 0x36 || ch == 0x2e || ch == 0x3e || (ch >= 0x64 && ch <= 0x67) || ch == 0xf2 || ch == 0xf3 //instruction prefixes
#ifdef _WIN64
            || (ch >= 0x40 && ch <= 0x4f)
#endif //_WIN64
           )
    {
        Capstone cp;
        unsigned char data[MAX_DISASM_BUFFER];
        memset(data, 0, sizeof(data));
        MemRead(cip, data, MAX_DISASM_BUFFER);
        cp.Disassemble(cip, data);
        if(cp.GetId() == X86_INS_RET)
            cbRtrFinalStep();
        else
            StepOver((void*)cbRtrStep);
    }
    else
    {
        StepOver((void*)cbRtrStep);
    }
}

static void cbTXCNDStep(bool bStepInto, void (*callback)())
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    if(traceCondition && traceCondition->ContinueTrace())
    {
        if(bTraceRecordEnabledDuringTrace)
            _dbg_dbgtraceexecute(GetContextDataEx(hActiveThread, UE_CIP));
        (bStepInto ? StepInto : StepOver)(callback);
    }
    else
    {
        auto steps = dbgcleartracecondition();
#ifdef _WIN64
        dprintf(QT_TRANSLATE_NOOP("DBG", "Trace finished after %llu steps!\n"), steps);
#else //x86
        dprintf(QT_TRANSLATE_NOOP("DBG", "Trace finished after %u steps!\n"), steps);
#endif //_WIN64
        cbRtrFinalStep();
    }
}

void cbTOCNDStep()
{
    cbTXCNDStep(false, cbTOCNDStep);
}

void cbTICNDStep()
{
    cbTXCNDStep(true, cbTICNDStep);
}

static void cbTXXTStep(bool bStepInto, bool bInto, void (*callback)())
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    // Trace record
    duint CIP = GetContextDataEx(hActiveThread, UE_CIP);
    if(!traceCondition)
    {
        _dbg_dbgtraceexecute(CIP);
        dprintf(QT_TRANSLATE_NOOP("DBG", "Bad tracing state.\n"));
        cbRtrFinalStep();
        return;
    }
    if(!traceCondition->ContinueTrace() || (TraceRecord.getTraceRecordType(CIP) != TraceRecordManager::TraceRecordNone && (TraceRecord.getHitCount(CIP) == 0) ^ bInto))
    {
        _dbg_dbgtraceexecute(CIP);
        auto steps = dbgcleartracecondition();
#ifdef _WIN64
        dprintf(QT_TRANSLATE_NOOP("DBG", "Trace finished after %llu steps!\n"), steps);
#else //x86
        dprintf(QT_TRANSLATE_NOOP("DBG", "Trace finished after %u steps!\n"), steps);
#endif //_WIN64
        cbRtrFinalStep();
        return;
    }
    if(bTraceRecordEnabledDuringTrace)
        _dbg_dbgtraceexecute(CIP);
    (bStepInto ? StepInto : StepOver)(callback);
}

void cbTIBTStep()
{
    cbTXXTStep(true, false, cbTIBTStep);
}

void cbTOBTStep()
{
    cbTXXTStep(false, false, cbTOBTStep);
}

void cbTIITStep()
{
    cbTXXTStep(true, true, cbTIITStep);
}

void cbTOITStep()
{
    cbTXXTStep(false, true, cbTOITStep);
}

static void cbCreateProcess(CREATE_PROCESS_DEBUG_INFO* CreateProcessInfo)
{
    void* base = CreateProcessInfo->lpBaseOfImage;

    char DebugFileName[deflen] = "";
    if(!GetFileNameFromHandle(CreateProcessInfo->hFile, DebugFileName) && !GetFileNameFromProcessHandle(CreateProcessInfo->hProcess, DebugFileName))
        strcpy_s(DebugFileName, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "??? (GetFileNameFromHandle failed)")));
    dprintf(QT_TRANSLATE_NOOP("DBG", "Process Started: %p %s\n"), base, DebugFileName);

    //update memory map
    MemUpdateMap();
    GuiUpdateMemoryView();

    GuiDumpAt(MemFindBaseAddr(GetContextDataEx(CreateProcessInfo->hThread, UE_CIP), 0) + PAGE_SIZE); //dump somewhere

    // Init program database
    DbLoad(DbLoadSaveType::DebugData);

    SafeSymSetOptions(SYMOPT_IGNORE_CVREC | SYMOPT_DEBUG | SYMOPT_LOAD_LINES | SYMOPT_ALLOW_ABSOLUTE_SYMBOLS | SYMOPT_FAVOR_COMPRESSED | SYMOPT_IGNORE_NT_SYMPATH);
    GuiSymbolLogClear();
    char szServerSearchPath[MAX_PATH * 2] = "";
    sprintf_s(szServerSearchPath, "SRV*%s", szSymbolCachePath);
    SafeSymInitializeW(fdProcessInfo->hProcess, StringUtils::Utf8ToUtf16(szServerSearchPath).c_str(), false); //initialize symbols
    SafeSymRegisterCallbackW64(fdProcessInfo->hProcess, SymRegisterCallbackProc64, 0);
    SafeSymLoadModuleExW(fdProcessInfo->hProcess, CreateProcessInfo->hFile, StringUtils::Utf8ToUtf16(DebugFileName).c_str(), 0, (DWORD64)base, 0, 0, 0);

    IMAGEHLP_MODULEW64 modInfo;
    memset(&modInfo, 0, sizeof(modInfo));
    modInfo.SizeOfStruct = sizeof(modInfo);
    if(SafeSymGetModuleInfoW64(fdProcessInfo->hProcess, (DWORD64)base, &modInfo))
        ModLoad((duint)base, modInfo.ImageSize, StringUtils::Utf16ToUtf8(modInfo.ImageName).c_str());

    char modname[256] = "";
    if(ModNameFromAddr((duint)base, modname, true))
        BpEnumAll(cbSetModuleBreakpoints, modname, duint(base));
    BpEnumAll(cbSetModuleBreakpoints, "");
    GuiUpdateBreakpointsView();
    pCreateProcessBase = (duint)CreateProcessInfo->lpBaseOfImage;
    if(!bFileIsDll && !bIsAttached) //Set entry breakpoint
    {
        pDebuggedBase = pCreateProcessBase; //debugged base = executable
        char command[deflen] = "";

        if(settingboolget("Events", "TlsCallbacks"))
        {
            DWORD NumberOfCallBacks = 0;
            TLSGrabCallBackDataW(StringUtils::Utf8ToUtf16(DebugFileName).c_str(), 0, &NumberOfCallBacks);
            if(NumberOfCallBacks)
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "TLS Callbacks: %d\n"), NumberOfCallBacks);
                Memory<duint*> TLSCallBacks(NumberOfCallBacks * sizeof(duint), "cbCreateProcess:TLSCallBacks");
                if(!TLSGrabCallBackDataW(StringUtils::Utf8ToUtf16(DebugFileName).c_str(), TLSCallBacks(), &NumberOfCallBacks))
                    dputs(QT_TRANSLATE_NOOP("DBG", "Failed to get TLS callback addresses!"));
                else
                {
                    duint ImageBase = GetPE32DataW(StringUtils::Utf8ToUtf16(DebugFileName).c_str(), 0, UE_IMAGEBASE);
                    int invalidCount = 0;
                    for(unsigned int i = 0; i < NumberOfCallBacks; i++)
                    {
                        duint callbackVA = TLSCallBacks()[i] - ImageBase + pDebuggedBase;
                        if(MemIsValidReadPtr(callbackVA))
                        {
                            String breakpointname = StringUtils::sprintf(GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "TLS Callback %d")), i + 1);
                            sprintf_s(command, "bp %p,\"%s\",ss", callbackVA, breakpointname.c_str());
                            cmddirectexec(command);
                        }
                        else
                            invalidCount++;
                    }
                    if(invalidCount)
                        dprintf(QT_TRANSLATE_NOOP("DBG", "%d invalid TLS callback addresses...\n"), invalidCount);
                }
            }
        }

        if(settingboolget("Events", "EntryBreakpoint"))
        {
            sprintf_s(command, "bp %p,\"%s\",ss", (duint)CreateProcessInfo->lpStartAddress, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "entry breakpoint")));
            cmddirectexec(command);
        }

        bTraceRecordEnabledDuringTrace = settingboolget("Engine", "TraceRecordEnabledDuringTrace");
    }
    GuiUpdateBreakpointsView();

    //call plugin callback
    PLUG_CB_CREATEPROCESS callbackInfo;
    callbackInfo.CreateProcessInfo = CreateProcessInfo;
    IMAGEHLP_MODULE64 modInfoUtf8;
    memset(&modInfoUtf8, 0, sizeof(modInfoUtf8));
    modInfoUtf8.SizeOfStruct = sizeof(modInfoUtf8);
    modInfoUtf8.BaseOfImage = modInfo.BaseOfImage;
    modInfoUtf8.ImageSize = modInfo.ImageSize;
    modInfoUtf8.TimeDateStamp = modInfo.TimeDateStamp;
    modInfoUtf8.CheckSum = modInfo.CheckSum;
    modInfoUtf8.NumSyms = modInfo.NumSyms;
    modInfoUtf8.SymType = modInfo.SymType;
    strncpy_s(modInfoUtf8.ModuleName, StringUtils::Utf16ToUtf8(modInfo.ModuleName).c_str(), _TRUNCATE);
    strncpy_s(modInfoUtf8.ImageName, StringUtils::Utf16ToUtf8(modInfo.ImageName).c_str(), _TRUNCATE);
    strncpy_s(modInfoUtf8.LoadedImageName, StringUtils::Utf16ToUtf8(modInfo.LoadedImageName).c_str(), _TRUNCATE);
    strncpy_s(modInfoUtf8.LoadedPdbName, StringUtils::Utf16ToUtf8(modInfo.LoadedPdbName).c_str(), _TRUNCATE);
    modInfoUtf8.CVSig = modInfo.CVSig;
    strncpy_s(modInfoUtf8.CVData, StringUtils::Utf16ToUtf8(modInfo.CVData).c_str(), _TRUNCATE);
    modInfoUtf8.PdbSig = modInfo.PdbSig;
    modInfoUtf8.PdbSig70 = modInfo.PdbSig70;
    modInfoUtf8.PdbAge = modInfo.PdbAge;
    modInfoUtf8.PdbUnmatched = modInfo.PdbUnmatched;
    modInfoUtf8.DbgUnmatched = modInfo.DbgUnmatched;
    modInfoUtf8.LineNumbers = modInfo.LineNumbers;
    modInfoUtf8.GlobalSymbols = modInfo.GlobalSymbols;
    modInfoUtf8.TypeInfo = modInfo.TypeInfo;
    modInfoUtf8.SourceIndexed = modInfo.SourceIndexed;
    modInfoUtf8.Publics = modInfo.Publics;
    callbackInfo.modInfo = &modInfoUtf8;
    callbackInfo.DebugFileName = DebugFileName;
    callbackInfo.fdProcessInfo = fdProcessInfo;
    plugincbcall(CB_CREATEPROCESS, &callbackInfo);

    //update thread list
    CREATE_THREAD_DEBUG_INFO threadInfo;
    threadInfo.hThread = CreateProcessInfo->hThread;
    threadInfo.lpStartAddress = CreateProcessInfo->lpStartAddress;
    threadInfo.lpThreadLocalBase = CreateProcessInfo->lpThreadLocalBase;
    ThreadCreate(&threadInfo);
}

static void cbExitProcess(EXIT_PROCESS_DEBUG_INFO* ExitProcess)
{
    dprintf(QT_TRANSLATE_NOOP("DBG", "Process stopped with exit code 0x%X\n"), ExitProcess->dwExitCode);
    PLUG_CB_EXITPROCESS callbackInfo;
    callbackInfo.ExitProcess = ExitProcess;
    plugincbcall(CB_EXITPROCESS, &callbackInfo);
    _dbg_animatestop(); // Stop animating
    //unload main module
    SafeSymUnloadModule64(fdProcessInfo->hProcess, pCreateProcessBase);
    //history
    dbgcleartracecondition();
    dbgClearRtuBreakpoints();
    HistoryClear();
    ModClear(); //clear all modules
}

static void cbCreateThread(CREATE_THREAD_DEBUG_INFO* CreateThread)
{
    ThreadCreate(CreateThread); //update thread list
    DWORD dwThreadId = ((DEBUG_EVENT*)GetDebugData())->dwThreadId;
    hActiveThread = ThreadGetHandle(dwThreadId);

    if(settingboolget("Events", "ThreadEntry"))
    {
        String command;
        command = StringUtils::sprintf("bp %p,\"%s %X\",ss", (duint)CreateThread->lpStartAddress, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "Thread")), dwThreadId);
        cmddirectexec(command.c_str());
    }

    PLUG_CB_CREATETHREAD callbackInfo;
    callbackInfo.CreateThread = CreateThread;
    callbackInfo.dwThreadId = dwThreadId;
    plugincbcall(CB_CREATETHREAD, &callbackInfo);

    dprintf(QT_TRANSLATE_NOOP("DBG", "Thread %X created, Entry: %p\n"), dwThreadId, CreateThread->lpStartAddress);

    if(settingboolget("Events", "ThreadStart"))
    {
        HistoryClear();
        //update memory map
        MemUpdateMap();
        //update GUI
        DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), true);
        //lock
        lock(WAITID_RUN);
        // Plugin callback
        PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
        plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
        SetForegroundWindow(GuiGetWindowHandle());
        wait(WAITID_RUN);
    }
}

static void cbExitThread(EXIT_THREAD_DEBUG_INFO* ExitThread)
{
    // Not called when the main (last) thread exits. Instead
    // EXIT_PROCESS_DEBUG_EVENT is signalled.
    // Switch to the main thread (because the thread is terminated).
    hActiveThread = ThreadGetHandle(fdProcessInfo->dwThreadId);
    if(!hActiveThread)
    {
        std::vector<THREADINFO> threads;
        ThreadGetList(threads);
        if(!threads.size())
            dputs(QT_TRANSLATE_NOOP("DBG", "No threads left to switch to (bug?)"));
        hActiveThread = threads[0].Handle;
    }
    DWORD dwThreadId = ((DEBUG_EVENT*)GetDebugData())->dwThreadId;
    PLUG_CB_EXITTHREAD callbackInfo;
    callbackInfo.ExitThread = ExitThread;
    callbackInfo.dwThreadId = dwThreadId;
    plugincbcall(CB_EXITTHREAD, &callbackInfo);
    HistoryClear();
    ThreadExit(dwThreadId);
    dprintf(QT_TRANSLATE_NOOP("DBG", "Thread %X exit\n"), dwThreadId);

    if(settingboolget("Events", "ThreadEnd"))
    {
        //update GUI
        DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), true);
        //lock
        lock(WAITID_RUN);
        // Plugin callback
        PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
        plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
        SetForegroundWindow(GuiGetWindowHandle());
        wait(WAITID_RUN);
    }
}

static void cbSystemBreakpoint(void* ExceptionData) // TODO: System breakpoint event shouldn't be dropped
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);

    // Update GUI (this should be the first triggered event)
    duint cip = GetContextDataEx(hActiveThread, UE_CIP);
    GuiDumpAt(MemFindBaseAddr(cip, 0, true)); //dump somewhere
    DebugUpdateGuiSetStateAsync(cip, true, running);

    //log message
    if(bIsAttached)
        dputs(QT_TRANSLATE_NOOP("DBG", "Attach breakpoint reached!"));
    else
        dputs(QT_TRANSLATE_NOOP("DBG", "System breakpoint reached!"));
    bSkipExceptions = false; //we are not skipping first-chance exceptions

    //plugin callbacks
    PLUG_CB_SYSTEMBREAKPOINT callbackInfo;
    callbackInfo.reserved = 0;
    plugincbcall(CB_SYSTEMBREAKPOINT, &callbackInfo);

    lock(WAITID_RUN); // Allow the user to run a script file now
    if(bIsAttached ? settingboolget("Events", "AttachBreakpoint") : settingboolget("Events", "SystemBreakpoint"))
    {
        //lock
        GuiSetDebugStateAsync(paused);
        // Plugin callback
        PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
        plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
        SetForegroundWindow(GuiGetWindowHandle());
        char script[MAX_SETTING_SIZE];
        if(BridgeSettingGet("Engine", "InitializeScript", script)) // Global script file
        {
            if(scriptLoadSync(script) == 0)
                scriptRunSync((void*)0);
            else
                dputs(QT_TRANSLATE_NOOP("DBG", "Error: Cannot load global initialization script."));
        }
        if(szDebuggeeInitializationScript[0] != 0)
        {
            if(scriptLoadSync(szDebuggeeInitializationScript) == 0)
                scriptRunSync((void*)0);
            else
                dputs(QT_TRANSLATE_NOOP("DBG", "Error: Cannot load debuggee initialization script."));
        }
    }
    else
    {
        char script[MAX_SETTING_SIZE];
        if(BridgeSettingGet("Engine", "InitializeScript", script)) // Global script file
        {
            if(scriptLoadSync(script) == 0)
                scriptRunSync((void*)0);
            else
                dputs(QT_TRANSLATE_NOOP("DBG", "Error: Cannot load global initialization script."));
        }
        if(szDebuggeeInitializationScript[0] != 0)
        {
            if(scriptLoadSync(szDebuggeeInitializationScript) == 0)
                scriptRunSync((void*)0);
            else
                dputs(QT_TRANSLATE_NOOP("DBG", "Error: Cannot load debuggee initialization script."));
        }
        unlock(WAITID_RUN);
    }
    wait(WAITID_RUN);
}

static void cbLoadDll(LOAD_DLL_DEBUG_INFO* LoadDll)
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    void* base = LoadDll->lpBaseOfDll;

    char DLLDebugFileName[deflen] = "";
    if(!GetFileNameFromHandle(LoadDll->hFile, DLLDebugFileName))
        strcpy_s(DLLDebugFileName, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "??? (GetFileNameFromHandle failed)")));

    SafeSymLoadModuleExW(fdProcessInfo->hProcess, LoadDll->hFile, StringUtils::Utf8ToUtf16(DLLDebugFileName).c_str(), 0, (DWORD64)base, 0, 0, 0);
    IMAGEHLP_MODULEW64 modInfo;
    memset(&modInfo, 0, sizeof(modInfo));
    modInfo.SizeOfStruct = sizeof(modInfo);
    if(SafeSymGetModuleInfoW64(fdProcessInfo->hProcess, (DWORD64)base, &modInfo))
        ModLoad((duint)base, modInfo.ImageSize, StringUtils::Utf16ToUtf8(modInfo.ImageName).c_str());

    // Update memory map
    MemUpdateMapAsync();

    char modname[256] = "";
    if(ModNameFromAddr((duint)base, modname, true))
        BpEnumAll(cbSetModuleBreakpoints, modname, duint(base));
    GuiUpdateBreakpointsView();
    bool bAlreadySetEntry = false;

    char command[256] = "";
    bool bIsDebuggingThis = false;
    if(bFileIsDll && !_stricmp(DLLDebugFileName, szFileName) && !bIsAttached) //Set entry breakpoint
    {
        bIsDebuggingThis = true;
        pDebuggedBase = (duint)base;
        if(settingboolget("Events", "EntryBreakpoint"))
        {
            bAlreadySetEntry = true;
            sprintf_s(command, "bp %p,\"%s\",ss", pDebuggedBase + pDebuggedEntry, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "entry breakpoint")));
            cmddirectexec(command);
        }
    }
    GuiUpdateBreakpointsView();

    if(settingboolget("Events", "TlsCallbacks"))
    {
        DWORD NumberOfCallBacks = 0;
        TLSGrabCallBackDataW(StringUtils::Utf8ToUtf16(DLLDebugFileName).c_str(), 0, &NumberOfCallBacks);
        if(NumberOfCallBacks)
        {
            dprintf(QT_TRANSLATE_NOOP("DBG", "TLS Callbacks: %d\n"), NumberOfCallBacks);
            Memory<duint*> TLSCallBacks(NumberOfCallBacks * sizeof(duint), "cbLoadDll:TLSCallBacks");
            if(!TLSGrabCallBackDataW(StringUtils::Utf8ToUtf16(DLLDebugFileName).c_str(), TLSCallBacks(), &NumberOfCallBacks))
                dputs(QT_TRANSLATE_NOOP("DBG", "Failed to get TLS callback addresses!"));
            else
            {
                duint ImageBase = GetPE32DataW(StringUtils::Utf8ToUtf16(DLLDebugFileName).c_str(), 0, UE_IMAGEBASE);
                int invalidCount = 0;
                for(unsigned int i = 0; i < NumberOfCallBacks; i++)
                {
                    duint callbackVA = TLSCallBacks()[i] - ImageBase + (duint)base;
                    if(MemIsValidReadPtr(callbackVA))
                    {
                        if(bIsDebuggingThis)
                            sprintf_s(command, "bp %p,\"%s %d\",ss", callbackVA, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "TLS Callback")), i + 1);
                        else
                            sprintf_s(command, "bp %p,\"%s %d (%s)\",ss", callbackVA, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "TLS Callback")), i + 1, modname);
                        cmddirectexec(command);
                    }
                    else
                        invalidCount++;
                }
                if(invalidCount)
                    dprintf(QT_TRANSLATE_NOOP("DBG", "%d invalid TLS callback addresses...\n"), invalidCount);
            }
        }
    }

    if((bBreakOnNextDll || settingboolget("Events", "DllEntry")) && !bAlreadySetEntry)
    {
        duint oep = GetPE32DataW(StringUtils::Utf8ToUtf16(DLLDebugFileName).c_str(), 0, UE_OEP);
        if(oep)
        {
            char command[256] = "";
            sprintf_s(command, "bp %p,\"DllMain (%s)\",ss", oep + (duint)base, modname);
            cmddirectexec(command);
        }
    }

    dprintf(QT_TRANSLATE_NOOP("DBG", "DLL Loaded: %p %s\n"), base, DLLDebugFileName);

    //plugin callback
    PLUG_CB_LOADDLL callbackInfo;
    callbackInfo.LoadDll = LoadDll;
    IMAGEHLP_MODULE64 modInfoUtf8;
    memset(&modInfoUtf8, 0, sizeof(modInfoUtf8));
    modInfoUtf8.SizeOfStruct = sizeof(modInfoUtf8);
    modInfoUtf8.BaseOfImage = modInfo.BaseOfImage;
    modInfoUtf8.ImageSize = modInfo.ImageSize;
    modInfoUtf8.TimeDateStamp = modInfo.TimeDateStamp;
    modInfoUtf8.CheckSum = modInfo.CheckSum;
    modInfoUtf8.NumSyms = modInfo.NumSyms;
    modInfoUtf8.SymType = modInfo.SymType;
    strncpy_s(modInfoUtf8.ModuleName, StringUtils::Utf16ToUtf8(modInfo.ModuleName).c_str(), _TRUNCATE);
    strncpy_s(modInfoUtf8.ImageName, StringUtils::Utf16ToUtf8(modInfo.ImageName).c_str(), _TRUNCATE);
    strncpy_s(modInfoUtf8.LoadedImageName, StringUtils::Utf16ToUtf8(modInfo.LoadedImageName).c_str(), _TRUNCATE);
    strncpy_s(modInfoUtf8.LoadedPdbName, StringUtils::Utf16ToUtf8(modInfo.LoadedPdbName).c_str(), _TRUNCATE);
    modInfoUtf8.CVSig = modInfo.CVSig;
    strncpy_s(modInfoUtf8.CVData, StringUtils::Utf16ToUtf8(modInfo.CVData).c_str(), _TRUNCATE);
    modInfoUtf8.PdbSig = modInfo.PdbSig;
    modInfoUtf8.PdbSig70 = modInfo.PdbSig70;
    modInfoUtf8.PdbAge = modInfo.PdbAge;
    modInfoUtf8.PdbUnmatched = modInfo.PdbUnmatched;
    modInfoUtf8.DbgUnmatched = modInfo.DbgUnmatched;
    modInfoUtf8.LineNumbers = modInfo.LineNumbers;
    modInfoUtf8.GlobalSymbols = modInfo.GlobalSymbols;
    modInfoUtf8.TypeInfo = modInfo.TypeInfo;
    modInfoUtf8.SourceIndexed = modInfo.SourceIndexed;
    modInfoUtf8.Publics = modInfo.Publics;
    callbackInfo.modInfo = &modInfoUtf8;
    callbackInfo.modname = modname;
    plugincbcall(CB_LOADDLL, &callbackInfo);

    if(bBreakOnNextDll || settingboolget("Events", "DllLoad"))
    {
        bBreakOnNextDll = false;
        //update GUI
        DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), true);
        //lock
        lock(WAITID_RUN);
        // Plugin callback
        PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
        plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
        SetForegroundWindow(GuiGetWindowHandle());
        wait(WAITID_RUN);
    }
}

static void cbUnloadDll(UNLOAD_DLL_DEBUG_INFO* UnloadDll)
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    PLUG_CB_UNLOADDLL callbackInfo;
    callbackInfo.UnloadDll = UnloadDll;
    plugincbcall(CB_UNLOADDLL, &callbackInfo);

    void* base = UnloadDll->lpBaseOfDll;
    char modname[256] = "???";
    if(ModNameFromAddr((duint)base, modname, true))
        BpEnumAll(cbRemoveModuleBreakpoints, modname, duint(base));
    GuiUpdateBreakpointsView();
    SafeSymUnloadModule64(fdProcessInfo->hProcess, (DWORD64)base);
    dprintf(QT_TRANSLATE_NOOP("DBG", "DLL Unloaded: %p %s\n"), base, modname);

    if(bBreakOnNextDll || settingboolget("Events", "DllUnload"))
    {
        bBreakOnNextDll = false;
        //update GUI
        DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), true);
        //lock
        lock(WAITID_RUN);
        // Plugin callback
        PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
        plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
        SetForegroundWindow(GuiGetWindowHandle());
        wait(WAITID_RUN);
    }

    ModUnload((duint)base);

    //update memory map
    MemUpdateMapAsync();
}

static void cbOutputDebugString(OUTPUT_DEBUG_STRING_INFO* DebugString)
{

    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    PLUG_CB_OUTPUTDEBUGSTRING callbackInfo;
    callbackInfo.DebugString = DebugString;
    plugincbcall(CB_OUTPUTDEBUGSTRING, &callbackInfo);

    if(!DebugString->fUnicode) //ASCII
    {
        Memory<char*> DebugText(DebugString->nDebugStringLength + 1, "cbOutputDebugString:DebugText");
        if(MemRead((duint)DebugString->lpDebugStringData, DebugText(), DebugString->nDebugStringLength))
        {
            String str = String(DebugText());
            if(str != lastDebugText)  //fix for every string being printed twice
            {
                if(str != "\n")
                    dprintf(QT_TRANSLATE_NOOP("DBG", "DebugString: \"%s\"\n"), StringUtils::Escape(str).c_str());
                lastDebugText = str;
            }
            else
                lastDebugText.clear();
        }
    }

    if(settingboolget("Events", "DebugStrings"))
    {
        //update GUI
        DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), true);
        //lock
        lock(WAITID_RUN);
        // Plugin callback
        PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
        plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
        SetForegroundWindow(GuiGetWindowHandle());
        wait(WAITID_RUN);
    }
}

static void cbException(EXCEPTION_DEBUG_INFO* ExceptionData)
{
    hActiveThread = ThreadGetHandle(((DEBUG_EVENT*)GetDebugData())->dwThreadId);
    PLUG_CB_EXCEPTION callbackInfo;
    callbackInfo.Exception = ExceptionData;
    unsigned int ExceptionCode = ExceptionData->ExceptionRecord.ExceptionCode;
    GuiSetLastException(ExceptionCode);
    if(ExceptionData)
        lastExceptionInfo = *ExceptionData;

    duint addr = (duint)ExceptionData->ExceptionRecord.ExceptionAddress;
    if(ExceptionData->ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT)
    {
        if(isDetachedByUser)
        {
            PLUG_CB_DETACH detachInfo;
            detachInfo.fdProcessInfo = fdProcessInfo;
            plugincbcall(CB_DETACH, &detachInfo);
            if(!DetachDebuggerEx(fdProcessInfo->dwProcessId))
                dputs(QT_TRANSLATE_NOOP("DBG", "DetachDebuggerEx failed..."));
            else
                dputs(QT_TRANSLATE_NOOP("DBG", "Detached!"));
            isDetachedByUser = false;
            _dbg_animatestop(); // Stop animating
            return;
        }
        else if(isPausedByUser)
        {
            dputs(QT_TRANSLATE_NOOP("DBG", "paused!"));
            SetNextDbgContinueStatus(DBG_CONTINUE);
            _dbg_animatestop(); // Stop animating
            //update memory map
            MemUpdateMap();
            DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), true);
            // Clear tracing conditions
            dbgcleartracecondition();
            dbgClearRtuBreakpoints();
            //lock
            lock(WAITID_RUN);
            // Plugin callback
            PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
            plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
            SetForegroundWindow(GuiGetWindowHandle());
            bSkipExceptions = false;
            plugincbcall(CB_EXCEPTION, &callbackInfo);
            wait(WAITID_RUN);
            return;
        }
        SetContextDataEx(hActiveThread, UE_CIP, (duint)ExceptionData->ExceptionRecord.ExceptionAddress);
    }
    else if(ExceptionData->ExceptionRecord.ExceptionCode == MS_VC_EXCEPTION) //SetThreadName exception
    {
        THREADNAME_INFO nameInfo; //has no valid local pointers
        memcpy(&nameInfo, ExceptionData->ExceptionRecord.ExceptionInformation, sizeof(THREADNAME_INFO));
        if(nameInfo.dwThreadID == -1) //current thread
            nameInfo.dwThreadID = ((DEBUG_EVENT*)GetDebugData())->dwThreadId;
        if(nameInfo.dwType == 0x1000 && nameInfo.dwFlags == 0 && ThreadIsValid(nameInfo.dwThreadID)) //passed basic checks
        {
            Memory<char*> ThreadName(MAX_THREAD_NAME_SIZE, "cbException:ThreadName");
            if(MemRead((duint)nameInfo.szName, ThreadName(), MAX_THREAD_NAME_SIZE - 1))
            {
                String ThreadNameEscaped = StringUtils::Escape(ThreadName());
                dprintf(QT_TRANSLATE_NOOP("DBG", "SetThreadName(%X, \"%s\")\n"), nameInfo.dwThreadID, ThreadNameEscaped.c_str());
                ThreadSetName(nameInfo.dwThreadID, ThreadNameEscaped.c_str());
            }
        }
    }
    auto exceptionName = ExceptionCodeToName(ExceptionCode);
    if(!exceptionName.size())  //if no exception was found, try the error codes (RPC_S_*)
        exceptionName = ErrorCodeToName(ExceptionCode);
    if(ExceptionData->dwFirstChance) //first chance exception
    {
        if(exceptionName.size())
            dprintf(QT_TRANSLATE_NOOP("DBG", "First chance exception on %p (%.8X, %s)!\n"), addr, ExceptionCode, exceptionName.c_str());
        else
            dprintf(QT_TRANSLATE_NOOP("DBG", "First chance exception on %p (%.8X)!\n"), addr, ExceptionCode);
        SetNextDbgContinueStatus(DBG_EXCEPTION_NOT_HANDLED);
        if(bSkipExceptions || dbgisignoredexception(ExceptionCode))
            return;
    }
    else //lock the exception
    {
        if(exceptionName.size())
            dprintf(QT_TRANSLATE_NOOP("DBG", "Last chance exception on %p (%.8X, %s)!\n"), addr, ExceptionCode, exceptionName.c_str());
        else
            dprintf(QT_TRANSLATE_NOOP("DBG", "Last chance exception on %p (%.8X)!\n"), addr, ExceptionCode);
        SetNextDbgContinueStatus(DBG_CONTINUE);
    }

    DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), true);
    //lock
    lock(WAITID_RUN);
    // Plugin callback
    PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
    plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
    SetForegroundWindow(GuiGetWindowHandle());
    bSkipExceptions = false;
    plugincbcall(CB_EXCEPTION, &callbackInfo);
    wait(WAITID_RUN);
}

static void cbDebugEvent(DEBUG_EVENT* DebugEvent)
{
    InterlockedIncrement(&DbgEvents);
    PLUG_CB_DEBUGEVENT debugEventInfo;
    debugEventInfo.DebugEvent = DebugEvent;
    plugincbcall(CB_DEBUGEVENT, &debugEventInfo);
}

bool cbDeleteAllBreakpoints(const BREAKPOINT* bp)
{
    if(bp->type != BPNORMAL)
        return true;
    if(!BpDelete(bp->addr, BPNORMAL))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Delete breakpoint failed (BpDelete): %p\n"), bp->addr);
        return false;
    }
    if(bp->enabled && !DeleteBPX(bp->addr))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Delete breakpoint failed (DeleteBPX): %p\n"), bp->addr);
        return false;
    }
    return true;
}

bool cbEnableAllBreakpoints(const BREAKPOINT* bp)
{
    if(bp->type != BPNORMAL || bp->enabled)
        return true;

    if(!SetBPX(bp->addr, bp->titantype, (void*)cbUserBreakpoint))
    {
        if(!MemIsValidReadPtr(bp->addr))
            return true;
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not enable breakpoint %p (SetBPX)\n"), bp->addr);
        return false;
    }
    if(!BpEnable(bp->addr, BPNORMAL, true))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not enable breakpoint %p (BpEnable)\n"), bp->addr);
        return false;
    }
    return true;
}

bool cbDisableAllBreakpoints(const BREAKPOINT* bp)
{
    if(bp->type != BPNORMAL || !bp->enabled)
        return true;

    if(!BpEnable(bp->addr, BPNORMAL, false))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not disable breakpoint %p (BpEnable)\n"), bp->addr);
        return false;
    }
    if(!DeleteBPX(bp->addr))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not disable breakpoint %p (DeleteBPX)\n"), bp->addr);
        return false;
    }
    return true;
}

bool cbEnableAllHardwareBreakpoints(const BREAKPOINT* bp)
{
    if(bp->type != BPHARDWARE || bp->enabled)
        return true;
    DWORD drx = 0;
    if(!GetUnusedHardwareBreakPointRegister(&drx))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Did not enable hardware breakpoint %p (all slots full)\n"), bp->addr);
        return true;
    }
    int titantype = bp->titantype;
    TITANSETDRX(titantype, drx);
    BpSetTitanType(bp->addr, BPHARDWARE, titantype);
    if(!BpEnable(bp->addr, BPHARDWARE, true))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not enable hardware breakpoint %p (BpEnable)\n"), bp->addr);
        return false;
    }
    if(!SetHardwareBreakPoint(bp->addr, drx, TITANGETTYPE(bp->titantype), TITANGETSIZE(bp->titantype), (void*)cbHardwareBreakpoint))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not enable hardware breakpoint %p (SetHardwareBreakPoint)\n"), bp->addr);
        return false;
    }
    return true;
}

bool cbDisableAllHardwareBreakpoints(const BREAKPOINT* bp)
{
    if(bp->type != BPHARDWARE)
        return true;
    if(!BpEnable(bp->addr, BPHARDWARE, false))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not disable hardware breakpoint %p (BpEnable)\n"), bp->addr);
        return false;
    }
    if(bp->enabled && !DeleteHardwareBreakPoint(TITANGETDRX(bp->titantype)))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not disable hardware breakpoint %p (DeleteHardwareBreakPoint)\n"), bp->addr);
        return false;
    }
    return true;
}

bool cbEnableAllMemoryBreakpoints(const BREAKPOINT* bp)
{
    if(bp->type != BPMEMORY || bp->enabled)
        return true;
    duint size = 0;
    MemFindBaseAddr(bp->addr, &size);
    if(!BpEnable(bp->addr, BPMEMORY, true))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not enable memory breakpoint %p (BpEnable)\n"), bp->addr);
        return false;
    }
    if(!SetMemoryBPXEx(bp->addr, size, bp->titantype, !bp->singleshoot, (void*)cbMemoryBreakpoint))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not enable memory breakpoint %p (SetMemoryBPXEx)\n"), bp->addr);
        return false;
    }
    return true;
}

bool cbDisableAllMemoryBreakpoints(const BREAKPOINT* bp)
{
    if(bp->type != BPMEMORY || !bp->enabled)
        return true;
    if(!BpEnable(bp->addr, BPMEMORY, false))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not disable memory breakpoint %p (BpEnable)\n"), bp->addr);
        return false;
    }
    if(!RemoveMemoryBPX(bp->addr, 0))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Could not disable memory breakpoint %p (RemoveMemoryBPX)\n"), bp->addr);
        return false;
    }
    return true;
}

bool cbBreakpointList(const BREAKPOINT* bp)
{
    const char* type = 0;
    if(bp->type == BPNORMAL)
    {
        if(bp->singleshoot)
            type = "SS";
        else
            type = "BP";
    }
    else if(bp->type == BPHARDWARE)
        type = "HW";
    else if(bp->type == BPMEMORY)
        type = "GP";
    bool enabled = bp->enabled;
    if(*bp->name)
        dprintf_untranslated("%d:%s:%p:\"%s\"\n", enabled, type, bp->addr, bp->name);
    else
        dprintf_untranslated("%d:%s:%p\n", enabled, type, bp->addr);
    return true;
}

bool cbDeleteAllMemoryBreakpoints(const BREAKPOINT* bp)
{
    if(bp->type != BPMEMORY)
        return true;
    duint size;
    MemFindBaseAddr(bp->addr, &size);
    if(!BpDelete(bp->addr, BPMEMORY))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Delete memory breakpoint failed (BpDelete): %p\n"), bp->addr);
        return false;
    }
    if(bp->enabled && !RemoveMemoryBPX(bp->addr, size))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Delete memory breakpoint failed (RemoveMemoryBPX): %p\n"), bp->addr);
        return false;
    }
    return true;
}

bool cbDeleteAllHardwareBreakpoints(const BREAKPOINT* bp)
{
    if(bp->type != BPHARDWARE)
        return true;
    if(!BpDelete(bp->addr, BPHARDWARE))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Delete hardware breakpoint failed (BpDelete): %p\n"), bp->addr);
        return false;
    }
    if(bp->enabled && !DeleteHardwareBreakPoint(TITANGETDRX(bp->titantype)))
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Delete hardware breakpoint failed (DeleteHardwareBreakPoint): %p\n"), bp->addr);
        return false;
    }
    return true;
}

static void cbAttachDebugger()
{
    if(hEvent) //Signal the AeDebug event
    {
        SetEvent(hEvent);
        hEvent = 0;
    }
    hProcess = fdProcessInfo->hProcess;
    varset("$hp", (duint)fdProcessInfo->hProcess, true);
    varset("$pid", fdProcessInfo->dwProcessId, true);
}

void cbDetach()
{
    if(!isDetachedByUser)
        return;
    PLUG_CB_DETACH detachInfo;
    detachInfo.fdProcessInfo = fdProcessInfo;
    plugincbcall(CB_DETACH, &detachInfo);
    if(!DetachDebuggerEx(fdProcessInfo->dwProcessId))
        dputs(QT_TRANSLATE_NOOP("DBG", "DetachDebuggerEx failed..."));
    else
        dputs(QT_TRANSLATE_NOOP("DBG", "Detached!"));
    return;
}

cmdline_qoutes_placement_t getqoutesplacement(const char* cmdline)
{
    cmdline_qoutes_placement_t quotesPos;
    quotesPos.firstPos = quotesPos.secondPos = 0;

    char quoteSymb = cmdline[0];
    if(quoteSymb == '"' || quoteSymb == '\'')
    {
        for(size_t i = 1; i < strlen(cmdline); i++)
        {
            if(cmdline[i] == quoteSymb)
            {
                quotesPos.posEnum = i == strlen(cmdline) - 1 ? QOUTES_AT_BEGIN_AND_END : QOUTES_AROUND_EXE;
                quotesPos.secondPos = i;
                break;
            }
        }
        if(!quotesPos.secondPos)
            quotesPos.posEnum = NO_CLOSE_QUOTE_FOUND;
    }
    else
    {
        quotesPos.posEnum = NO_QOUTES;
        //try to locate first quote
        for(size_t i = 1; i < strlen(cmdline); i++)
            if(cmdline[i] == '"' || cmdline[i] == '\'')
                quotesPos.secondPos = i;
    }

    return quotesPos;
}

bool dbglistprocesses(std::vector<PROCESSENTRY32>* infoList, std::vector<std::string>* commandList)
{
    infoList->clear();
    Handle hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(!hProcessSnap)
        return false;
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    if(!Process32First(hProcessSnap, &pe32))
        return false;
    do
    {
        if(pe32.th32ProcessID == GetCurrentProcessId())
            continue;
        if(pe32.th32ProcessID == 0 || pe32.th32ProcessID == 4) // System process and Idle process have special PID.
            continue;
        Handle hProcess = TitanOpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pe32.th32ProcessID);
        if(!hProcess)
            continue;
        BOOL wow64 = false, mewow64 = false;
        if(!IsWow64Process(hProcess, &wow64) || !IsWow64Process(GetCurrentProcess(), &mewow64))
            continue;
        if((mewow64 && !wow64) || (!mewow64 && wow64))
            continue;
        wchar_t szExePath[MAX_PATH] = L"";
        if(GetModuleFileNameExW(hProcess, 0, szExePath, MAX_PATH))
            strcpy_s(pe32.szExeFile, StringUtils::Utf16ToUtf8(szExePath).c_str());
        infoList->push_back(pe32);
        //
        char* cmdline;

        if(!dbggetcmdline(&cmdline, NULL, hProcess))
            commandList->push_back("ARG_GET_ERROR");
        else
        {
            cmdline_qoutes_placement_t posEnum = getqoutesplacement(cmdline);
            char* cmdLineExe = strstr(cmdline, pe32.szExeFile);
            size_t cmdLineExeSize = cmdLineExe ? strlen(pe32.szExeFile) : 0;

            if(!cmdLineExe)
            {
                char* exeName = strrchr(pe32.szExeFile, '\\') ? strrchr(pe32.szExeFile, '\\') + 1 :
                                strrchr(pe32.szExeFile, '/') ? strrchr(pe32.szExeFile, '/') + 1 : pe32.szExeFile;
                size_t exeNameLen = strlen(exeName);

                char* peNameInCmd = strstr(cmdline, exeName);
                //check for exe name is used in path to exe
                for(char* exeNameInCmdTmp = peNameInCmd; exeNameInCmdTmp;)
                {
                    exeNameInCmdTmp = strstr(exeNameInCmdTmp + exeNameLen, exeName);
                    if(!exeNameInCmdTmp)
                        break;

                    char* nextSlash = strchr(exeNameInCmdTmp, '\\') ? strchr(exeNameInCmdTmp, '\\') :
                                      strchr(exeNameInCmdTmp, '/') ? strchr(exeNameInCmdTmp, '/') : NULL;
                    if(nextSlash && posEnum.posEnum == NO_QOUTES)  //if there NO_QOUTES, then the path to PE in cmdline can't contain spaces
                    {
                        if(strchr(exeNameInCmdTmp, ' ') < nextSlash)  //slash is in arguments
                        {
                            peNameInCmd = exeNameInCmdTmp;
                            break;
                        }
                        else
                            continue;
                    }
                    else if(nextSlash && posEnum.posEnum == QOUTES_AROUND_EXE)
                    {
                        if((cmdline + posEnum.secondPos) < nextSlash)  //slash is in arguments
                        {
                            peNameInCmd = exeNameInCmdTmp;
                            break;
                        }
                        else
                            continue;
                    }
                    else
                    {
                        peNameInCmd = exeNameInCmdTmp;
                        break;
                    }
                }

                if(peNameInCmd)
                    cmdLineExeSize = (size_t)(((LPBYTE)peNameInCmd - (LPBYTE)cmdline) + exeNameLen);
                else
                {
                    //try to locate basic name, without extension
                    Memory<char*> basicName(strlen(exeName) + 1, "dbglistprocesses:basicName");
                    strncpy_s(basicName(), sizeof(char) * strlen(exeName) + 1, exeName, _TRUNCATE);
                    char* dotInName = strrchr(basicName(), '.');
                    dotInName[0] = '\0';
                    size_t basicNameLen = strlen(basicName());
                    peNameInCmd = strstr(cmdline, basicName());
                    //check for basic name is used in path to exe
                    for(char* basicNameInCmdTmp = peNameInCmd; basicNameInCmdTmp;)
                    {
                        basicNameInCmdTmp = strstr(basicNameInCmdTmp + basicNameLen, basicName());
                        if(!basicNameInCmdTmp)
                            break;

                        char* nextSlash = strchr(basicNameInCmdTmp, '\\') ? strchr(basicNameInCmdTmp, '\\') :
                                          strchr(basicNameInCmdTmp, '/') ? strchr(basicNameInCmdTmp, '/') : NULL;
                        if(nextSlash && posEnum.posEnum == NO_QOUTES)  //if there NO_QOUTES, then the path to PE in cmdline can't contain spaces
                        {
                            if(strchr(basicNameInCmdTmp, ' ') < nextSlash)  //slash is in arguments
                            {
                                peNameInCmd = basicNameInCmdTmp;
                                break;
                            }
                            else
                                continue;
                        }
                        else if(nextSlash && posEnum.posEnum == QOUTES_AROUND_EXE)
                        {
                            if((cmdline + posEnum.secondPos) < nextSlash)  //slash is in arguments
                            {
                                peNameInCmd = basicNameInCmdTmp;
                                break;
                            }
                            else
                                continue;
                        }
                        else
                        {
                            peNameInCmd = basicNameInCmdTmp;
                            break;
                        }
                    }

                    if(peNameInCmd)
                        cmdLineExeSize = (size_t)(((LPBYTE)peNameInCmd - (LPBYTE)cmdline) + basicNameLen);
                }
            }

            switch(posEnum.posEnum)
            {
            case NO_CLOSE_QUOTE_FOUND:
                commandList->push_back(cmdline + cmdLineExeSize + 1);
                break;
            case NO_QOUTES:
                if(!posEnum.secondPos)
                    commandList->push_back(cmdline + cmdLineExeSize);
                else
                    commandList->push_back(cmdline + (cmdLineExeSize > posEnum.secondPos + 1 ? cmdLineExeSize : posEnum.secondPos + 1));
                break;
            case QOUTES_AROUND_EXE:
                commandList->push_back(cmdline + cmdLineExeSize + 2);
                break;
            case QOUTES_AT_BEGIN_AND_END:
                cmdline[strlen(cmdline) - 1] = '\0';
                commandList->push_back(cmdline + cmdLineExeSize + 1);
                break;
            }

            if(!commandList->empty())
                commandList->back() = StringUtils::Trim(commandList->back());

            efree(cmdline);
        }
    }
    while(Process32Next(hProcessSnap, &pe32));
    return true;
}

static bool getcommandlineaddr(duint* addr, cmdline_error_t* cmd_line_error, HANDLE hProcess = NULL)
{
    duint pprocess_parameters;

    cmd_line_error->addr = (duint)GetPEBLocation(hProcess ? hProcess : fdProcessInfo->hProcess);

    if(cmd_line_error->addr == 0)
    {
        cmd_line_error->type = CMDL_ERR_GET_PEB;
        return false;
    }

    if(hProcess)
    {
        duint NumberOfBytesRead;
        if(!MemoryReadSafe(hProcess, (LPVOID)((cmd_line_error->addr) + offsetof(PEB, ProcessParameters)),
                           &pprocess_parameters, sizeof(duint), &NumberOfBytesRead))
        {
            cmd_line_error->type = CMDL_ERR_READ_PROCPARM_PTR;
            return false;
        }

        *addr = (pprocess_parameters) + offsetof(RTL_USER_PROCESS_PARAMETERS, CommandLine);
    }
    else
    {
        //cast-trick to calculate the address of the remote peb field ProcessParameters
        cmd_line_error->addr = (duint) & (((PPEB)cmd_line_error->addr)->ProcessParameters);
        if(!MemRead(cmd_line_error->addr, &pprocess_parameters, sizeof(pprocess_parameters)))
        {
            cmd_line_error->type = CMDL_ERR_READ_PEBBASE;
            return false;
        }

        *addr = (duint) & (((RTL_USER_PROCESS_PARAMETERS*)pprocess_parameters)->CommandLine);
    }
    return true;
}

static bool patchcmdline(duint getcommandline, duint new_command_line, cmdline_error_t* cmd_line_error)
{
    duint command_line_stored = 0;
    unsigned char data[100];

    cmd_line_error->addr = getcommandline;
    if(!MemRead(cmd_line_error->addr, & data, sizeof(data)))
    {
        cmd_line_error->type = CMDL_ERR_READ_GETCOMMANDLINEBASE;
        return false;
    }

#ifdef _WIN64
    /*
    00007FFC5B91E3C8 | 48 8B 05 19 1D 0E 00     | mov rax,qword ptr ds:[7FFC5BA000E8]
    00007FFC5B91E3CF | C3                       | ret                                     |
    This is a relative offset then to get the symbol: next instruction of getmodulehandle (+7 bytes) + offset to symbol
    (the last 4 bytes of the instruction)
    */
    if(data[0] != 0x48 ||  data[1] != 0x8B || data[2] != 0x05 || data[7] != 0xC3)
    {
        cmd_line_error->type = CMDL_ERR_CHECK_GETCOMMANDLINESTORED;
        return false;
    }
    DWORD offset = * ((DWORD*) & data[3]);
    command_line_stored = getcommandline + 7 + offset;
#else //x86
    /*
    750FE9CA | A1 CC DB 1A 75           | mov eax,dword ptr ds:[751ADBCC]         |
    750FE9CF | C3                       | ret                                     |
    */
    if(data[0] != 0xA1 ||  data[5] != 0xC3)
    {
        cmd_line_error->type = CMDL_ERR_CHECK_GETCOMMANDLINESTORED;
        return false;
    }
    command_line_stored = * ((duint*) & data[1]);
#endif

    //update the pointer in the debuggee
    if(!MemWrite(command_line_stored, &new_command_line, sizeof(new_command_line)))
    {
        cmd_line_error->addr = command_line_stored;
        cmd_line_error->type = CMDL_ERR_WRITE_GETCOMMANDLINESTORED;
        return false;
    }

    return true;
}

static bool fixgetcommandlinesbase(duint new_command_line_unicode, duint new_command_line_ascii, cmdline_error_t* cmd_line_error)
{
    duint getcommandline;

    if(!valfromstring("kernelBase:GetCommandLineA", &getcommandline))
    {
        if(!valfromstring("kernel32:GetCommandLineA", &getcommandline))
        {
            cmd_line_error->type = CMDL_ERR_GET_GETCOMMANDLINE;
            return false;
        }
    }
    if(!patchcmdline(getcommandline, new_command_line_ascii, cmd_line_error))
        return false;

    if(!valfromstring("kernelbase:GetCommandLineW", &getcommandline))
    {
        if(!valfromstring("kernel32:GetCommandLineW", &getcommandline))
        {
            cmd_line_error->type = CMDL_ERR_GET_GETCOMMANDLINE;
            return false;
        }
    }
    if(!patchcmdline(getcommandline, new_command_line_unicode, cmd_line_error))
        return false;

    return true;
}

bool dbgsetcmdline(const char* cmd_line, cmdline_error_t* cmd_line_error)
{
    cmdline_error_t cmd_line_error_aux;
    UNICODE_STRING new_command_line;
    duint command_line_addr;

    if(cmd_line_error == NULL)
        cmd_line_error = &cmd_line_error_aux;

    if(!getcommandlineaddr(&cmd_line_error->addr, cmd_line_error))
        return false;

    command_line_addr = cmd_line_error->addr;

    SIZE_T cmd_line_size = strlen(cmd_line);
    new_command_line.Length = (USHORT)(strlen(cmd_line) + 1) * sizeof(WCHAR);
    new_command_line.MaximumLength = new_command_line.Length;

    Memory<wchar_t*> command_linewstr(new_command_line.Length);

    // Covert to Unicode.
    if(!MultiByteToWideChar(CP_UTF8, 0, cmd_line, (int)cmd_line_size + 1, command_linewstr(), (int)cmd_line_size + 1))
    {
        cmd_line_error->type = CMDL_ERR_CONVERTUNICODE;
        return false;
    }

    new_command_line.Buffer = command_linewstr();

    duint mem = (duint)MemAllocRemote(0, new_command_line.Length * 2);
    if(!mem)
    {
        cmd_line_error->type = CMDL_ERR_ALLOC_UNICODEANSI_COMMANDLINE;
        return false;
    }

    if(!MemWrite(mem, new_command_line.Buffer, new_command_line.Length))
    {
        cmd_line_error->addr = mem;
        cmd_line_error->type = CMDL_ERR_WRITE_UNICODE_COMMANDLINE;
        return false;
    }

    if(!MemWrite((mem + new_command_line.Length), (void*)cmd_line, strlen(cmd_line) + 1))
    {
        cmd_line_error->addr = mem + new_command_line.Length;
        cmd_line_error->type = CMDL_ERR_WRITE_ANSI_COMMANDLINE;
        return false;
    }

    if(!fixgetcommandlinesbase(mem, mem + new_command_line.Length, cmd_line_error))
        return false;

    new_command_line.Buffer = (PWSTR) mem;
    if(!MemWrite(command_line_addr, &new_command_line, sizeof(new_command_line)))
    {
        cmd_line_error->addr = command_line_addr;
        cmd_line_error->type = CMDL_ERR_WRITE_PEBUNICODE_COMMANDLINE;
        return false;
    }

    // Copy command line
    copyCommandLine(cmd_line);

    return true;
}

bool dbggetcmdline(char** cmd_line, cmdline_error_t* cmd_line_error, HANDLE hProcess /* = NULL */)
{
    UNICODE_STRING CommandLine;
    Memory<wchar_t*> wstr_cmd;
    cmdline_error_t cmd_line_error_aux;

    if(!cmd_line_error)
        cmd_line_error = &cmd_line_error_aux;

    if(!getcommandlineaddr(&cmd_line_error->addr, cmd_line_error, hProcess))
        return false;

    if(hProcess)
    {
        duint NumberOfBytesRead;
        if(!MemoryReadSafe(hProcess, (LPVOID)cmd_line_error->addr, &CommandLine, sizeof(UNICODE_STRING), &NumberOfBytesRead))
        {
            cmd_line_error->type = CMDL_ERR_READ_GETCOMMANDLINEBASE;
            return false;
        }

        wstr_cmd.realloc(CommandLine.Length + sizeof(wchar_t));

        cmd_line_error->addr = (duint)CommandLine.Buffer;
        if(!MemoryReadSafe(hProcess, (LPVOID)cmd_line_error->addr, wstr_cmd(), CommandLine.Length, &NumberOfBytesRead))
        {
            cmd_line_error->type = CMDL_ERR_GET_GETCOMMANDLINE;
            return false;
        }
    }
    else
    {
        if(!MemRead(cmd_line_error->addr, &CommandLine, sizeof(CommandLine)))
        {
            cmd_line_error->type = CMDL_ERR_READ_PROCPARM_PTR;
            return false;
        }

        wstr_cmd.realloc(CommandLine.Length + sizeof(wchar_t));

        cmd_line_error->addr = (duint)CommandLine.Buffer;
        if(!MemRead(cmd_line_error->addr, wstr_cmd(), CommandLine.Length))
        {
            cmd_line_error->type = CMDL_ERR_READ_PROCPARM_CMDLINE;
            return false;
        }
    }
    SIZE_T wstr_cmd_size = wcslen(wstr_cmd()) + 1;
    SIZE_T cmd_line_size = wstr_cmd_size * 2;

    *cmd_line = (char*)emalloc(cmd_line_size, "dbggetcmdline:cmd_line");

    if(cmd_line_size <= 2)
    {
        *cmd_line[0] = '\0';
        return true;
    }

    //Convert TO UTF-8
    if(!WideCharToMultiByte(CP_UTF8, 0, wstr_cmd(), (int)wstr_cmd_size, *cmd_line, (int)cmd_line_size, NULL, NULL))
    {
        efree(*cmd_line);
        *cmd_line = nullptr;
        cmd_line_error->type = CMDL_ERR_CONVERTUNICODE;
        return false;
    }

    return true;
}

static DWORD WINAPI scriptThread(void* data)
{
    CBPLUGINSCRIPT cbScript = (CBPLUGINSCRIPT)data;
    cbScript();
    return 0;
}

void dbgstartscriptthread(CBPLUGINSCRIPT cbScript)
{
    CloseHandle(CreateThread(0, 0, scriptThread, cbScript, 0, 0));
}

duint dbggetdebuggedbase()
{
    return pDebuggedBase;
}

static void debugLoopFunction(void* lpParameter, bool attach)
{
    //we are running
    EXCLUSIVE_ACQUIRE(LockDebugStartStop);
    lock(WAITID_STOP);

    //initialize variables
    bIsAttached = attach;
    bSkipExceptions = false;
    bBreakOnNextDll = false;
    bFreezeStack = false;
    ecount = 0;

    //prepare attach/createprocess
    DWORD pid;
    INIT_STRUCT* init;
    if(attach)
    {
        pid = DWORD(lpParameter);
        static PROCESS_INFORMATION pi_attached;
        memset(&pi_attached, 0, sizeof(pi_attached));
        fdProcessInfo = &pi_attached;
    }
    else
    {
        init = (INIT_STRUCT*)lpParameter;
        pDebuggedEntry = GetPE32DataW(StringUtils::Utf8ToUtf16(init->exe).c_str(), 0, UE_OEP);
        strcpy_s(szFileName, init->exe);
    }

    bFileIsDll = IsFileDLLW(StringUtils::Utf8ToUtf16(szFileName).c_str(), 0);
    DbSetPath(nullptr, szFileName);

    if(!attach)
    {
        // Load command line if it exists in DB
        DbLoad(DbLoadSaveType::CommandLine);
        if(!isCmdLineEmpty())
        {
            char* commandLineArguments = NULL;
            commandLineArguments = getCommandLineArgs();

            if(commandLineArguments)
                init->commandline = commandLineArguments;
        }

        //start the process
        if(bFileIsDll)
            fdProcessInfo = (PROCESS_INFORMATION*)InitDLLDebugW(StringUtils::Utf8ToUtf16(init->exe).c_str(), false, StringUtils::Utf8ToUtf16(init->commandline).c_str(), StringUtils::Utf8ToUtf16(init->currentfolder).c_str(), 0);
        else
            fdProcessInfo = (PROCESS_INFORMATION*)InitDebugW(StringUtils::Utf8ToUtf16(init->exe).c_str(), StringUtils::Utf8ToUtf16(init->commandline).c_str(), StringUtils::Utf8ToUtf16(init->currentfolder).c_str());
        if(!fdProcessInfo)
        {
            fdProcessInfo = &g_pi;
            dprintf(QT_TRANSLATE_NOOP("DBG", "Error starting process (CreateProcess, %s)!\n"), ErrorCodeToName(GetLastError()).c_str());
            unlock(WAITID_STOP);
            return;
        }

        //check for WOW64
        BOOL wow64 = false, mewow64 = false;
        if(!IsWow64Process(fdProcessInfo->hProcess, &wow64) || !IsWow64Process(GetCurrentProcess(), &mewow64))
        {
            dputs(QT_TRANSLATE_NOOP("DBG", "IsWow64Process failed!"));
            StopDebug();
            unlock(WAITID_STOP);
            return;
        }
        if((mewow64 && !wow64) || (!mewow64 && wow64))
        {
#ifdef _WIN64
            dputs(QT_TRANSLATE_NOOP("DBG", "Use x32dbg to debug this process!"));
#else
            dputs(QT_TRANSLATE_NOOP("DBG", "Use x64dbg to debug this process!"));
#endif // _WIN64
            unlock(WAITID_STOP);
            return;
        }

        //set script variables
        varset("$hp", (duint)fdProcessInfo->hProcess, true);
        varset("$pid", fdProcessInfo->dwProcessId, true);

        if(!OpenProcessToken(fdProcessInfo->hProcess, TOKEN_ALL_ACCESS, &hProcessToken))
            hProcessToken = 0;
    }

    //set custom handlers
    SetCustomHandler(UE_CH_CREATEPROCESS, (void*)cbCreateProcess);
    SetCustomHandler(UE_CH_EXITPROCESS, (void*)cbExitProcess);
    SetCustomHandler(UE_CH_CREATETHREAD, (void*)cbCreateThread);
    SetCustomHandler(UE_CH_EXITTHREAD, (void*)cbExitThread);
    SetCustomHandler(UE_CH_SYSTEMBREAKPOINT, (void*)cbSystemBreakpoint);
    SetCustomHandler(UE_CH_LOADDLL, (void*)cbLoadDll);
    SetCustomHandler(UE_CH_UNLOADDLL, (void*)cbUnloadDll);
    SetCustomHandler(UE_CH_OUTPUTDEBUGSTRING, (void*)cbOutputDebugString);
    SetCustomHandler(UE_CH_UNHANDLEDEXCEPTION, (void*)cbException);
    SetCustomHandler(UE_CH_DEBUGEVENT, (void*)cbDebugEvent);

    //inform GUI we started without problems
    GuiSetDebugState(initialized);
    GuiAddRecentFile(szFileName);

    //set GUI title
    strcpy_s(szBaseFileName, szFileName);
    int len = (int)strlen(szBaseFileName);
    while(szBaseFileName[len] != '\\' && len)
        len--;
    if(len)
        strcpy_s(szBaseFileName, szBaseFileName + len + 1);
    GuiUpdateWindowTitle(szBaseFileName);

    //call plugin callback
    PLUG_CB_INITDEBUG initInfo;
    initInfo.szFileName = szFileName;
    plugincbcall(CB_INITDEBUG, &initInfo);

    //call plugin callback (attach)
    if(attach)
    {
        PLUG_CB_ATTACH attachInfo;
        attachInfo.dwProcessId = (DWORD)pid;
        plugincbcall(CB_ATTACH, &attachInfo);
    }

    //run debug loop (returns when process debugging is stopped)
    if(attach)
    {
        AttachDebugger(pid, true, fdProcessInfo, (void*)cbAttachDebugger);
    }
    else
    {
        hProcess = fdProcessInfo->hProcess;
        DebugLoop();
    }

    //call plugin callback
    PLUG_CB_STOPDEBUG stopInfo;
    stopInfo.reserved = 0;
    plugincbcall(CB_STOPDEBUG, &stopInfo);

    //cleanup dbghelp
    SafeSymRegisterCallbackW64(hProcess, nullptr, 0);
    SafeSymCleanup(hProcess);

    //message the user/do final stuff
    RemoveAllBreakPoints(UE_OPTION_REMOVEALL); //remove all breakpoints

    //cleanup
    dbgcleartracecondition();
    dbgClearRtuBreakpoints();
    DbClose();
    ModClear();
    ThreadClear();
    WatchClear();
    TraceRecord.clear();
    GuiSetDebugState(stopped);
    GuiUpdateAllViews();
    dputs(QT_TRANSLATE_NOOP("DBG", "Debugging stopped!"));
    varset("$hp", (duint)0, true);
    varset("$pid", (duint)0, true);
    if(hProcessToken)
        CloseHandle(hProcessToken);
    unlock(WAITID_STOP); //we are done
    pDebuggedEntry = 0;
    pDebuggedBase = 0;
    pCreateProcessBase = 0;
    isDetachedByUser = false;
}

void dbgsetdebuggeeinitscript(const char* fileName)
{
    if(fileName)
        strcpy_s(szDebuggeeInitializationScript, fileName);
    else
        szDebuggeeInitializationScript[0] = 0;
}

const char* dbggetdebuggeeinitscript()
{
    return szDebuggeeInitializationScript;
}

DWORD WINAPI threadDebugLoop(void* lpParameter)
{
    debugLoopFunction(lpParameter, false);
    return 0;
}

DWORD WINAPI threadAttachLoop(void* lpParameter)
{
    debugLoopFunction(lpParameter, true);
    return 0;
}
