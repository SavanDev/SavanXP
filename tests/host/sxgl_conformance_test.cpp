// SxGL against the OpenGL specification, on the host
// (docs/roadmaps/open/SXGL_VERSIONS_ROADMAP.md).
//
// The conformance harness the roadmap's steps add to. Each case names the
// part of the specification it checks, so a failure says which promise
// broke, and the cases of a version are what lets GL_VERSION name it.
// sxgl_test.cpp stays the test of how SxGL draws; this one is the test of
// what it promises.
//
// Step 0 adds the binding: <GL/gl.h> and sxgl_get_proc_address agree, and
// GL_EXTENSIONS is what <GL/glext.h> says.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "GL/glext.h"
#include "sxgfx/sxgl.h"
}

#ifndef SXGL_GL_HEADER
#error "SXGL_GL_HEADER must name include/GL/gl.h"
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(nullptr);
}

using CaseFunction = void (*)();

struct Case {
    const char* section;
    const char* name;
    CaseFunction run;
};

std::vector<Case>& cases()
{
    static std::vector<Case> all;
    return all;
}

struct Register {
    Register(const char* section, const char* name, CaseFunction run)
    {
        cases().push_back({section, name, run});
    }
};

// SXGL_CASE(section, name): a case of the specification section `section`,
// named by its title, or "binding" for what OpenGL leaves to the platform,
// such as wglGetProcAddress and the headers.
#define SXGL_CASE(section, name)                                      \
    void sxgl_case_##name();                                          \
    const Register sxgl_register_##name(section, #name, sxgl_case_##name); \
    void sxgl_case_##name()

// A context over a small bitmap, current while it lives.
struct Context {
    std::vector<uint32_t> pixels;
    sx_bitmap bitmap{};
    sxgl_context* context = nullptr;

    Context(int width, int height) : pixels((size_t)width * (size_t)height, 0u)
    {
        bitmap.pixels = pixels.data();
        bitmap.info.width = (uint32_t)width;
        bitmap.info.height = (uint32_t)height;
        bitmap.info.pitch = (uint32_t)width * 4u;
        bitmap.info.bpp = 32u;
        bitmap.info.buffer_size = bitmap.info.pitch * bitmap.info.height;
        bitmap.format = SX_PIXEL_FORMAT_BGRX8888;
        bitmap.owns_pixels = 0;
        context = sxgl_context_create_ex(&bitmap, SXGL_CONTEXT_SOFTWARE);
        sxgl_make_current(context);
    }
    ~Context()
    {
        sxgl_make_current(nullptr);
        sxgl_context_destroy(context);
    }
};

// The entry points <GL/gl.h> declares, read from the header itself so a
// declaration added without registering it fails here.
std::vector<std::string> declared_entry_points()
{
    std::ifstream file(SXGL_GL_HEADER);
    std::stringstream text;
    text << file.rdbuf();
    const std::string source = text.str();
    const std::regex declaration(R"(APIENTRY\s+(gl[A-Z]\w*)\s*\()");
    std::vector<std::string> names;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), declaration); it != std::sregex_iterator(); ++it) {
        names.push_back((*it)[1].str());
    }
    return names;
}

std::vector<std::string> split_extensions(const char* text)
{
    std::vector<std::string> names;
    std::string current;
    for (const char* ch = text; *ch != '\0'; ++ch) {
        if (*ch == ' ') {
            names.push_back(current);
            current.clear();
        } else {
            current.push_back(*ch);
        }
    }
    names.push_back(current);
    return names;
}

SXGL_CASE("binding", every_declared_entry_point_resolves)
{
    const std::vector<std::string> names = declared_entry_points();
    check(names.size() >= 70, "<GL/gl.h> was read (" + std::to_string(names.size()) + " entry points)");
    std::set<sxgl_proc> distinct;
    int missing = 0;
    for (const std::string& name : names) {
        const sxgl_proc proc = sxgl_get_proc_address(name.c_str());
        if (proc == nullptr) {
            ++missing;
            check(false, name + " is declared and does not resolve");
        }
        distinct.insert(proc);
    }
    check(missing == 0, "every entry point <GL/gl.h> declares resolves by name");
    check(distinct.size() == names.size(), "each name resolves to a function of its own");
    // The other direction needs no check of its own: the table registers a
    // function by taking its address, so it cannot name one no header
    // declares.
}

SXGL_CASE("binding", a_name_resolves_to_the_function_itself)
{
    check(sxgl_get_proc_address("glClear") == (sxgl_proc)glClear, "glClear is glClear");
    check(sxgl_get_proc_address("glDrawElements") == (sxgl_proc)glDrawElements, "glDrawElements is glDrawElements");
    check(sxgl_get_proc_address("glTexImage2D") == (sxgl_proc)glTexImage2D, "glTexImage2D is glTexImage2D");
}

SXGL_CASE("binding", an_absent_name_resolves_to_null)
{
    // GL 1.1 calls SxGL does not implement yet: absent, not stubbed.
    for (const char* name : {"glPointSize", "glLineWidth", "glPolygonOffset", "glStencilFunc", "glNewList",
             "glReadPixels", "glTexGeni", "glPushAttrib"}) {
        check(sxgl_get_proc_address(name) == nullptr, std::string(name) + " is absent");
    }
    check(sxgl_get_proc_address("glclear") == nullptr, "names are case-sensitive");
    check(sxgl_get_proc_address("glClearX") == nullptr, "a longer name is not a prefix match");
    check(sxgl_get_proc_address("") == nullptr, "the empty name is absent");
    check(sxgl_get_proc_address(nullptr) == nullptr, "a null name is absent");
}

SXGL_CASE("binding", lookup_needs_no_current_context)
{
    sxgl_make_current(nullptr);
    check(sxgl_get_proc_address("glBegin") == (sxgl_proc)glBegin, "glBegin resolves with no context current");
}

SXGL_CASE("state queries: glGetString", version_string_has_the_specified_form)
{
    Context context(4, 4);
    check(context.context != nullptr, "a software context over a 4x4 bitmap");
    const char* version = (const char*)glGetString(GL_VERSION);
    check(version != nullptr, "GL_VERSION is a string");
    if (version != nullptr) {
        // "<major>.<minor>", optionally ".<release>", then a space and
        // vendor-specific text.
        check(std::regex_match(version, std::regex(R"(\d+\.\d+(\.\d+)?( .*)?)")),
            std::string("GL_VERSION begins with major.minor: \"") + version + "\"");
        check(std::strcmp(version, "1.1 SxGL subset") == 0,
            "GL_VERSION still says subset: 1.1 is not complete (roadmap step 1)");
    }
#ifdef GL_VERSION_1_1
    check(false, "GL_VERSION_1_1 is defined before 1.1 is complete");
#else
    check(true, "GL_VERSION_1_1 is not defined before 1.1 is complete");
#endif
}

SXGL_CASE("state queries: glGetString", extensions_are_the_ones_glext_h_declares)
{
    Context context(4, 4);
    const char* text = (const char*)glGetString(GL_EXTENSIONS);
    check(text != nullptr, "GL_EXTENSIONS is a string");
    if (text == nullptr) {
        return;
    }
    const std::vector<std::string> names = split_extensions(text);
    bool well_formed = true;
    for (const std::string& name : names) {
        well_formed = well_formed && name.rfind("GL_", 0) == 0 && name.size() > 3;
    }
    check(well_formed, std::string("names separated by single spaces, each GL_*: \"") + text + "\"");
    const std::set<std::string> advertised(names.begin(), names.end());
    check(advertised.size() == names.size(), "no extension is listed twice");
    // Every extension <GL/glext.h> defines a macro for is advertised, and
    // nothing else is.
    std::set<std::string> declared;
#ifdef GL_EXT_bgra
    declared.insert("GL_EXT_bgra");
#endif
    check(advertised == declared, "GL_EXTENSIONS matches <GL/glext.h>");
}

} // namespace

int main()
{
    const char* section = nullptr;
    for (const Case& entry : cases()) {
        if (section == nullptr || std::strcmp(section, entry.section) != 0) {
            section = entry.section;
            std::printf("%s\n", section);
        }
        std::printf(" %s\n", entry.name);
        entry.run();
    }
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
