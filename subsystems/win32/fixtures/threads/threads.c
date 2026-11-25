/*
 * threads.exe -- WP3's fixture (WIN32_WORDPAD_ROADMAP.md): a second thread
 * and what MFC's start-up does with one. A thread started suspended, its
 * suspend count, thread-local storage (TlsAlloc and static TLS with its
 * callbacks), DLL_THREAD_ATTACH and DETACH in order and
 * DisableThreadLibraryCalls (tlog.dll, tquiet.dll), events, mutexes,
 * semaphores and the waits on them, a critical section two threads contend
 * for, the interlocked functions, _beginthreadex with a priority, and a
 * window on the second thread: sent, posted and thread messages, each
 * handled on the thread that owns the window, and per-thread queues. The
 * process ends with one thread still waiting.
 *
 * Only the main thread prints, at points where the other thread waits, so
 * the output is the same on Windows XP, in the harness and on SavanXP at any
 * number of processors.
 */
#include "../kernel/print.h"

typedef DWORD(WINAPI *THREAD_START)(LPVOID);
typedef unsigned(__stdcall *BEGIN_START)(void *);

DECLSPEC_IMPORT HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, THREAD_START start, LPVOID argument,
                                           DWORD flags, LPDWORD id);
DECLSPEC_IMPORT DWORD WINAPI ResumeThread(HANDLE thread);
DECLSPEC_IMPORT DWORD WINAPI SuspendThread(HANDLE thread);
DECLSPEC_IMPORT BOOL WINAPI GetExitCodeThread(HANDLE thread, LPDWORD code);
DECLSPEC_IMPORT __declspec(noreturn) void WINAPI ExitThread(DWORD code);
DECLSPEC_IMPORT BOOL WINAPI SetThreadPriority(HANDLE thread, int priority);
DECLSPEC_IMPORT int WINAPI GetThreadPriority(HANDLE thread);
DECLSPEC_IMPORT HANDLE WINAPI GetCurrentThread(void);
DECLSPEC_IMPORT DWORD WINAPI GetCurrentThreadId(void);
DECLSPEC_IMPORT HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name);
DECLSPEC_IMPORT BOOL WINAPI SetEvent(HANDLE event);
DECLSPEC_IMPORT BOOL WINAPI ResetEvent(HANDLE event);
DECLSPEC_IMPORT HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name);
DECLSPEC_IMPORT BOOL WINAPI ReleaseMutex(HANDLE mutex);
DECLSPEC_IMPORT HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name);
DECLSPEC_IMPORT BOOL WINAPI ReleaseSemaphore(HANDLE semaphore, LONG count, LPLONG previous);
DECLSPEC_IMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DECLSPEC_IMPORT DWORD WINAPI WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD milliseconds);
DECLSPEC_IMPORT void WINAPI Sleep(DWORD milliseconds);
DECLSPEC_IMPORT BOOL WINAPI DuplicateHandle(HANDLE source_process, HANDLE source, HANDLE target_process,
                                            HANDLE *target, DWORD access, BOOL inherit, DWORD options);
DECLSPEC_IMPORT HANDLE WINAPI GetCurrentProcess(void);
DECLSPEC_IMPORT DWORD WINAPI TlsAlloc(void);
DECLSPEC_IMPORT LPVOID WINAPI TlsGetValue(DWORD index);
DECLSPEC_IMPORT BOOL WINAPI TlsSetValue(DWORD index, LPVOID value);
DECLSPEC_IMPORT BOOL WINAPI TlsFree(DWORD index);
DECLSPEC_IMPORT void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs);
DECLSPEC_IMPORT void WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs);
DECLSPEC_IMPORT BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION cs);
DECLSPEC_IMPORT void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs);
DECLSPEC_IMPORT void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs);
DECLSPEC_IMPORT LONG WINAPI InterlockedIncrement(LPLONG value);
DECLSPEC_IMPORT LONG WINAPI InterlockedDecrement(LPLONG value);
DECLSPEC_IMPORT LONG WINAPI InterlockedExchange(LPLONG value, LONG v);
DECLSPEC_IMPORT LONG WINAPI InterlockedCompareExchange(LPLONG value, LONG exchange, LONG comparand);
DECLSPEC_IMPORT LONG WINAPI InterlockedExchangeAdd(LPLONG value, LONG add);

DECLSPEC_IMPORT ATOM WINAPI RegisterClassW(const WNDCLASSW *wc);
DECLSPEC_IMPORT HWND WINAPI CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w,
                                            int h, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param);
DECLSPEC_IMPORT BOOL WINAPI DestroyWindow(HWND hwnd);
DECLSPEC_IMPORT LRESULT WINAPI DefWindowProcW(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
DECLSPEC_IMPORT LRESULT WINAPI SendMessageW(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
DECLSPEC_IMPORT BOOL WINAPI PostMessageW(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
DECLSPEC_IMPORT BOOL WINAPI PostThreadMessageW(DWORD thread, UINT msg, WPARAM w, LPARAM l);
DECLSPEC_IMPORT BOOL WINAPI GetMessageW(LPMSG msg, HWND hwnd, UINT first, UINT last);
DECLSPEC_IMPORT BOOL WINAPI PeekMessageW(LPMSG msg, HWND hwnd, UINT first, UINT last, UINT remove);
DECLSPEC_IMPORT LRESULT WINAPI DispatchMessageW(const MSG *msg);
DECLSPEC_IMPORT void WINAPI PostQuitMessage(int code);
DECLSPEC_IMPORT DWORD WINAPI GetWindowThreadProcessId(HWND hwnd, LPDWORD process);
DECLSPEC_IMPORT BOOL WINAPI IsWindow(HWND hwnd);

DECLSPEC_IMPORT unsigned long __cdecl _beginthreadex(void *security, unsigned stack, BEGIN_START start, void *argument,
                                                     unsigned flags, unsigned *id);
DECLSPEC_IMPORT void __cdecl _endthreadex(unsigned code);

DECLSPEC_IMPORT void WINAPI TlogTake(char *out);
DECLSPEC_IMPORT void WINAPI TlogNote(char letter);
DECLSPEC_IMPORT unsigned WINAPI TquietValue(void);

#define CREATE_SUSPENDED 0x4u
#define STILL_ACTIVE 259u
#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED_0 0x80u
#define INFINITE 0xffffffffu
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ERROR_RETURN 0x7fffffff
#define DUPLICATE_SAME_ACCESS 0x2u

/* ---- Static TLS, as the C runtime's tlssup.c provides it ----------------------- */

#pragma section(".CRT$XLA", read)
#pragma section(".CRT$XLB", read)
#pragma section(".CRT$XLZ", read)
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
__asm__(".globl __tls_array\n.set __tls_array, 0x2c");
DWORD _tls_index = 0;
__declspec(allocate(".tls")) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end = 0;
__declspec(allocate(".CRT$XLA")) PIMAGE_TLS_CALLBACK __xl_a = 0;
__declspec(allocate(".CRT$XLZ")) PIMAGE_TLS_CALLBACK __xl_z = 0;
const IMAGE_TLS_DIRECTORY32 _tls_used = {
    (DWORD)&_tls_start, (DWORD)&_tls_end, (DWORD)&_tls_index, (DWORD)(&__xl_a + 1), 0, 0,
};

__declspec(thread) unsigned thread_value = 5;

/* Notes the reason as a digit: 1 and 0 for the process, 2 and 3 a thread. */
static void WINAPI tls_callback(LPVOID instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    TlogNote((char)('0' + reason));
    if (reason == DLL_THREAD_ATTACH) {
        thread_value += 10;
    }
}
__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK callbacks[] = {tls_callback};

/* ---- The worker thread ---------------------------------------------------------- */

static DWORD g_slot;
static HANDLE g_ready;
static HANDLE g_go;
static HANDLE g_mutex;
static DWORD g_worker_id;
static CRITICAL_SECTION g_section;
static LONG g_counter;
static LONG g_worker_entered;
static HWND g_window;

/* What the worker saw, printed by the main thread. */
static struct {
    DWORD id_matches;
    DWORD slot_value;
    unsigned static_value;
    DWORD mutex_wait;
    BOOL try_enter;
    DWORD window_thread;
    DWORD posted;
    DWORD thread_message;
    DWORD destroyed;
} g_seen;

static void take_log(const char *label) {
    char log[256];
    TlogTake(log);
    line(label);
    say("[");
    say(log);
    say("]");
}

static LRESULT WINAPI worker_window(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_USER + 1:
        return GetCurrentThreadId() == g_worker_id ? 42 : 13;
    case WM_USER + 2:
        g_seen.posted = (DWORD)w + (GetCurrentThreadId() == g_worker_id ? 100u : 0u);
        SetEvent(g_ready);
        return 0;
    case WM_USER + 4:
        g_seen.destroyed = DestroyWindow(hwnd);
        PostQuitMessage(7);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, w, l);
    }
}

static DWORD WINAPI worker(LPVOID argument) {
    g_seen.id_matches = GetCurrentThreadId() == g_worker_id && argument == (LPVOID)0x1234;
    g_seen.slot_value = (DWORD)TlsGetValue(g_slot);
    g_seen.static_value = thread_value;
    TlsSetValue(g_slot, (LPVOID)2);
    thread_value = 99;
    SetEvent(g_ready);

    /* The mutex the main thread owns. */
    WaitForSingleObject(g_go, INFINITE);
    g_seen.mutex_wait = WaitForSingleObject(g_mutex, 0);
    SetEvent(g_ready);

    /* The critical section: refused while held, then waited for. */
    WaitForSingleObject(g_go, INFINITE);
    g_seen.try_enter = TryEnterCriticalSection(&g_section);
    SetEvent(g_ready);
    EnterCriticalSection(&g_section);
    InterlockedExchange(&g_worker_entered, 1);
    LeaveCriticalSection(&g_section);
    for (int i = 0; i < 20000; ++i) {
        InterlockedIncrement(&g_counter);
    }
    SetEvent(g_ready);

    /* A window of this thread, and its message loop. */
    WaitForSingleObject(g_go, INFINITE);
    g_window = CreateWindowExW(0, L"SxwThreadWindow", L"worker", 0, 0, 0, 10, 10, HWND_MESSAGE, NULL, NULL, NULL);
    g_seen.window_thread = GetWindowThreadProcessId(g_window, NULL) == g_worker_id;
    SetEvent(g_ready);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.hwnd == NULL && msg.message == WM_USER + 3) {
            g_seen.thread_message = (DWORD)msg.wParam;
            SetEvent(g_ready);
            continue;
        }
        DispatchMessageW(&msg);
    }
    return (DWORD)msg.wParam;
}

static unsigned __stdcall ended(void *argument) {
    _endthreadex((unsigned)(ULONG_PTR)argument);
    return 1;
}

static DWORD WINAPI returns(LPVOID argument) {
    return (DWORD)(ULONG_PTR)argument;
}

static DWORD WINAPI exits(LPVOID argument) {
    ExitThread((DWORD)(ULONG_PTR)argument + 1u);
}

static DWORD WINAPI waits_forever(LPVOID argument) {
    (void)argument;
    SetEvent(g_ready);
    WaitForSingleObject(g_go, INFINITE);
    return 0;
}

static void say_wait(DWORD result) {
    switch (result) {
    case WAIT_OBJECT_0: say("object 0"); break;
    case WAIT_OBJECT_0 + 1: say("object 1"); break;
    case WAIT_OBJECT_0 + 2: say("object 2"); break;
    case WAIT_ABANDONED_0: say("abandoned 0"); break;
    case WAIT_TIMEOUT: say("timeout"); break;
    case WAIT_FAILED:
        say("failed");
        say_error();
        break;
    default: say_hex(result); break;
    }
}

static void say_exit_code(HANDLE thread) {
    DWORD code = 0;
    say_result(GetExitCodeThread(thread, &code));
    say(" ");
    say_int((long)code);
}

static void wait_ready(void) {
    WaitForSingleObject(g_ready, INFINITE);
}

static void synchronization(void) {
    HANDLE manual = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE automatic = CreateEventW(NULL, FALSE, TRUE, NULL);
    line("auto-reset event, signaled: wait, wait");
    say_wait(WaitForSingleObject(automatic, 0));
    say(", ");
    say_wait(WaitForSingleObject(automatic, 0));
    line("manual-reset event, set: wait, wait, reset, wait");
    SetEvent(manual);
    say_wait(WaitForSingleObject(manual, 0));
    say(", ");
    say_wait(WaitForSingleObject(manual, 0));
    say(", ");
    say_result(ResetEvent(manual));
    say(", ");
    say_wait(WaitForSingleObject(manual, 0));
    line("a wait of 30 ms on nothing");
    say_wait(WaitForSingleObject(manual, 30));

    HANDLE semaphore = CreateSemaphoreW(NULL, 2, 3, NULL);
    line("semaphore 2 of 3: wait, wait, wait");
    say_wait(WaitForSingleObject(semaphore, 0));
    say(", ");
    say_wait(WaitForSingleObject(semaphore, 0));
    say(", ");
    say_wait(WaitForSingleObject(semaphore, 0));
    LONG previous = -1;
    line("ReleaseSemaphore(2)");
    say_result(ReleaseSemaphore(semaphore, 2, &previous));
    say(" previous ");
    say_int(previous);
    line("ReleaseSemaphore(2) past its maximum");
    previous = -1;
    say_result(ReleaseSemaphore(semaphore, 2, &previous));
    say(" previous ");
    say_int(previous);
    line("CreateSemaphoreW(3, 2)");
    say_yes(CreateSemaphoreW(NULL, 3, 2, NULL) != NULL);
    say_error();

    line("WaitForMultipleObjects(any): manual unset, semaphore");
    HANDLE pair[2] = {manual, semaphore};
    say_wait(WaitForMultipleObjects(2, pair, FALSE, 0));
    line("WaitForMultipleObjects(all) with one unset");
    say_wait(WaitForMultipleObjects(2, pair, TRUE, 0));
    line("the semaphore was not taken");
    LONG count = 0;
    ReleaseSemaphore(semaphore, 0, &count);
    say_error();
    SetEvent(manual);
    line("WaitForMultipleObjects(all) with both set");
    say_wait(WaitForMultipleObjects(2, pair, TRUE, 0));
    line("then the semaphore holds");
    ReleaseSemaphore(semaphore, 1, &count);
    say_int(count);
    line("WaitForMultipleObjects(any): both set, the first wins");
    say_wait(WaitForMultipleObjects(2, pair, FALSE, 0));
    line("WaitForMultipleObjects(0 handles)");
    say_wait(WaitForMultipleObjects(0, pair, FALSE, 0));
    line("WaitForSingleObject(a closed handle)");
    HANDLE gone = CreateEventW(NULL, TRUE, TRUE, NULL);
    CloseHandle(gone);
    say_wait(WaitForSingleObject(gone, 0));

    line("DuplicateHandle, then the copy");
    HANDLE copy = NULL;
    say_result(DuplicateHandle(GetCurrentProcess(), manual, GetCurrentProcess(), &copy, 0, FALSE,
                               DUPLICATE_SAME_ACCESS));
    say(" ");
    say_yes(copy != manual);
    ResetEvent(copy);
    say(", the original ");
    say_wait(WaitForSingleObject(manual, 0));
    CloseHandle(copy);
    CloseHandle(manual);
    CloseHandle(automatic);
    CloseHandle(semaphore);

    HANDLE mutex = CreateMutexW(NULL, FALSE, NULL);
    line("mutex: wait, wait again, release, release, release");
    say_wait(WaitForSingleObject(mutex, 0));
    say(", ");
    say_wait(WaitForSingleObject(mutex, 0));
    say(", ");
    say_result(ReleaseMutex(mutex));
    say(", ");
    say_result(ReleaseMutex(mutex));
    say(", ");
    say_result(ReleaseMutex(mutex));
    CloseHandle(mutex);
    line("Sleep(0), Sleep(20)");
    Sleep(0);
    Sleep(20);
    say("ok");
}

static void second_thread(void) {
    g_slot = TlsAlloc();
    TlsSetValue(g_slot, (LPVOID)1);
    thread_value = 6;
    g_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_mutex = CreateMutexW(NULL, TRUE, NULL);
    InitializeCriticalSection(&g_section);
    static WNDCLASSW wc;
    wc.lpfnWndProc = worker_window;
    wc.lpszClassName = L"SxwThreadWindow";
    RegisterClassW(&wc);
    take_log("notes before the thread");

    line("CreateThread(CREATE_SUSPENDED)");
    HANDLE thread = CreateThread(NULL, 0, worker, (LPVOID)0x1234, CREATE_SUSPENDED, &g_worker_id);
    say_yes(thread != NULL && g_worker_id != 0 && g_worker_id != GetCurrentThreadId());
    line("exit code while suspended");
    say_exit_code(thread);
    line("SuspendThread, ResumeThread, ResumeThread");
    say_int((long)SuspendThread(thread));
    say(", ");
    say_int((long)ResumeThread(thread));
    say(", ");
    say_int((long)ResumeThread(thread));
    wait_ready();
    take_log("notes after it started");
    line("the thread saw its id, an empty slot and the TLS template");
    say_yes(g_seen.id_matches);
    say(" ");
    say_int((long)g_seen.slot_value);
    say(" ");
    say_int((long)g_seen.static_value);
    line("this thread's slot and static TLS");
    say_int((long)(DWORD)TlsGetValue(g_slot));
    say(" ");
    say_int((long)thread_value);

    SetEvent(g_go);
    wait_ready();
    line("the thread's wait on our mutex");
    say_wait(g_seen.mutex_wait);
    line("ReleaseMutex, ReleaseMutex");
    say_result(ReleaseMutex(g_mutex));
    say(", ");
    say_result(ReleaseMutex(g_mutex));

    EnterCriticalSection(&g_section);
    SetEvent(g_go);
    wait_ready();
    line("the thread's TryEnterCriticalSection while we hold it");
    say_yes(g_seen.try_enter);
    Sleep(50);
    line("still out after 50 ms");
    say_yes(g_worker_entered == 0);
    line("the section, held");
    say_int(g_section.RecursionCount);
    say(" waiting ");
    say_yes(g_section.LockCount > 0 || g_section.LockCount < -1);
    for (int i = 0; i < 20000; ++i) {
        InterlockedIncrement(&g_counter);
    }
    LeaveCriticalSection(&g_section);
    wait_ready();
    line("in after we left, and the counter");
    say_yes(g_worker_entered == 1);
    say(" ");
    say_int(g_counter);

    SetEvent(g_go);
    wait_ready();
    line("the window belongs to the thread");
    say_yes(g_seen.window_thread);
    line("SendMessageW from here, handled there");
    say_int((long)SendMessageW(g_window, WM_USER + 1, 0, 0));
    line("PostMessageW");
    say_result(PostMessageW(g_window, WM_USER + 2, 5, 0));
    wait_ready();
    say(" handled there ");
    say_int((long)g_seen.posted);
    line("PostThreadMessageW");
    say_result(PostThreadMessageW(g_worker_id, WM_USER + 3, 9, 0));
    wait_ready();
    say(" seen ");
    say_int((long)g_seen.thread_message);
    line("this thread's queue holds none of it");
    MSG msg;
    say_yes(!PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE));
    line("PostThreadMessageW to itself");
    PostThreadMessageW(GetCurrentThreadId(), WM_USER + 5, 11, 0);
    say_yes(PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE) && msg.message == WM_USER + 5 && msg.wParam == 11);
    line("DestroyWindow from another thread");
    say_result(DestroyWindow(g_window));
    line("destroyed by its own thread");
    SendMessageW(g_window, WM_USER + 4, 0, 0);
    say_yes(g_seen.destroyed);
    say(" ");
    say_yes(!IsWindow(g_window));

    line("wait for the thread");
    say_wait(WaitForSingleObject(thread, INFINITE));
    line("its exit code");
    say_exit_code(thread);
    take_log("notes after it ended");
    line("CloseHandle");
    say_result(CloseHandle(thread));
    DeleteCriticalSection(&g_section);
    TlsFree(g_slot);
}

static void other_threads(void) {
    line("_beginthreadex(CREATE_SUSPENDED), idle priority");
    unsigned id = 0;
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, ended, (void *)3, CREATE_SUSPENDED, &id);
    say_yes(thread != NULL && id != 0);
    say(" ");
    say_result(SetThreadPriority(thread, THREAD_PRIORITY_IDLE));
    say(" ");
    say_int(GetThreadPriority(thread));
    line("this thread's priority");
    say_int(GetThreadPriority(GetCurrentThread()));
    ResumeThread(thread);
    line("_endthreadex(3)");
    say_wait(WaitForSingleObject(thread, INFINITE));
    say(" ");
    say_exit_code(thread);
    CloseHandle(thread);
    take_log("notes");

    line("a thread that returns 21");
    thread = CreateThread(NULL, 0, returns, (LPVOID)21, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    say_exit_code(thread);
    CloseHandle(thread);
    line("a thread that calls ExitThread(31)");
    thread = CreateThread(NULL, 65536, exits, (LPVOID)30, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    say_exit_code(thread);
    line("ResumeThread on an ended thread");
    say_int((long)ResumeThread(thread));
    CloseHandle(thread);
    take_log("notes");
    line("ResumeThread on a closed handle");
    say_hex(ResumeThread(thread));
    say_error();
}

void __stdcall start(void) {
    say("threads.exe");
    take_log("notes at start");
    line("tquiet says");
    say_int((long)TquietValue());
    synchronization();
    second_thread();
    other_threads();
    line("a thread left waiting as the process ends");
    HANDLE thread = CreateThread(NULL, 0, waits_forever, NULL, 0, NULL);
    wait_ready();
    say_yes(thread != NULL);
    take_log("notes");
    ExitProcess(5);
}
