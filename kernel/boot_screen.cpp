#include "kernel/boot_screen.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/timer.hpp"
#include "shared/version.h"

namespace boot_screen {
namespace {

// Splash al estilo XP: fondo negro, el logo del SO centrado con el nombre
// debajo y una barra de bloques que se desplazan. Todo se dibuja a mano sobre
// el framebuffer lineal de Limine porque esto corre antes del compositor, del
// heap y de cualquier decodificador de imagenes.
constexpr uint32_t kBackground = 0x00000000U;
constexpr uint32_t kWordmark = 0x00f2f6f9U;
constexpr uint32_t kMuted = 0x006f8290U;
constexpr uint32_t kTroughFill = 0x00080d12U;
constexpr uint32_t kTroughEdge = 0x00263442U;
constexpr uint32_t kBlock = 0x002e9bd0U;
constexpr uint32_t kBlockHighlight = 0x007fd3f5U;

#include "kernel/console_font_unifont.inc"
#include "boot_logo.h"

// Pasos del ciclo de la barra. A 20 pasos por segundo, una vuelta completa
// dura poco mas de un segundo, como la de XP.
constexpr uint32_t kMarqueeSteps = 24;
constexpr uint64_t kStepNs = 50000000ull;

boot::FramebufferInfo g_framebuffer = {};
bool g_ready = false;
bool g_painted = false;
// Lo que sigue lo comparten el hilo de arranque y el handler del timer, que
// puede caer en cualquier instruccion del medio: `g_drawing` es la exclusion
// entre los dos (un solo core, alcanza con que el handler vea el flag) y
// `g_active` lo apaga cuando el splash deja de ser dueño de la pantalla.
volatile bool g_active = false;
volatile bool g_drawing = false;
volatile uint32_t g_phase = 0;

size_t text_length(const char* text) {
    size_t length = 0;
    while (text != nullptr && text[length] != '\0') {
        ++length;
    }
    return length;
}

uint64_t min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

void plot_pixel(uint64_t x, uint64_t y, uint32_t colour) {
    if (!g_ready || x >= g_framebuffer.width || y >= g_framebuffer.height) {
        return;
    }

    auto* pixels = static_cast<uint8_t*>(g_framebuffer.address);
    auto* row = reinterpret_cast<uint32_t*>(pixels + (y * g_framebuffer.pitch));
    row[x] = colour;
}

void fill_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t colour) {
    if (!g_ready || width == 0 || height == 0) {
        return;
    }

    const uint64_t end_x = min_u64(x + width, g_framebuffer.width);
    const uint64_t end_y = min_u64(y + height, g_framebuffer.height);
    for (uint64_t row = y; row < end_y; ++row) {
        for (uint64_t column = x; column < end_x; ++column) {
            plot_pixel(column, row, colour);
        }
    }
}

// Igual que fill_rect pero recortado a una ventana horizontal, y con la x en
// con signo: los bloques de la barra nacen y mueren fuera de la canaleta.
void fill_rect_clipped(int64_t x, uint64_t y, uint64_t width, uint64_t height,
                       uint64_t clip_x, uint64_t clip_width, uint32_t colour) {
    int64_t left = x;
    int64_t right = x + static_cast<int64_t>(width);
    const int64_t clip_left = static_cast<int64_t>(clip_x);
    const int64_t clip_right = clip_left + static_cast<int64_t>(clip_width);

    if (left < clip_left) {
        left = clip_left;
    }
    if (right > clip_right) {
        right = clip_right;
    }
    if (right <= left) {
        return;
    }
    fill_rect(static_cast<uint64_t>(left), y, static_cast<uint64_t>(right - left), height, colour);
}

// Rectangulo con las cuatro esquinas comidas en un pixel: alcanza para que la
// canaleta de la barra se lea redondeada, igual que la de XP, sin meter un
// rasterizador de arcos en el kernel.
void fill_round_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t colour) {
    if (width < 3 || height < 3) {
        fill_rect(x, y, width, height, colour);
        return;
    }
    fill_rect(x + 1, y, width - 2, 1, colour);
    fill_rect(x, y + 1, width, height - 2, colour);
    fill_rect(x + 1, y + height - 1, width - 2, 1, colour);
}

// Mezcla lineal entre el fondo y el color de trazo segun la cobertura (0..255)
// que trae la mascara del wordmark.
uint32_t blend(uint32_t background, uint32_t foreground, uint32_t coverage) {
    const uint32_t inverse = 255u - coverage;
    const uint32_t red = (((foreground >> 16) & 0xFFu) * coverage + ((background >> 16) & 0xFFu) * inverse) / 255u;
    const uint32_t green = (((foreground >> 8) & 0xFFu) * coverage + ((background >> 8) & 0xFFu) * inverse) / 255u;
    const uint32_t blue = ((foreground & 0xFFu) * coverage + (background & 0xFFu) * inverse) / 255u;
    return (red << 16) | (green << 8) | blue;
}

void draw_glyph(uint64_t x, uint64_t y, char character, uint32_t scale, uint32_t colour) {
    const auto* glyph = sx_console_glyph(static_cast<unsigned char>(character));
    for (uint64_t row = 0; row < SX_CONSOLE_GLYPH_H; ++row) {
        for (uint64_t column = 0; column < SX_CONSOLE_GLYPH_W; ++column) {
            if ((glyph[row] & (1u << (SX_CONSOLE_GLYPH_W - 1u - column))) == 0) {
                continue;
            }
            fill_rect(x + (column * scale), y + (row * scale), scale, scale, colour);
        }
    }
}

void draw_text(uint64_t x, uint64_t y, const char* text, uint32_t scale, uint32_t colour) {
    if (text == nullptr || scale == 0) {
        return;
    }

    for (size_t index = 0; text[index] != '\0'; ++index) {
        draw_glyph(x + (index * SX_CONSOLE_GLYPH_W * scale), y, text[index], scale, colour);
    }
}

uint64_t text_width(const char* text, uint32_t scale) {
    return text_length(text) * SX_CONSOLE_GLYPH_W * scale;
}

void draw_centered_text(uint64_t y, const char* text, uint32_t scale, uint32_t colour) {
    const uint64_t width = text_width(text, scale);
    const uint64_t x = g_framebuffer.width > width ? (g_framebuffer.width - width) / 2 : 0;
    draw_text(x, y, text, scale, colour);
}

// El logo viaja horneado como indices de paleta (tools/gen_boot_logo.py), ya
// compuesto sobre el fondo del splash: aca solo hay lookup y replicacion por
// un factor entero.
void draw_logo(uint64_t x, uint64_t y, uint32_t scale) {
    for (uint64_t row = 0; row < SX_BOOT_LOGO_H; ++row) {
        for (uint64_t column = 0; column < SX_BOOT_LOGO_W; ++column) {
            const uint32_t colour = k_boot_logo_palette[k_boot_logo_pixels[(row * SX_BOOT_LOGO_W) + column]];
            fill_rect(x + (column * scale), y + (row * scale), scale, scale, colour);
        }
    }
}

// El nombre del SO es una mascara de cobertura de 8 bits rendereada con la Noto
// Sans del escritorio, no la fuente de consola: un wordmark de splash necesita
// bordes suavizados.
void draw_wordmark(uint64_t x, uint64_t y, uint32_t scale, uint32_t colour) {
    for (uint64_t row = 0; row < SX_BOOT_WORDMARK_H; ++row) {
        for (uint64_t column = 0; column < SX_BOOT_WORDMARK_W; ++column) {
            const uint32_t coverage = k_boot_wordmark_alpha[(row * SX_BOOT_WORDMARK_W) + column];
            if (coverage == 0) {
                continue;
            }
            fill_rect(x + (column * scale), y + (row * scale), scale, scale,
                      blend(kBackground, colour, coverage));
        }
    }
}

struct Layout {
    uint32_t scale;
    bool with_logo;
    uint64_t logo_x;
    uint64_t logo_y;
    uint64_t wordmark_x;
    uint64_t wordmark_y;
    uint64_t bar_x;
    uint64_t bar_y;
    uint64_t bar_width;
    uint64_t bar_height;
    uint64_t status_y;
};

Layout compute_layout() {
    Layout layout = {};
    // Un solo factor entero para todo el splash: duplicar recien tiene sentido
    // en paneles muy altos, donde el arte horneado quedaria diminuto.
    layout.scale = g_framebuffer.height >= 1440 ? 2u : 1u;

    const uint64_t scale = layout.scale;
    const uint64_t logo_height = SX_BOOT_LOGO_H * scale;
    const uint64_t wordmark_height = SX_BOOT_WORDMARK_H * scale;
    const uint64_t logo_gap = 24 * scale;
    const uint64_t bar_gap = 62 * scale;

    layout.bar_width = min_u64(240 * scale, g_framebuffer.width - (48 * scale));
    layout.bar_height = 16 * scale;

    // En framebuffers muy bajos no entra el bloque completo: se cae al
    // wordmark solo antes que recortar el logo.
    layout.with_logo = g_framebuffer.height >= (logo_height + wordmark_height + (150 * scale));

    // La composicion entera (logo, nombre y barra) se centra como un bloque:
    // la barra cuelga del nombre en vez de anclarse a un porcentaje del alto,
    // que es lo que dejaba un hueco muerto en pantallas anchas.
    const uint64_t block_height = (layout.with_logo ? (logo_height + logo_gap) : 0) +
        wordmark_height + bar_gap + layout.bar_height;
    const uint64_t block_top = (g_framebuffer.height * 46u / 100u) - (block_height / 2);

    layout.logo_x = (g_framebuffer.width - (SX_BOOT_LOGO_W * scale)) / 2;
    layout.logo_y = block_top;
    layout.wordmark_x = (g_framebuffer.width - (SX_BOOT_WORDMARK_W * scale)) / 2;
    layout.wordmark_y = layout.with_logo ? (block_top + logo_height + logo_gap) : block_top;

    layout.bar_x = (g_framebuffer.width - layout.bar_width) / 2;
    layout.bar_y = layout.wordmark_y + wordmark_height + bar_gap;
    layout.status_y = layout.bar_y + layout.bar_height + (18 * scale);
    return layout;
}

// La canaleta no cambia entre pasos: se pinta una sola vez y despues solo se
// repinta la franja por donde corren los bloques.
void draw_trough(const Layout& layout) {
    const uint64_t scale = layout.scale;
    fill_round_rect(layout.bar_x, layout.bar_y, layout.bar_width, layout.bar_height, kTroughEdge);
    fill_round_rect(layout.bar_x + (2 * scale), layout.bar_y + (2 * scale),
                    layout.bar_width - (4 * scale), layout.bar_height - (4 * scale), kTroughFill);
}

// Los tres bloques de XP: entran por la izquierda, cruzan la canaleta y salen
// por la derecha. `phase` es la posicion dentro del ciclo, no el progreso: la
// barra dice "sigo vivo", y quien dice en que anda el arranque es el texto.
void draw_marquee(const Layout& layout, uint32_t phase) {
    const uint64_t scale = layout.scale;
    const uint64_t inner_x = layout.bar_x + (2 * scale);
    const uint64_t inner_width = layout.bar_width - (4 * scale);
    const uint64_t band_y = layout.bar_y + (3 * scale);
    const uint64_t band_height = layout.bar_height - (6 * scale);

    fill_rect(inner_x, band_y, inner_width, band_height, kTroughFill);

    const uint64_t block_width = 12 * scale;
    const uint64_t block_gap = 6 * scale;
    if (inner_width <= block_width) {
        return;
    }

    // El grupo arranca pegado al borde izquierdo y se va recortando contra el
    // derecho. Que la fase 0 lo muestre entero y adentro importa: mientras el
    // reloj todavia no corre, esa es la imagen fija que se ve.
    const int64_t offset = static_cast<int64_t>((inner_width * phase) / kMarqueeSteps);

    for (int64_t index = 0; index < 3; ++index) {
        const int64_t x = static_cast<int64_t>(inner_x) + offset +
            (index * static_cast<int64_t>(block_width + block_gap));
        fill_rect_clipped(x, band_y, block_width, band_height, inner_x, inner_width, kBlock);
        fill_rect_clipped(x, band_y, block_width, scale, inner_x, inner_width, kBlockHighlight);
    }
}

// Posicion dentro del ciclo de la barra: ~20 pasos por segundo. El reloj es el
// TSC y no el contador de ticks porque buena parte del arranque corre con las
// interrupciones deshabilitadas, y ahi los ticks no avanzan; el TSC si. Los
// ticks quedan de respaldo para antes de que el TSC este calibrado.
uint32_t marquee_phase() {
    const uint64_t elapsed_ns = timer::monotonic_ns();
    if (elapsed_ns != 0) {
        return static_cast<uint32_t>((elapsed_ns / kStepNs) % kMarqueeSteps);
    }

    const uint32_t frequency = timer::frequency_hz();
    const uint64_t ticks_per_step = frequency >= 20u ? (frequency / 20u) : 1u;
    return static_cast<uint32_t>((timer::ticks() / ticks_per_step) % kMarqueeSteps);
}

} // namespace

void initialize(const boot::FramebufferInfo& framebuffer) {
    g_framebuffer = framebuffer;
    g_ready = framebuffer.available &&
        framebuffer.address != nullptr &&
        framebuffer.width >= 320 &&
        framebuffer.height >= 200 &&
        framebuffer.pitch >= framebuffer.width * sizeof(uint32_t) &&
        framebuffer.bpp == 32;
    g_painted = false;
    g_active = g_ready;
    g_drawing = false;
    g_phase = 0;
}

bool ready() {
    return g_ready;
}

void show(const char* status) {
    if (!g_ready || !g_active) {
        return;
    }

    g_drawing = true;
    const Layout layout = compute_layout();

    // El logo y el wordmark se pintan una sola vez: repintarlos en cada paso
    // del arranque los haria parpadear. De ahi en mas solo se refrescan la
    // barra y la linea de estado.
    if (!g_painted) {
        fill_rect(0, 0, g_framebuffer.width, g_framebuffer.height, kBackground);
        if (layout.with_logo) {
            draw_logo(layout.logo_x, layout.logo_y, layout.scale);
        }
        draw_wordmark(layout.wordmark_x, layout.wordmark_y, layout.scale, kWordmark);

        const uint64_t version_width = text_width(SAVANXP_VERSION_STRING, layout.scale);
        draw_text(g_framebuffer.width - version_width - (16 * layout.scale),
                  g_framebuffer.height - (SX_CONSOLE_GLYPH_H * layout.scale) - (12 * layout.scale),
                  SAVANXP_VERSION_STRING, layout.scale, kMuted);
        draw_trough(layout);
        g_painted = true;
    }

    g_phase = marquee_phase();
    draw_marquee(layout, g_phase);

    fill_rect(0, layout.status_y, g_framebuffer.width, SX_CONSOLE_GLYPH_H * layout.scale, kBackground);
    draw_centered_text(layout.status_y, status != nullptr ? status : "Starting", layout.scale, kMuted);
    g_drawing = false;
}

void animate() {
    if (!g_ready || !g_active || !g_painted || g_drawing) {
        return;
    }

    const uint32_t phase = marquee_phase();
    if (phase == g_phase) {
        return;
    }

    g_drawing = true;
    g_phase = phase;
    draw_marquee(compute_layout(), phase);
    g_drawing = false;
}

void finish() {
    g_active = false;
}

} // namespace boot_screen
