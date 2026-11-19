/* The SxBASIC core on the host (docs/userland/SXBASIC.md): programs and the
 * output they must print, compile errors and runtime errors, and the console
 * sample project against its expected-output.txt.
 *
 * Usage: savanxp-sxbasic-test <system/sxbasic/samples directory> */
#define _POSIX_C_SOURCE 200809L
#include "sxbasic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static char g_out[65536];
static size_t g_out_length;
static const char *g_input;

static void host_write(void *user, const char *text, size_t length)
{
    (void)user;
    if (g_out_length + length < sizeof(g_out)) {
        memcpy(g_out + g_out_length, text, length);
        g_out_length += length;
        g_out[g_out_length] = '\0';
    }
}

static int host_read_file(void *user, const char *path, char **data, size_t *size)
{
    (void)user;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return SXB_ERR_FILE_NOT_FOUND;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    *data = malloc((size_t)length + 1u);
    *size = fread(*data, 1, (size_t)length, file);
    fclose(file);
    return 0;
}

static int host_write_file(void *user, const char *path, const char *data, size_t size)
{
    (void)user;
    FILE *file = fopen(path, "wb");
    if (file == NULL) return SXB_ERR_PERMISSION;
    fwrite(data, 1, size, file);
    fclose(file);
    return 0;
}

static int host_file_exists(void *user, const char *path)
{
    (void)user;
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static long host_read_line(void *user, char **line)
{
    (void)user;
    if (g_input == NULL || *g_input == '\0') return -1;
    const char *end = strchr(g_input, '\n');
    size_t n = end ? (size_t)(end - g_input) : strlen(g_input);
    *line = malloc(n + 1u);
    memcpy(*line, g_input, n);
    (*line)[n] = '\0';
    g_input = end ? end + 1 : g_input + n;
    return (long)n;
}

static uint64_t host_clock(void *user)
{
    (void)user;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static const struct sxb_host g_host = {NULL, host_write, host_read_file, host_write_file, host_file_exists,
                                       host_read_line, host_clock};

static int g_failures;
static int g_cases;

/* Runs a program; the text is its output, plus "!E<n>@<line>" for an
 * uncaught runtime error, or "!C<line>:<message>" for a compile error. */
static const char *run(const char *source, const char *input)
{
    struct sxb_diagnostic diag;
    g_out_length = 0;
    g_out[0] = '\0';
    g_input = input;
    struct sxb_program *program = sxb_compile_source("Test.bas", source, strlen(source), &diag);
    if (program == NULL) {
        snprintf(g_out, sizeof(g_out), "!C%d:%s", diag.line, diag.message);
        return g_out;
    }
    struct sxb_vm *vm = sxb_vm_create(program, &g_host, "a b", "/app");
    enum sxb_status status = sxb_vm_run(vm, 0);
    if (status == SXB_ERROR) {
        const struct sxb_runtime_error *error = sxb_vm_error(vm);
        char tail[64];
        snprintf(tail, sizeof(tail), "!E%d@%d", error->number, error->line);
        host_write(NULL, tail, strlen(tail));
    }
    sxb_vm_destroy(vm);
    sxb_program_free(program);
    return g_out;
}

static void expect(int line, const char *source, const char *expected)
{
    g_cases++;
    const char *got = run(source, NULL);
    int ok = expected[0] == '!' && expected[1] == 'C' ? strncmp(got, expected, strlen(expected)) == 0
                                                       : strcmp(got, expected) == 0;
    if (!ok) {
        g_failures++;
        fprintf(stderr, "sxbasic-test:%d: expected\n%s\n--- got\n%s\n---\n", line, expected, got);
    }
}

#define EXPECT(source, expected) expect(__LINE__, source, expected)
#define MAIN(body) "Sub Main()\n" body "End Sub\n"

static void language(void)
{
    EXPECT(MAIN("Print \"hi\"\n"), "hi\n");
    EXPECT(MAIN("Print 1 + 2 * 3; \n"), "7");
    EXPECT(MAIN("Print 7 / 2\nPrint 7 \\ 2\nPrint -7 Mod 3\nPrint 2 ^ 10\n"), "3.5\n3\n-1\n1024\n");
    EXPECT(MAIN("Print -2 ^ 2\nPrint 2 ^ -1\n"), "-4\n0.5\n");
    EXPECT(MAIN("Print 1 / 3\nPrint 4.0\nPrint 1e20\nPrint 0.0001\nPrint 0.00001\nPrint -1.5\n"),
           "0.333333333333333\n4\n1E+20\n0.0001\n1E-05\n-1.5\n");
    EXPECT(MAIN("Print 0.1 + 0.2\nPrint 100000000000000\nPrint 123456789.125\n"),
           "0.3\n100000000000000\n123456789.125\n");
    EXPECT(MAIN("Print \"a\" & 1 & True & 2.5\n"), "a1True2.5\n");
    EXPECT(MAIN("Print \"say \"\"hi\"\"\"\n"), "say \"hi\"\n");
    EXPECT(MAIN("Dim x As Integer = 5\nx = x + 1\nPrint x\n"), "6\n");
    EXPECT(MAIN("Dim d As Double = 1\nd = d / 4\nPrint d\n"), "0.25\n");
    EXPECT(MAIN("If 1 < 2 And 2 < 3 Then Print \"yes\"\nIf Not (1 = 1) Or False Then Print \"no\"\n"), "yes\n");
    EXPECT(MAIN("Dim i As Integer\nFor i = 1 To 3\nIf i = 2 Then\nPrint \"two\"\nElseIf i = 3 Then\nPrint \"three\"\nElse\nPrint i\nEnd If\nNext\n"),
           "1\ntwo\nthree\n");
    EXPECT(MAIN("Dim i As Integer\nFor i = 10 To 1 Step -4\nPrint i;\nNext i\nPrint\n"), "1062\n");
    EXPECT(MAIN("Dim x As Double\nFor x = 0 To 1 Step 0.5\nPrint x;\nPrint \" \";\nNext\n"), "0 0.5 1 ");
    EXPECT(MAIN("Dim n As Integer\nDo While n < 3\nn = n + 1\nLoop\nPrint n\nDo\nn = n - 1\nLoop Until n = 0\nPrint n\n"),
           "3\n0\n");
    EXPECT(MAIN("Dim n As Integer\nDo\nn = n + 1\nIf n = 4 Then Exit Do\nLoop\nPrint n\n"), "4\n");
    EXPECT(MAIN("Dim s As String = \"b\"\nSelect Case s\nCase \"a\"\nPrint 1\nCase \"b\", \"c\"\nPrint 2\nCase Else\nPrint 3\nEnd Select\n"),
           "2\n");
    EXPECT(MAIN("Dim d As Double = 2.5\nSelect Case d\nCase 1 To 2\nPrint \"low\"\nCase Is > 2\nPrint \"high\"\nEnd Select\n"),
           "high\n");
    EXPECT(MAIN("Dim a(2) As Integer\na(0) = 5\na(2) = 7\nPrint a(0) + a(1) + a(2); UBound(a)\n"), "122\n");
    EXPECT(MAIN("Dim a() As String\nPrint UBound(a)\nReDim a(1)\na(1) = \"x\"\nReDim Preserve a(2)\nPrint a(1); UBound(a)\n"),
           "-1\nx2\n");
    EXPECT(MAIN("Dim a(1) As Integer\nDim b() As Integer\nb = a\nb(0) = 9\nPrint a(0); b(0)\n"), "09\n");
    EXPECT(MAIN("Dim w As String\nFor Each w In Split(\"x,y\", \",\")\nPrint w\nNext\n"), "x\ny\n");
    EXPECT(MAIN("Print Join(Split(\"a b c\", \" \"), \"-\")\nPrint UBound(Split(\"\", \",\"))\n"), "a-b-c\n-1\n");
    EXPECT("Sub Swap(ByRef a As Integer, ByRef b As Integer)\nDim t As Integer = a\na = b\nb = t\nEnd Sub\n"
           MAIN("Dim x As Integer = 1\nDim y As Integer = 2\nSwap x, y\nPrint x; y\n"), "21\n");
    EXPECT("Sub Bump(n As Integer)\nn = n + 1\nEnd Sub\n" MAIN("Dim x As Integer = 1\nBump x\nPrint x\n"), "1\n");
    EXPECT("Sub Fill(v() As Integer)\nReDim v(2)\nv(2) = 4\nEnd Sub\n" MAIN("Dim a() As Integer\nFill a\nPrint a(2)\n"), "4\n");
    EXPECT("Function Fact(n As Integer) As Integer\nIf n <= 1 Then Return 1\nReturn n * Fact(n - 1)\nEnd Function\n"
           MAIN("Print Fact(20)\n"), "2432902008176640000\n");
    EXPECT("Function Nothing1() As String\nEnd Function\n" MAIN("Print \"[\" & Nothing1() & \"]\"\n"), "[]\n");
    EXPECT("Dim counter As Integer\nConst Base = 10\nConst Name = \"n\" & Base\n"
           "Sub Tick()\ncounter = counter + Base\nEnd Sub\n" MAIN("Tick\nTick()\nPrint counter; Name\n"), "20n10\n");
    EXPECT(MAIN("Const Half = 1 / 2\nPrint Half\n"), "0.5\n");
    EXPECT(MAIN("Rem a comment\nPrint 1 ' another\nPrint \"a\" & _\n  \"b\"\n"), "1\nab\n");
    EXPECT(MAIN("dim X as integer = 3\nprint x\n"), "3\n");
}

static void builtins(void)
{
    EXPECT(MAIN("Print Len(\"Año\"); Left(\"Año\", 2); Right(\"Año\", 1); Mid(\"Año\", 2); Mid(\"hello\", 2, 3)\n"),
           "3Añoñoell\n");
    EXPECT(MAIN("Print InStr(\"hello\", \"l\"); InStr(\"hello\", \"l\", 4); InStr(\"hello\", \"z\"); InStr(\"ñab\", \"b\")\n"),
           "3403\n");
    EXPECT(MAIN("Print UCase(\"abc\"); LCase(\"DEF\"); \"[\"; Trim(\"  x  \"); \"]\"; LTrim(\" y\"); RTrim(\"z \"); \"|\"\n"),
           "ABCdef[x]yz|\n");
    EXPECT(MAIN("Print Replace(\"a-b-c\", \"-\", \"+\"); Space(2); \"|\"; Chr(65); Asc(\"a\"); Chr(241)\n"),
           "a+b+c  |A97ñ\n");
    EXPECT(MAIN("Print CInt(2.5); CInt(-2.5); CInt(\"42\"); CInt(\" 3.4 \"); CDbl(\"1.25\")\n"), "3-34231.25\n");
    EXPECT(MAIN("Print CStr(12) & CStr(True); CBool(\"true\"); IsNumeric(\"1e3\"); IsNumeric(\"x\")\n"),
           "12TrueTrueTrueFalse\n");
    EXPECT(MAIN("Print Abs(-3); Abs(-2.5); Sgn(-7); Sgn(0.5); Sqr(16); Int(-1.5); Fix(-1.5)\n"), "32.5-114-2-1\n");
    EXPECT(MAIN("Print Round(2.5); Round(-2.5); Round(1.2345, 2); Round(21.456, 1)\n"), "3-31.2321.5\n");
    EXPECT(MAIN("Randomize 7\nDim r As Double = Rnd()\nPrint r >= 0 And r < 1\n"), "True\n");
    EXPECT(MAIN("Print Command(); AppPath()\n"), "a b/app\n");
    EXPECT(MAIN("Print NewLine = Chr(10); Len(Tab)\n"), "True1\n");
}

static void errors(void)
{
    EXPECT(MAIN("Print 1 \\ 0\n"), "!E11@2");
    EXPECT(MAIN("Print 1 / 0\n"), "!E11@2");
    EXPECT(MAIN("Dim a(1) As Integer\nPrint a(2)\n"), "!E9@3");
    EXPECT(MAIN("Print CInt(\"x\")\n"), "!E13@2");
    EXPECT(MAIN("Dim x As Integer = 9223372036854775807\nx = x + 1\n"), "!E6@3");
    EXPECT(MAIN("Print Sqr(-1)\n"), "!E5@2");
    EXPECT(MAIN("Dim i As Integer\nFor i = 1 To 2 Step 0\nNext\n"), "!E5@3");
    EXPECT("Sub Deep(n As Integer)\nDeep n + 1\nEnd Sub\n" MAIN("Deep 0\n"), "!E28@2");
    EXPECT(MAIN("Print ReadAllText(\"relative\")\n"), "!E75@2");
    EXPECT(MAIN("Try\nPrint 1\nErr.Raise 1001, \"mine\"\nPrint 2\nCatch\nPrint Err.Number; Err.Description\nEnd Try\nPrint \"after\"\n"),
           "1\n1001mine\nafter\n");
    EXPECT("Sub Fail()\nDim a() As Integer\na(0) = 1\nEnd Sub\n"
           MAIN("Try\nFail\nCatch\nPrint Err.Number; \" \"; Err.Description\nEnd Try\n"), "9 Index out of range\n");
    EXPECT(MAIN("Dim i As Integer\nFor i = 1 To 3\nTry\nIf i = 2 Then Exit For\nCatch\nEnd Try\nNext\nErr.Raise 7\n"),
           "!E7@9");
    EXPECT(MAIN("Print 1\nEnd\nPrint 2\n"), "1\n");
    /* compile errors: the line, and the message's start */
    EXPECT(MAIN("Print \"a\" + \"b\"\n"), "!C2:\"+\" does not join strings");
    EXPECT(MAIN("Dim x As Integer = 1.5\n"), "!C2:the initial value: a Double does not become an Integer");
    EXPECT(MAIN("Dim n As Integer\nIf n Then Print 1\n"), "!C3:an If condition has to be a Boolean");
    EXPECT(MAIN("Dim a, b As Integer\n"), "!C2:\"a\" needs a type");
    EXPECT(MAIN("Print y\n"), "!C2:nothing named \"y\" is declared");
    EXPECT(MAIN("Dim x As Integer\nDim x As String\n"), "!C3:\"x\" is already declared");
    EXPECT("Sub S(ByRef v As Integer)\nEnd Sub\n" MAIN("S 1 + 2\n"), "!C4:argument 1 of S is passed by reference");
    EXPECT("Function F() As Integer\nF = 1\nEnd Function\n" MAIN(""), "!C2:a Function gives its value with Return");
    EXPECT(MAIN("MsgBox \"x\"\n"), "!C2:MsgBox shows a window");
    EXPECT(MAIN("Dim i As Integer\nFor i = 1 To 2\nNext j\n"), "!C4:this Next names \"j\"");
    EXPECT(MAIN("Print 1: Print 2\n"), "!C2:\":\" does not separate statements");
    EXPECT("Sub Other()\nEnd Sub\n", "!C0:the program starts at Sub Main");
    EXPECT(MAIN("Exit For\n"), "!C2:Exit For is not inside a For loop");
    EXPECT(MAIN("Print Len(1)\n"), "!C2:argument 1 of Len: expected String, found Integer");
}

static int read_text(const char *path, char **text)
{
    size_t size;
    if (host_read_file(NULL, path, text, &size) != 0) return -1;
    (*text)[size] = '\0';
    return 0;
}

static void sample(const char *samples)
{
    char project[1024], expected_path[1024];
    snprintf(project, sizeof(project), "%s/hello/hello.sbp", samples);
    snprintf(expected_path, sizeof(expected_path), "%s/hello/expected-output.txt", samples);
    g_cases++;
    struct sxb_diagnostic diag;
    struct sxb_program *program = sxb_compile_project(project, &g_host, &diag);
    if (program == NULL) {
        g_failures++;
        fprintf(stderr, "sxbasic-test: hello.sbp: %s:%d:%d: %s\n", diag.file, diag.line, diag.column, diag.message);
        return;
    }
    g_out_length = 0;
    g_out[0] = '\0';
    struct sxb_vm *vm = sxb_vm_create(program, &g_host, "", samples);
    enum sxb_status status = sxb_vm_run(vm, 0);
    char *expected = NULL;
    if (status != SXB_DONE || read_text(expected_path, &expected) != 0 || strcmp(expected, g_out) != 0) {
        g_failures++;
        fprintf(stderr, "sxbasic-test: hello.sbp printed\n%s\n--- expected\n%s\n", g_out, expected ? expected : "?");
    }
    free(expected);
    sxb_vm_destroy(vm);
    sxb_program_free(program);

    /* The Windows samples are refused until forms exist, with the line that names one. */
    snprintf(project, sizeof(project), "%s/greeter/greeter.sbp", samples);
    g_cases++;
    program = sxb_compile_project(project, &g_host, &diag);
    if (program != NULL || strstr(diag.message, "forms are not supported yet") == NULL || diag.line != 8) {
        g_failures++;
        fprintf(stderr, "sxbasic-test: greeter.sbp: expected the forms refusal at line 8, got %d: %s\n", diag.line,
                diag.message);
    }
    sxb_program_free(program);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <samples directory>\n", argv[0]);
        return 2;
    }
    language();
    builtins();
    errors();
    sample(argv[1]);
    printf("sxbasic-test: %d of %d cases passed\n", g_cases - g_failures, g_cases);
    return g_failures ? 1 : 0;
}
