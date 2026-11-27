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
// GL_EXTENSIONS is what <GL/glext.h> says. Step 1 adds OpenGL 1.1 a part at a
// time, each with the cases of its section.

#include <cmath>
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


// --- Lighting --------------------------------------------------------------

// The colour of pixel (x, y) from the top, as 0xRRGGBB.
uint32_t pixel(const Context& context, int x, int y)
{
    return context.pixels[(size_t)y * context.bitmap.info.width + (size_t)x] & 0x00ffffffu;
}

int channel(uint32_t colour, int shift)
{
    return (int)((colour >> shift) & 0xffu);
}

bool near(int got, double expected, int tolerance = 2)
{
    const int want = (int)(expected * 255.0 + 0.5);
    return got >= want - tolerance && got <= want + tolerance;
}

std::string rgb(uint32_t colour)
{
    char text[16];
    std::snprintf(text, sizeof(text), "#%06x", colour);
    return text;
}

// A square over the whole 8x8 viewport, at eye depth -1, facing the viewer
// (normal +z), wound counter-clockwise unless `clockwise`.
void lit_square(bool clockwise = false)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 10.0);
    glMatrixMode(GL_MODELVIEW);
    glClear(GL_COLOR_BUFFER_BIT);
    glNormal3f(0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    if (clockwise) {
        glVertex3f(-1.0f, -1.0f, -1.0f);
        glVertex3f(-1.0f, 1.0f, -1.0f);
        glVertex3f(1.0f, 1.0f, -1.0f);
        glVertex3f(1.0f, -1.0f, -1.0f);
    } else {
        glVertex3f(-1.0f, -1.0f, -1.0f);
        glVertex3f(1.0f, -1.0f, -1.0f);
        glVertex3f(1.0f, 1.0f, -1.0f);
        glVertex3f(-1.0f, 1.0f, -1.0f);
    }
    glEnd();
    glFinish();
}

// Lighting with nothing but what the case sets: no global ambient, a black
// material, light 0 directional along +z.
void dark_scene()
{
    const GLfloat black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const GLfloat along_z[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, black);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, black);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, black);
    glLightfv(GL_LIGHT0, GL_POSITION, along_z);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
}

bool equal4(const GLfloat* got, float a, float b, float c, float d)
{
    return got[0] == a && got[1] == b && got[2] == c && got[3] == d;
}

SXGL_CASE("lighting", initial_state_is_the_specified_one)
{
    Context context(8, 8);
    GLfloat v[4] = {};
    GLint i[4] = {};
    glGetIntegerv(GL_MAX_LIGHTS, i);
    check(i[0] >= 8, "GL_MAX_LIGHTS is at least 8 (" + std::to_string(i[0]) + ")");
    glGetLightfv(GL_LIGHT0, GL_DIFFUSE, v);
    check(equal4(v, 1, 1, 1, 1), "light 0 diffuse is (1, 1, 1, 1)");
    glGetLightfv(GL_LIGHT0, GL_SPECULAR, v);
    check(equal4(v, 1, 1, 1, 1), "light 0 specular is (1, 1, 1, 1)");
    glGetLightfv(GL_LIGHT7, GL_DIFFUSE, v);
    check(equal4(v, 0, 0, 0, 1), "light 7 diffuse is (0, 0, 0, 1)");
    glGetLightfv(GL_LIGHT7, GL_SPECULAR, v);
    check(equal4(v, 0, 0, 0, 1), "light 7 specular is (0, 0, 0, 1)");
    glGetLightfv(GL_LIGHT3, GL_POSITION, v);
    check(equal4(v, 0, 0, 1, 0), "a light's position is (0, 0, 1, 0)");
    glGetLightfv(GL_LIGHT3, GL_SPOT_DIRECTION, v);
    check(v[0] == 0 && v[1] == 0 && v[2] == -1, "its spot direction is (0, 0, -1)");
    glGetLightfv(GL_LIGHT3, GL_SPOT_CUTOFF, v);
    check(v[0] == 180.0f, "its spot cutoff is 180");
    glGetLightfv(GL_LIGHT3, GL_CONSTANT_ATTENUATION, v);
    check(v[0] == 1.0f, "its constant attenuation is 1");
    glGetMaterialfv(GL_FRONT, GL_AMBIENT, v);
    check(equal4(v, 0.2f, 0.2f, 0.2f, 1), "material ambient is (0.2, 0.2, 0.2, 1)");
    glGetMaterialfv(GL_BACK, GL_DIFFUSE, v);
    check(equal4(v, 0.8f, 0.8f, 0.8f, 1), "back material diffuse is (0.8, 0.8, 0.8, 1)");
    glGetMaterialfv(GL_FRONT, GL_SPECULAR, v);
    check(equal4(v, 0, 0, 0, 1), "material specular is (0, 0, 0, 1)");
    glGetMaterialfv(GL_FRONT, GL_SHININESS, v);
    check(v[0] == 0.0f, "material shininess is 0");
    glGetMaterialfv(GL_FRONT, GL_COLOR_INDEXES, v);
    check(v[0] == 0 && v[1] == 1 && v[2] == 1, "material colour indexes are (0, 1, 1)");
    glGetFloatv(GL_LIGHT_MODEL_AMBIENT, v);
    check(equal4(v, 0.2f, 0.2f, 0.2f, 1), "the light model ambient is (0.2, 0.2, 0.2, 1)");
    glGetIntegerv(GL_LIGHT_MODEL_LOCAL_VIEWER, i);
    check(i[0] == 0, "the viewer is infinite");
    glGetIntegerv(GL_LIGHT_MODEL_TWO_SIDE, i);
    check(i[0] == 0, "lighting is one-sided");
    glGetIntegerv(GL_COLOR_MATERIAL_FACE, i);
    check(i[0] == GL_FRONT_AND_BACK, "GL_COLOR_MATERIAL_FACE is GL_FRONT_AND_BACK");
    glGetIntegerv(GL_COLOR_MATERIAL_PARAMETER, i);
    check(i[0] == GL_AMBIENT_AND_DIFFUSE, "GL_COLOR_MATERIAL_PARAMETER is GL_AMBIENT_AND_DIFFUSE");
    check(glIsEnabled(GL_NORMALIZE) == GL_FALSE, "GL_NORMALIZE is off");
    check(glIsEnabled(GL_LIGHT7) == GL_FALSE, "light 7 is off");
    check(glGetError() == GL_NO_ERROR, "no query raised an error");
}

SXGL_CASE("lighting", parameters_and_their_errors)
{
    Context context(8, 8);
    GLfloat v[4] = {};
    GLint i[4] = {};

    glEnable(GL_LIGHT7);
    check(glIsEnabled(GL_LIGHT7) == GL_TRUE && glGetError() == GL_NO_ERROR, "GL_LIGHT7 is a capability");
    glEnable(GL_LIGHT0 + 8);
    check(glGetError() == GL_INVALID_ENUM, "GL_LIGHT0 + GL_MAX_LIGHTS is not");

    // The position is stored in eye coordinates: the modelview of the call.
    const GLfloat position[4] = {1.0f, 2.0f, 3.0f, 1.0f};
    glTranslatef(10.0f, 0.0f, 0.0f);
    glLightfv(GL_LIGHT1, GL_POSITION, position);
    glLoadIdentity();
    glGetLightfv(GL_LIGHT1, GL_POSITION, v);
    check(equal4(v, 11, 2, 3, 1), "glLightfv(GL_POSITION) is transformed by the modelview of the call");

    // Integer colours map linearly: the largest integer is 1.0, and back.
    const GLint full[4] = {2147483647, 0, -2147483647 - 1, 2147483647};
    glLightiv(GL_LIGHT2, GL_SPECULAR, full);
    glGetLightfv(GL_LIGHT2, GL_SPECULAR, v);
    check(v[0] == 1.0f && std::fabs(v[1]) < 1e-6f && v[2] == -1.0f, "glLightiv maps colour integers to [-1, 1]");
    glGetLightiv(GL_LIGHT2, GL_SPECULAR, i);
    check(i[0] == 2147483647 && i[2] == -2147483647 - 1, "glGetLightiv maps them back");
    glLighti(GL_LIGHT2, GL_SPOT_EXPONENT, 7);
    glGetLightiv(GL_LIGHT2, GL_SPOT_EXPONENT, i);
    check(i[0] == 7, "a scalar converts directly");
    check(glGetError() == GL_NO_ERROR, "no error so far");

    glLightf(GL_LIGHT0, GL_DIFFUSE, 1.0f);
    check(glGetError() == GL_INVALID_ENUM, "glLightf with a vector parameter is GL_INVALID_ENUM");
    glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 129.0f);
    check(glGetError() == GL_INVALID_VALUE, "a spot exponent above 128 is GL_INVALID_VALUE");
    glLightModelf(GL_LIGHT_MODEL_AMBIENT, 1.0f);
    check(glGetError() == GL_INVALID_ENUM, "glLightModelf with the ambient vector is GL_INVALID_ENUM");
    glMaterialf(GL_FRONT, GL_AMBIENT, 1.0f);
    check(glGetError() == GL_INVALID_ENUM, "glMaterialf takes only GL_SHININESS");
    glMaterialf(GL_FRONT, GL_SHININESS, 128.5f);
    check(glGetError() == GL_INVALID_VALUE, "a shininess above 128 is GL_INVALID_VALUE");
    glGetMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, v);
    check(glGetError() == GL_INVALID_ENUM, "glGetMaterial names one face");
    glGetMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, v);
    check(glGetError() == GL_INVALID_ENUM, "GL_AMBIENT_AND_DIFFUSE is not a query");

    glMateriali(GL_BACK, GL_SHININESS, 64);
    glGetMaterialiv(GL_BACK, GL_SHININESS, i);
    glGetMaterialfv(GL_FRONT, GL_SHININESS, v);
    check(i[0] == 64 && v[0] == 0.0f, "GL_BACK sets the back material alone");
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);
    glGetIntegerv(GL_LIGHT_MODEL_TWO_SIDE, i);
    check(i[0] == 1, "glLightModeli sets two-sided lighting");
    glColorMaterial(GL_BACK, GL_SPECULAR);
    glGetIntegerv(GL_COLOR_MATERIAL_FACE, i);
    check(i[0] == GL_BACK && glGetError() == GL_NO_ERROR, "glColorMaterial takes GL_BACK and GL_SPECULAR");

    // Inside glBegin/glEnd, glMaterial is legal and glLight is not.
    const GLfloat red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    glBegin(GL_TRIANGLES);
    glMaterialfv(GL_FRONT, GL_EMISSION, red);
    const GLenum material_error = glGetError();
    glLightfv(GL_LIGHT0, GL_DIFFUSE, red);
    const GLenum light_error = glGetError();
    glEnd();
    check(material_error == GL_NO_ERROR, "glMaterial between glBegin and glEnd is legal");
    check(light_error == GL_INVALID_OPERATION, "glLight between glBegin and glEnd is GL_INVALID_OPERATION");
}

SXGL_CASE("lighting", specular_follows_the_half_vector)
{
    // Light 0 at 60 degrees from the normal, infinite viewer: the half
    // vector is 30 degrees off it, so the specular term is cos(30)^10.
    Context context(8, 8);
    dark_scene();
    const GLfloat slanted[4] = {0.0f, std::sqrt(3.0f) / 2.0f, 0.5f, 0.0f};
    const GLfloat red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    glLightfv(GL_LIGHT0, GL_POSITION, slanted);
    glMaterialfv(GL_FRONT, GL_SPECULAR, red);
    glMaterialf(GL_FRONT, GL_SHININESS, 10.0f);
    lit_square();
    const double expected = std::pow(std::cos(3.14159265358979 / 6.0), 10.0);
    const uint32_t got = pixel(context, 4, 4);
    check(near(channel(got, 16), expected) && channel(got, 8) == 0 && channel(got, 0) == 0,
        "the highlight is cos(30)^10 of red: " + rgb(got));

    // No specular light where there is no diffuse light, even at shininess 0.
    const GLfloat behind[4] = {0.0f, 0.0f, -1.0f, 0.0f};
    glLightfv(GL_LIGHT0, GL_POSITION, behind);
    glMaterialf(GL_FRONT, GL_SHININESS, 0.0f);
    lit_square();
    check(pixel(context, 4, 4) == 0, "a light behind the surface adds no highlight: " + rgb(pixel(context, 4, 4)));
}

SXGL_CASE("lighting", a_local_viewer_moves_the_highlight)
{
    // Light along +z onto a square at depth -1 whose corners are at
    // (+-1, +-1). Infinite viewer: n.h = 1, the full highlight. Local viewer:
    // every corner sees the eye along (-+1, -+1, 1)/sqrt(3), so n.h is
    // (1 + 1/sqrt(3)) / |(1/sqrt(3), 1/sqrt(3), 1 + 1/sqrt(3))| at each.
    Context context(8, 8);
    dark_scene();
    const GLfloat white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glMaterialfv(GL_FRONT, GL_SPECULAR, white);
    glMaterialf(GL_FRONT, GL_SHININESS, 20.0f);
    lit_square();
    check(pixel(context, 4, 4) == 0xffffffu, "infinite viewer: the full highlight, " + rgb(pixel(context, 4, 4)));
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);
    lit_square();
    const double k = 1.0 / std::sqrt(3.0);
    const double n_dot_h = (1.0 + k) / std::sqrt(2.0 * k * k + (1.0 + k) * (1.0 + k));
    const uint32_t got = pixel(context, 4, 4);
    check(near(channel(got, 0), std::pow(n_dot_h, 20.0)), "local viewer: (n.h)^20, " + rgb(got));
}

SXGL_CASE("lighting", two_sided_lighting_lights_the_back_with_its_own_material)
{
    // The front material is diffuse green, the back one emits red and is
    // diffuse blue. The square is wound clockwise, so it shows its back.
    Context context(8, 8);
    dark_scene();
    const GLfloat green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    const GLfloat red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const GLfloat blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    glMaterialfv(GL_FRONT, GL_DIFFUSE, green);
    glMaterialfv(GL_BACK, GL_EMISSION, red);
    glMaterialfv(GL_BACK, GL_DIFFUSE, blue);
    lit_square(true);
    check(pixel(context, 4, 4) == 0x00ff00u, "one-sided: a back face shows the front material, " +
            rgb(pixel(context, 4, 4)));
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    lit_square(true);
    // The back face's normal is reversed, so the light along +z is behind it
    // and the blue diffuse adds nothing.
    check(pixel(context, 4, 4) == 0xff0000u, "two-sided: the back material, with the normal reversed, " +
            rgb(pixel(context, 4, 4)));
    lit_square(false);
    check(pixel(context, 4, 4) == 0x00ff00u, "two-sided: a front face keeps the front material, " +
            rgb(pixel(context, 4, 4)));
    glFrontFace(GL_CW);
    lit_square(true);
    check(pixel(context, 4, 4) == 0x00ff00u, "glFrontFace(GL_CW) decides which face that is, " +
            rgb(pixel(context, 4, 4)));
}

SXGL_CASE("lighting", normals_are_normalized_only_when_asked)
{
    // glScalef(2) shrinks the transformed normal to length 1/2.
    Context context(8, 8);
    dark_scene();
    const GLfloat grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    glMaterialfv(GL_FRONT, GL_DIFFUSE, grey);
    glScalef(2.0f, 2.0f, 2.0f);
    // The square is drawn at half size so it still covers the viewport.
    auto draw = [&]() {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 10.0);
        glMatrixMode(GL_MODELVIEW);
        glClear(GL_COLOR_BUFFER_BIT);
        glNormal3f(0.0f, 0.0f, 1.0f);
        glBegin(GL_QUADS);
        glVertex3f(-0.5f, -0.5f, -0.5f);
        glVertex3f(0.5f, -0.5f, -0.5f);
        glVertex3f(0.5f, 0.5f, -0.5f);
        glVertex3f(-0.5f, 0.5f, -0.5f);
        glEnd();
        glFinish();
    };
    draw();
    uint32_t got = pixel(context, 4, 4);
    check(near(channel(got, 8), 0.25), "GL_NORMALIZE off: the scaled normal halves the diffuse, " + rgb(got));
    glEnable(GL_NORMALIZE);
    draw();
    got = pixel(context, 4, 4);
    check(near(channel(got, 8), 0.5), "GL_NORMALIZE on: unit length again, " + rgb(got));
}

SXGL_CASE("lighting", eight_lights_each_contribute)
{
    Context context(8, 8);
    dark_scene();
    const GLfloat white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const GLfloat tenth[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    const GLfloat along_z[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    for (GLenum light = GL_LIGHT0; light <= GL_LIGHT7; ++light) {
        glLightfv(light, GL_DIFFUSE, tenth);
        glLightfv(light, GL_POSITION, along_z);
        glEnable(light);
    }
    lit_square();
    uint32_t got = pixel(context, 4, 4);
    check(near(channel(got, 16), 0.8), "eight lights of 0.1 add to 0.8, " + rgb(got));
    glDisable(GL_LIGHT7);
    lit_square();
    got = pixel(context, 4, 4);
    check(near(channel(got, 16), 0.7), "light 7 off takes its 0.1 away, " + rgb(got));
}

SXGL_CASE("lighting", colour_material_tracks_the_current_colour)
{
    Context context(8, 8);
    GLfloat v[4] = {};
    glColor4f(0.1f, 0.2f, 0.3f, 0.4f);
    glEnable(GL_COLOR_MATERIAL);
    glGetMaterialfv(GL_BACK, GL_DIFFUSE, v);
    check(equal4(v, 0.1f, 0.2f, 0.3f, 0.4f), "enabling it copies the current colour into the material");
    glColor4f(0.5f, 0.6f, 0.7f, 0.8f);
    glGetMaterialfv(GL_FRONT, GL_AMBIENT, v);
    check(equal4(v, 0.5f, 0.6f, 0.7f, 0.8f), "a new colour reaches the tracked parameters");
    glGetMaterialfv(GL_FRONT, GL_SPECULAR, v);
    check(equal4(v, 0, 0, 0, 1), "an untracked one keeps its value");
    glDisable(GL_COLOR_MATERIAL);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glGetMaterialfv(GL_FRONT, GL_DIFFUSE, v);
    check(equal4(v, 0.5f, 0.6f, 0.7f, 0.8f), "disabled, the material keeps the last tracked colour");
    glColorMaterial(GL_FRONT, GL_EMISSION);
    glEnable(GL_COLOR_MATERIAL);
    glColor4f(0.25f, 0.5f, 0.75f, 1.0f);
    glGetMaterialfv(GL_FRONT, GL_EMISSION, v);
    GLfloat back[4] = {};
    glGetMaterialfv(GL_BACK, GL_EMISSION, back);
    check(equal4(v, 0.25f, 0.5f, 0.75f, 1.0f) && equal4(back, 0, 0, 0, 1),
        "glColorMaterial(GL_FRONT, GL_EMISSION) tracks the front emission alone");
}


// --- Texturing: mipmaps ----------------------------------------------------

// The solid colour of level i of the test chain, as 0xRRGGBB.
const uint32_t kLevelColours[7] = {0xff0000u, 0x00ff00u, 0x0000ffu, 0xffff00u, 0x00ffffu, 0xff00ffu, 0xffffffu};

// Level `level` of a 64x64 chain: `size` by `size`, all one colour.
void solid_level(int level, int size, uint32_t colour, GLint internal_format = GL_RGBA)
{
    std::vector<unsigned char> texels((size_t)size * (size_t)size * 4u);
    for (size_t index = 0; index < (size_t)size * (size_t)size; ++index) {
        texels[index * 4 + 0] = (unsigned char)(colour >> 16);
        texels[index * 4 + 1] = (unsigned char)(colour >> 8);
        texels[index * 4 + 2] = (unsigned char)colour;
        texels[index * 4 + 3] = 255;
    }
    glTexImage2D(GL_TEXTURE_2D, level, internal_format, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
}

// A bound 64x64 texture with all seven levels, level i in kLevelColours[i].
GLuint mip_chain(GLenum min_filter)
{
    GLuint name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D, name);
    for (int level = 0; level < 7; ++level) {
        solid_level(level, 64 >> level, kLevelColours[level]);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)min_filter);
    return name;
}

// A square over the 16x16 viewport textured with s and t from 0 to
// `repeat`: rho is 64 * repeat / 16 texels per pixel, the same everywhere.
void textured_square(float repeat)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(repeat, 0.0f);
    glVertex2f(1.0f, -1.0f);
    glTexCoord2f(repeat, repeat);
    glVertex2f(1.0f, 1.0f);
    glTexCoord2f(0.0f, repeat);
    glVertex2f(-1.0f, 1.0f);
    glEnd();
    glFinish();
}

// The repeat that makes lambda = log2(rho) the given value.
float repeat_for_lambda(double lambda)
{
    return (float)(std::pow(2.0, lambda) * 16.0 / 64.0);
}

SXGL_CASE("texturing: mipmaps", the_level_follows_lambda)
{
    Context context(16, 16);
    mip_chain(GL_NEAREST_MIPMAP_NEAREST);
    struct Expect {
        double lambda;
        int level;
        const char* why;
    };
    for (const Expect& expect : {Expect{-1.0, 0, "magnified: level 0"}, Expect{1.0, 1, "lambda 1: level 1"},
             Expect{1.4, 1, "lambda 1.4 rounds to level 1"}, Expect{1.6, 2, "lambda 1.6 rounds to level 2"},
             Expect{3.0, 3, "lambda 3: level 3"}, Expect{8.0, 6, "past the last level: the 1x1 one"}}) {
        textured_square(repeat_for_lambda(expect.lambda));
        const uint32_t got = pixel(context, 8, 8);
        check(got == kLevelColours[expect.level], std::string("GL_NEAREST_MIPMAP_NEAREST, ") + expect.why + ": " +
                rgb(got));
    }
}

SXGL_CASE("texturing: mipmaps", linear_mipmaps_blend_the_two_levels_around_lambda)
{
    Context context(16, 16);
    mip_chain(GL_LINEAR_MIPMAP_LINEAR);
    textured_square(repeat_for_lambda(1.5));
    uint32_t got = pixel(context, 8, 8);
    check(channel(got, 16) == 0 && near(channel(got, 8), 0.5, 3) && near(channel(got, 0), 0.5, 3),
        "lambda 1.5: half level 1, half level 2, " + rgb(got));
    textured_square(repeat_for_lambda(2.75));
    got = pixel(context, 8, 8);
    // Level 2 is blue, level 3 yellow: a quarter of blue, three of yellow.
    check(near(channel(got, 16), 0.75, 3) && near(channel(got, 8), 0.75, 3) && near(channel(got, 0), 0.25, 3),
        "lambda 2.75: a quarter of level 2, three quarters of level 3, " + rgb(got));

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    textured_square(repeat_for_lambda(1.5));
    got = pixel(context, 8, 8);
    check(channel(got, 16) == 0 && near(channel(got, 8), 0.5, 3) && near(channel(got, 0), 0.5, 3),
        "GL_NEAREST_MIPMAP_LINEAR blends the levels too, " + rgb(got));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    textured_square(repeat_for_lambda(1.6));
    got = pixel(context, 8, 8);
    check(got == kLevelColours[2], "GL_LINEAR_MIPMAP_NEAREST takes the nearest level alone, " + rgb(got));
}

SXGL_CASE("texturing: mipmaps", an_incomplete_texture_disables_texturing)
{
    Context context(16, 16);
    glColor3f(0.5f, 0.5f, 0.5f);
    GLuint name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D, name);
    solid_level(0, 64, kLevelColours[0]);
    textured_square(1.0f);
    check(near(channel(pixel(context, 8, 8), 16), 0.5) && near(channel(pixel(context, 8, 8), 8), 0.5),
        "the initial minification filter mipmaps: level 0 alone is incomplete, " + rgb(pixel(context, 8, 8)));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    textured_square(1.0f);
    check(pixel(context, 8, 8) == 0x800000u || pixel(context, 8, 8) == 0x7f0000u,
        "with GL_LINEAR, level 0 alone is complete, " + rgb(pixel(context, 8, 8)));

    // Complete, lambda 2 takes level 2, blue, modulated by the grey: 0x00007f.
    // Incomplete, the square is the grey itself.
    mip_chain(GL_NEAREST_MIPMAP_NEAREST);
    solid_level(3, 4, kLevelColours[3]); // level 3 of a 64x64 chain is 8x8
    textured_square(1.0f);
    check(pixel(context, 8, 8) == 0x808080u, "a level of the wrong size makes it incomplete, " +
            rgb(pixel(context, 8, 8)));
    solid_level(3, 8, kLevelColours[3], GL_RGB);
    textured_square(1.0f);
    check(pixel(context, 8, 8) == 0x808080u, "so does a level of another internal format, " +
            rgb(pixel(context, 8, 8)));
    solid_level(3, 8, kLevelColours[3]);
    textured_square(1.0f);
    check(pixel(context, 8, 8) == 0x00007fu || pixel(context, 8, 8) == 0x000080u,
        "and redefining it right completes it again, " + rgb(pixel(context, 8, 8)));
}

SXGL_CASE("texturing: mipmaps", a_level_is_sampled_at_its_own_scale)
{
    // Level 1 (32x32) is red on its left half and green on its right. At
    // lambda 1 the square spans s from 0 to 0.5: the left half of every
    // level, so red, never green.
    Context context(16, 16);
    mip_chain(GL_NEAREST_MIPMAP_NEAREST);
    std::vector<unsigned char> halves(32 * 32 * 4, 0);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            unsigned char* texel = &halves[((size_t)y * 32 + (size_t)x) * 4];
            texel[x < 16 ? 0 : 1] = 255;
            texel[3] = 255;
        }
    }
    glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, halves.data());
    textured_square(repeat_for_lambda(1.0));
    bool all_red = true;
    for (int x = 0; x < 16; ++x) {
        all_red = all_red && pixel(context, x, 8) == 0xff0000u;
    }
    check(all_red, "s from 0 to 0.5 stays in the left half of level 1, " + rgb(pixel(context, 12, 8)));
}

SXGL_CASE("texturing: mipmaps", rho_is_the_larger_of_the_two_directions)
{
    // Squeezed along one axis only: rho takes the larger scale factor.
    Context context(16, 16);
    mip_chain(GL_NEAREST_MIPMAP_NEAREST);
    auto stretched = [&](float s_repeat, float t_repeat) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_2D);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f);
        glVertex2f(-1.0f, -1.0f);
        glTexCoord2f(s_repeat, 0.0f);
        glVertex2f(1.0f, -1.0f);
        glTexCoord2f(s_repeat, t_repeat);
        glVertex2f(1.0f, 1.0f);
        glTexCoord2f(0.0f, t_repeat);
        glVertex2f(-1.0f, 1.0f);
        glEnd();
        glFinish();
    };
    stretched(repeat_for_lambda(2.0), repeat_for_lambda(-1.0));
    check(pixel(context, 8, 8) == kLevelColours[2], "minified along x alone: level 2, " + rgb(pixel(context, 8, 8)));
    stretched(repeat_for_lambda(-1.0), repeat_for_lambda(2.0));
    check(pixel(context, 8, 8) == kLevelColours[2], "minified along y alone: level 2, " + rgb(pixel(context, 8, 8)));
}

SXGL_CASE("texturing: mipmaps", the_switch_to_minification_is_at_c)
{
    // Level 0 is a texel checkerboard of black and white. At lambda 0.25 the
    // texture is minified, but with a GL_LINEAR magnification filter and a
    // GL_NEAREST_MIPMAP_* minification one, c is 0.5 and the magnification
    // filter applies: bilinear, so greys. With GL_NEAREST for both, c is 0
    // and the nearest texel of level 0 is taken: black or white only.
    Context context(16, 16);
    mip_chain(GL_NEAREST_MIPMAP_NEAREST);
    std::vector<unsigned char> checker(64 * 64 * 4, 0);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            unsigned char* texel = &checker[((size_t)y * 64 + (size_t)x) * 4];
            const unsigned char value = ((x ^ y) & 1) != 0 ? 255 : 0;
            texel[0] = texel[1] = texel[2] = value;
            texel[3] = 255;
        }
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, checker.data());
    auto greys = [&]() {
        int count = 0;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const int value = channel(pixel(context, x, y), 0);
                count += value != 0 && value != 255 ? 1 : 0;
            }
        }
        return count;
    };
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    textured_square(repeat_for_lambda(0.25));
    const int linear_greys = greys();
    check(linear_greys > 0, "GL_LINEAR magnification below c = 0.5 filters bilinearly (" +
            std::to_string(linear_greys) + " grey pixels)");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    textured_square(repeat_for_lambda(0.25));
    check(greys() == 0, "GL_NEAREST for both: c = 0, the nearest texel (" + std::to_string(greys()) + " grey pixels)");
}

SXGL_CASE("texturing: mipmaps", under_perspective_the_level_grows_with_distance)
{
    // A floor from just below the eye to far away, under the solid chain:
    // going up the screen towards the horizon, each row samples the same
    // level as the row below it or a later one, from level 0 to the last.
    Context context(32, 32);
    mip_chain(GL_NEAREST_MIPMAP_NEAREST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 400.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex3f(-1.0f, -1.0f, -1.0f);
    glTexCoord2f(0.25f, 0.0f);
    glVertex3f(1.0f, -1.0f, -1.0f);
    glTexCoord2f(0.25f, 50.0f);
    glVertex3f(1.0f, -1.0f, -390.0f);
    glTexCoord2f(0.0f, 50.0f);
    glVertex3f(-1.0f, -1.0f, -390.0f);
    glEnd();
    glFinish();
    auto level_of = [](uint32_t colour) {
        for (int level = 0; level < 7; ++level) {
            if (kLevelColours[level] == colour) {
                return level;
            }
        }
        return -1;
    };
    int previous = 0;
    int highest = 0;
    bool monotonic = true;
    bool known = true;
    std::string levels;
    for (int y = 31; y >= 17; --y) {
        const int level = level_of(pixel(context, 16, y));
        known = known && level >= 0;
        monotonic = monotonic && level >= previous;
        previous = level > previous ? level : previous;
        highest = level > highest ? level : highest;
        levels += std::to_string(level);
    }
    check(known, "every row samples one level of the chain: " + levels);
    check(monotonic, "the level never falls towards the horizon: " + levels);
    check(level_of(pixel(context, 16, 31)) == 0 && highest == 6, "from level 0 near the eye to the last: " + levels);
}

SXGL_CASE("texturing: mipmaps", a_rectangular_chain_ends_at_one_by_one)
{
    // 8x2, 4x1, 2x1, 1x1: once one dimension reaches 1 it stays there.
    Context context(16, 16);
    GLuint name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D, name);
    const int sizes[4][2] = {{8, 2}, {4, 1}, {2, 1}, {1, 1}};
    for (int level = 0; level < 4; ++level) {
        std::vector<unsigned char> texels((size_t)sizes[level][0] * (size_t)sizes[level][1] * 4u, 0);
        for (size_t index = 0; index < texels.size() / 4; ++index) {
            texels[index * 4 + 1] = 255;
            texels[index * 4 + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, sizes[level][0], sizes[level][1], 0, GL_RGBA, GL_UNSIGNED_BYTE,
            texels.data());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    textured_square(8.0f);
    check(pixel(context, 8, 8) == 0x00ff00u, "8x2 to 1x1 is complete and samples, " + rgb(pixel(context, 8, 8)));
}

SXGL_CASE("texturing: mipmaps", levels_are_specified_and_updated_one_at_a_time)
{
    Context context(16, 16);
    mip_chain(GL_NEAREST_MIPMAP_NEAREST);
    const unsigned char white[4 * 4 * 4] = {};
    std::vector<unsigned char> grey(32 * 32 * 4, 128);
    glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, grey.data());
    textured_square(repeat_for_lambda(1.0));
    check(pixel(context, 8, 8) == 0x808080u, "glTexSubImage2D at level 1 changes level 1, " +
            rgb(pixel(context, 8, 8)));
    textured_square(repeat_for_lambda(2.0));
    check(pixel(context, 8, 8) == kLevelColours[2], "and nothing else, " + rgb(pixel(context, 8, 8)));
    check(glGetError() == GL_NO_ERROR, "no error so far");

    glTexImage2D(GL_TEXTURE_2D, -1, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    check(glGetError() == GL_INVALID_VALUE, "a negative level is GL_INVALID_VALUE");
    glTexImage2D(GL_TEXTURE_2D, 11, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    check(glGetError() == GL_INVALID_VALUE, "a level past log2(GL_MAX_TEXTURE_SIZE) is GL_INVALID_VALUE");
    GLuint fresh = 0;
    glGenTextures(1, &fresh);
    glBindTexture(GL_TEXTURE_2D, fresh);
    solid_level(0, 4, kLevelColours[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);
    check(glGetError() == GL_INVALID_OPERATION, "glTexSubImage2D at a level never specified is GL_INVALID_OPERATION");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    check(glGetError() == GL_INVALID_ENUM, "a mipmapping magnification filter is GL_INVALID_ENUM");
    for (GLenum filter : {GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
             GL_LINEAR_MIPMAP_LINEAR}) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
    }
    check(glGetError() == GL_NO_ERROR, "the four mipmapping minification filters are accepted");
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
