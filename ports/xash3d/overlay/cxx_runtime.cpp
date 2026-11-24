#include <stddef.h>
#include <stdlib.h>

/* The modules are compiled without exceptions. Allocation failure is
 * fatal and each loaded module owns its destructor list. */
void* operator new(size_t size)
{
    void* ptr = malloc(size ? size : 1);
    if (!ptr)
        abort();
    return ptr;
}

void* operator new[](size_t size)
{
    return ::operator new(size);
}

void operator delete(void* ptr) noexcept
{
    free(ptr);
}

void operator delete(void* ptr, size_t) noexcept
{
    free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
    free(ptr);
}

struct Destructor {
    void (*run)(void*);
    void* arg;
    Destructor* next;
};

static Destructor* destructors;
static bool registered;

static void run_destructors()
{
    while (destructors) {
        Destructor* entry = destructors;
        destructors = entry->next;
        auto run = entry->run;
        void* arg = entry->arg;
        free(entry);
        run(arg);
    }
}

extern "C" {

__attribute__((visibility("hidden"))) void* __dso_handle = &__dso_handle;

int __cxa_atexit(void (*run)(void*), void* arg, void*)
{
    auto* entry = static_cast<Destructor*>(malloc(sizeof(Destructor)));
    if (!entry)
        return -1;
    if (!registered) {
        if (atexit(run_destructors) != 0) {
            free(entry);
            return -1;
        }
        registered = true;
    }
    *entry = {run, arg, destructors};
    destructors = entry;
    return 0;
}

void __cxa_pure_virtual()
{
    abort();
}

#ifdef XASH3D_HLSDK
/* HLSDK's own global objects register here during DT_INIT_ARRAY. */
__attribute__((visibility("default"))) int xash3d_hlsdk_has_destructors()
{
    return destructors != nullptr;
}
#endif

}
