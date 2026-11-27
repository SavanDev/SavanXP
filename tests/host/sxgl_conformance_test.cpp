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
