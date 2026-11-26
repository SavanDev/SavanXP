/*
 * hook.c -- Hooks (docs/userland/WIN32.md, "Hooks and subclassing"):
 * WH_CBT, WH_CALLWNDPROC and WH_CALLWNDPROCRET, WH_GETMESSAGE and
 * WH_MSGFILTER, the ones MFC and WordPad install. A hook belongs to the
 * thread it names, or to every thread of the process when it names none
 * and gives a module. The newest hook of a kind is called first and
 * CallNextHookEx passes on to the next older one, as on Windows.
 *
 * Where each kind is called, and with what, is what Windows XP did for
 * fixtures/hooks: the CBT hook before a window gets its first message, before
 * an activation, a focus change, a system command and the destruction of a
 * top-level window; the window procedure hooks around every sent message but
 * not a dispatched one; the message hook on what GetMessage and PeekMessage
 * return; the filter hook on every message a dialog, menu or scroll bar loop
 * takes.
 */
#include "user32_private.h"

#define MAX_HOOKS 64
#define HOOK_TAG 0x00070000u
#define WH_MIN (-1)
#define WH_MAX 14

typedef struct hook {
    int used;
    int id;
    HOOKPROC proc;
    u32 thread; /* 0: every thread of the process */
    u32 serial; /* newer hooks have higher serials */
} hook;

static hook g_hooks[MAX_HOOKS];
static u32 g_serial;
int g_hook_count;

static HHOOK handle(const hook *h) {
    return (HHOOK)(HOOK_TAG | (u32)((h - g_hooks) + 1) * 4u);
}

static hook *hook_of(HHOOK h) {
    const u32 v = (u32)h;
    const u32 index = (v & 0xffffu) / 4u;
    if ((v & 0xffff0000u) != HOOK_TAG || (v & 3u) != 0 || index == 0 || index > MAX_HOOKS) {
        return 0;
    }
    hook *k = &g_hooks[index - 1u];
    return k->used ? k : 0;
}

HHOOK WINAPI SetWindowsHookExW(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId) {
    if (idHook < WH_MIN || idHook > WH_MAX || idHook == 0) {
        set_last_error(ERROR_INVALID_HOOK_FILTER);
        return NULL;
    }
    if (dwThreadId == 0 && hmod == NULL) {
        set_last_error(ERROR_HOOK_NEEDS_HMOD);
        return NULL;
    }
    if (lpfn == NULL) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    for (int i = 0; i < MAX_HOOKS; ++i) {
        hook *h = &g_hooks[i];
        if (!h->used) {
            h->used = 1;
            h->id = idHook;
            h->proc = lpfn;
            h->thread = dwThreadId;
            h->serial = ++g_serial;
            ++g_hook_count;
            return handle(h);
        }
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

BOOL WINAPI UnhookWindowsHookEx(HHOOK hhk) {
    hook *h = hook_of(hhk);
    if (h == 0) {
        set_last_error(ERROR_INVALID_HOOK_HANDLE);
        return FALSE;
    }
    h->used = 0;
    --g_hook_count;
    return TRUE;
}

/* The newest hook of a kind for this thread older than `below` (any when
 * below is 0). */
static hook *next_hook(int id, u32 below) {
    const u32 self = GetCurrentThreadId();
    hook *best = 0;
    for (int i = 0; i < MAX_HOOKS; ++i) {
        hook *h = &g_hooks[i];
        if (h->used && h->id == id && (h->thread == 0 || h->thread == self) && (below == 0 || h->serial < below) &&
            (best == 0 || h->serial > best->serial)) {
            best = h;
        }
    }
    return best;
}

LRESULT WINAPI CallNextHookEx(HHOOK hhk, int nCode, WPARAM wParam, LPARAM lParam) {
    hook *h = hook_of(hhk);
    if (h == 0) {
        return 0;
    }
    hook *next = next_hook(h->id, h->serial);
    return next != 0 ? next->proc(nCode, wParam, lParam) : 0;
}

int hook_installed(int id) {
    return g_hook_count != 0 && next_hook(id, 0) != 0;
}

LRESULT call_hook(int id, int code, WPARAM wParam, LPARAM lParam) {
    if (g_hook_count == 0) {
        return 0;
    }
    hook *h = next_hook(id, 0);
    return h != 0 ? h->proc(code, wParam, lParam) : 0;
}

LRESULT cbt_hook(int code, WPARAM wParam, LPARAM lParam) {
    return call_hook(WH_CBT, code, wParam, lParam);
}

/* WH_MSGFILTER: TRUE when a hook took the message. */
BOOL WINAPI CallMsgFilterW(LPMSG lpMsg, int nCode) {
    if (lpMsg == NULL) {
        return FALSE;
    }
    return call_hook(WH_MSGFILTER, nCode, 0, (LPARAM)lpMsg) != 0;
}
