#include "extdll.h"
#include "eiface.h"
#include <dlfcn.h>
#include "vcs_info.h"

constexpr char module_path[] = "/games/xash3d/valve/dlls/hl_savanxp_amd64.so";
static bool commit_registered;
static bool skill_requested;
static cvar_t engine_cvar;

static cvar_t* get_cvar(const char*)
{
    return &engine_cvar;
}

static void register_cvar(cvar_t* cvar)
{
    if (strcmp(cvar->name, "sv_game_build_commit") == 0)
        commit_registered = strcmp(cvar->string, HLSDK_COMMIT) == 0;
}

static void server_command(const char* command)
{
    skill_requested = strcmp(command, "exec skill.cfg\n") == 0;
}

static int fail(const char* reason)
{
    fprintf(stderr, "xash3d: %s\n", reason);
    return 1;
}

extern "C" int main()
{
    void* module = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    if (!module)
        return fail(dlerror());

    auto pass_engine = reinterpret_cast<void (*)(enginefuncs_t*, globalvars_t*)>(
        dlsym(module, "GiveFnptrsToDll"));
    auto get_entities = reinterpret_cast<int (*)(DLL_FUNCTIONS*, int*)>(
        dlsym(module, "GetEntityAPI2"));
    auto has_destructors = reinterpret_cast<int (*)()>(
        dlsym(module, "xash3d_hlsdk_has_destructors"));
    void* worldspawn = dlsym(module, "worldspawn");
    if (!pass_engine || !get_entities || !worldspawn)
        return fail("HLSDK server exports missing");
    if (!has_destructors || !has_destructors())
        return fail("HLSDK C++ constructors did not register a destructor");

    Dl_info symbol = {};
    if (!dladdr(worldspawn, &symbol) || !symbol.dli_fname || !symbol.dli_sname ||
        strcmp(symbol.dli_fname, module_path) != 0 || strcmp(symbol.dli_sname, "worldspawn") != 0 ||
        symbol.dli_saddr != worldspawn)
        return fail("HLSDK worldspawn did not resolve by address");

    enginefuncs_t engine = {};
    globalvars_t globals = {};
    engine.pfnCVarGetPointer = get_cvar;
    engine.pfnCVarRegister = register_cvar;
    engine.pfnServerCommand = server_command;
    pass_engine(&engine, &globals);

    DLL_FUNCTIONS entities = {};
    int version = INTERFACE_VERSION;
    if (!get_entities(&entities, &version) || !entities.pfnGameInit || !entities.pfnGetGameDescription)
        return fail("HLSDK did not provide its entity interface");
    entities.pfnGameInit();
    if (!commit_registered || !skill_requested || strcmp(entities.pfnGetGameDescription(), "Half-Life") != 0)
        return fail("HLSDK game initialization failed");

    if (dlclose(module) != 0)
        return fail("Could not release HLSDK module reference");
    puts("HLSDK server module initialized");
    return 0;
}
