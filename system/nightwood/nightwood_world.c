/* The forest of Nightwood: generated from a seed, textured by hand-written
 * noise, drawn with SxGL. Nothing here talks to the system: like SxGL itself,
 * this unit is plain computation over memory, so the SUN side owns every
 * system call. */

#include "nightwood.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NW_DEGREES (180.0f / NW_PI)
#define NW_RADIANS (NW_PI / 180.0f)
#define NW_FENCE (NW_WORLD_HALF - 1.5f)
#define NW_VIEW_DISTANCE 30.0f
#define NW_FOG_START 3.0f
#define NW_FOG_END 26.0f

static const GLfloat g_fog_colour[4] = {0.012f, 0.016f, 0.028f, 1.0f};

/* ------------------------------------------------------------------------
 * Random numbers and noise
 * ------------------------------------------------------------------------ */

uint32_t nw_random(struct nw_rng* rng)
{
    /* xorshift64*: small, fast, and good enough to scatter trees. */
    uint64_t x = rng->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng->state = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

float nw_random_unit(struct nw_rng* rng)
{
    return (float)(nw_random(rng) >> 8) / 16777216.0f;
}

static float nw_hash(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xffffu) / 65535.0f;
}

static float nw_smooth(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

/* Value noise that tiles with `period` lattice cells, so textures repeat
 * without a seam. */
static float nw_noise(float x, float y, int period, uint32_t seed)
{
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    float fx = nw_smooth(x - (float)x0);
    float fy = nw_smooth(y - (float)y0);
    int ax = ((x0 % period) + period) % period;
    int ay = ((y0 % period) + period) % period;
    int bx = (ax + 1) % period;
    int by = (ay + 1) % period;
    float top = nw_hash(ax, ay, seed) + (nw_hash(bx, ay, seed) - nw_hash(ax, ay, seed)) * fx;
    float bottom = nw_hash(ax, by, seed) + (nw_hash(bx, by, seed) - nw_hash(ax, by, seed)) * fx;
    return top + (bottom - top) * fy;
}

/* Fractal noise over a texture of `size` texels, tiling. */
static float nw_fbm(int x, int y, int size, int base_cells, uint32_t seed)
{
    float total = 0.0f;
    float amplitude = 0.5f;
    float weight = 0.0f;
    int cells = base_cells;
    int octave;

    for (octave = 0; octave < 4; ++octave) {
        float scale = (float)cells / (float)size;
        total += amplitude * nw_noise((float)x * scale, (float)y * scale, cells, seed + (uint32_t)octave * 31u);
        weight += amplitude;
        amplitude *= 0.5f;
        cells *= 2;
    }
    return total / weight;
}

/* ------------------------------------------------------------------------
 * Textures
 * ------------------------------------------------------------------------ */

#define NW_TEXTURE_SIZE 64

static unsigned char nw_byte(float value)
{
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 255.0f) {
        return 255;
    }
    return (unsigned char)value;
}

static void nw_put(unsigned char* image, int size, int x, int y, float r, float g, float b, float a)
{
    unsigned char* texel = image + ((size_t)y * (size_t)size + (size_t)x) * 4u;
    texel[0] = nw_byte(r);
    texel[1] = nw_byte(g);
    texel[2] = nw_byte(b);
    texel[3] = nw_byte(a);
}

static void nw_texture_ground(unsigned char* image)
{
    int x;
    int y;

    for (y = 0; y < NW_TEXTURE_SIZE; ++y) {
        for (x = 0; x < NW_TEXTURE_SIZE; ++x) {
            float patch = nw_fbm(x, y, NW_TEXTURE_SIZE, 4, 11u);
            float detail = nw_fbm(x, y, NW_TEXTURE_SIZE, 16, 12u);
            float grass = patch > 0.48f ? 1.0f : 0.0f;
            float shade = 0.65f + 0.6f * detail;
            float r = (grass ? 62.0f : 104.0f) * shade;
            float g = (grass ? 92.0f : 80.0f) * shade;
            float b = (grass ? 46.0f : 54.0f) * shade;
            /* Fallen needles and leaves. */
            if (nw_hash(x, y, 13u) > 0.94f) {
                r = 96.0f * shade;
                g = 64.0f * shade;
                b = 30.0f * shade;
            }
            nw_put(image, NW_TEXTURE_SIZE, x, y, r, g, b, 255.0f);
        }
    }
}

static void nw_texture_bark(unsigned char* image)
{
    int x;
    int y;

    for (y = 0; y < NW_TEXTURE_SIZE; ++y) {
        for (x = 0; x < NW_TEXTURE_SIZE; ++x) {
            /* Stretched vertically: furrows running up the trunk. */
            float furrow = nw_noise((float)x * 0.25f, (float)y * 0.03125f, 16, 21u);
            float grain = nw_fbm(x, y, NW_TEXTURE_SIZE, 16, 22u);
            float shade = 0.45f + 0.55f * grain;
            if (furrow < 0.35f) {
                shade *= 0.45f;
            }
            nw_put(image, NW_TEXTURE_SIZE, x, y, 130.0f * shade, 110.0f * shade, 92.0f * shade, 255.0f);
        }
    }
}

/* A pine's silhouette, with transparent texels around it for the alpha test.
 * Row 0 is t = 0, the bottom of the quad. */
static void nw_texture_foliage(unsigned char* image)
{
    int x;
    int y;

    for (y = 0; y < NW_TEXTURE_SIZE; ++y) {
        float height = (float)y / (float)NW_TEXTURE_SIZE;
        /* Tiers: the width falls with height and saws back out at each one. */
        float tier = fmodf(height * 5.0f, 1.0f);
        float width = (1.0f - height) * (0.30f + 0.20f * (1.0f - tier));
        for (x = 0; x < NW_TEXTURE_SIZE; ++x) {
            float offset = fabsf(((float)x + 0.5f) / (float)NW_TEXTURE_SIZE - 0.5f);
            float jag = (nw_hash(x, y, 31u) - 0.5f) * 0.06f;
            float shade = 0.5f + 0.5f * nw_fbm(x, y, NW_TEXTURE_SIZE, 16, 32u);
            if (y < 3 || offset > width + jag) {
                nw_put(image, NW_TEXTURE_SIZE, x, y, 0.0f, 0.0f, 0.0f, 0.0f);
            } else {
                nw_put(image, NW_TEXTURE_SIZE, x, y, 30.0f * shade, 54.0f * shade, 36.0f * shade, 255.0f);
            }
        }
    }
}

static void nw_texture_stone(unsigned char* image)
{
    int x;
    int y;

    for (y = 0; y < NW_TEXTURE_SIZE; ++y) {
        for (x = 0; x < NW_TEXTURE_SIZE; ++x) {
            float value = nw_fbm(x, y, NW_TEXTURE_SIZE, 8, 41u);
            float moss = nw_fbm(x, y, NW_TEXTURE_SIZE, 4, 42u);
            float shade = 55.0f + 120.0f * value;
            if (moss > 0.62f) {
                nw_put(image, NW_TEXTURE_SIZE, x, y, shade * 0.55f, shade * 0.75f, shade * 0.45f, 255.0f);
            } else {
                nw_put(image, NW_TEXTURE_SIZE, x, y, shade, shade, shade * 0.95f, 255.0f);
            }
        }
    }
}

static void nw_texture_wood(unsigned char* image)
{
    int x;
    int y;

    for (y = 0; y < NW_TEXTURE_SIZE; ++y) {
        for (x = 0; x < NW_TEXTURE_SIZE; ++x) {
            float grain = nw_noise((float)x * 0.0625f, (float)y * 0.5f, 4, 51u);
            float shade = 0.55f + 0.45f * grain;
            /* Planks eight texels tall, with a dark gap between them. */
            if ((y & 7) == 0) {
                shade *= 0.3f;
            }
            nw_put(image, NW_TEXTURE_SIZE, x, y, 110.0f * shade, 84.0f * shade, 58.0f * shade, 255.0f);
        }
    }
}

static void nw_texture_metal(unsigned char* image)
{
    int x;
    int y;

    for (y = 0; y < NW_TEXTURE_SIZE; ++y) {
        for (x = 0; x < NW_TEXTURE_SIZE; ++x) {
            float rust = nw_fbm(x, y, NW_TEXTURE_SIZE, 8, 61u);
            float shade = 0.6f + 0.4f * nw_fbm(x, y, NW_TEXTURE_SIZE, 16, 62u);
            if (rust > 0.52f) {
                nw_put(image, NW_TEXTURE_SIZE, x, y, 120.0f * shade, 62.0f * shade, 30.0f * shade, 255.0f);
            } else {
                nw_put(image, NW_TEXTURE_SIZE, x, y, 92.0f * shade, 100.0f * shade, 104.0f * shade, 255.0f);
            }
        }
    }
}

/* A Polaroid: white frame, and inside it a dark photograph of tree trunks and,
 * between them, a tall pale figure. */
static void nw_texture_photo(unsigned char* image)
{
    int x;
    int y;

    for (y = 0; y < NW_TEXTURE_SIZE; ++y) {
        for (x = 0; x < NW_TEXTURE_SIZE; ++x) {
            int border = x < 4 || x >= NW_TEXTURE_SIZE - 4 || y >= NW_TEXTURE_SIZE - 4 || y < 14;
            float r;
            float g;
            float b;
            if (border) {
                float paper = 215.0f + 25.0f * nw_hash(x, y, 71u);
                r = paper;
                g = paper * 0.97f;
                b = paper * 0.88f;
            } else {
                float sky = 20.0f + 40.0f * ((float)(y - 14) / 46.0f);
                int trunk = ((x + 3) % 13) < 2 || ((x + 9) % 19) < 3;
                int figure = (x >= 30 && x <= 33 && y >= 22 && y <= 50) || (x >= 29 && x <= 34 && y >= 44 && y <= 48);
                r = sky;
                g = sky * 1.05f;
                b = sky * 1.2f;
                if (trunk) {
                    r = g = b = 8.0f;
                }
                if (figure) {
                    r = 170.0f;
                    g = 165.0f;
                    b = 155.0f;
                }
                r += 30.0f * (nw_hash(x, y, 72u) - 0.5f);
                g += 30.0f * (nw_hash(x, y, 72u) - 0.5f);
                b += 30.0f * (nw_hash(x, y, 72u) - 0.5f);
            }
            nw_put(image, NW_TEXTURE_SIZE, x, y, r, g, b, 255.0f);
        }
    }
}

static int nw_upload(GLuint* name, void (*paint)(unsigned char*))
{
    unsigned char* image = (unsigned char*)malloc(NW_TEXTURE_SIZE * NW_TEXTURE_SIZE * 4);

    if (image == 0) {
        return -1;
    }
    paint(image);
    glGenTextures(1, name);
    glBindTexture(GL_TEXTURE_2D, *name);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, NW_TEXTURE_SIZE, NW_TEXTURE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, image);
    /* Near textures are magnified: bilinear smooths them. Far ones would
     * shimmer with it and without mipmaps, so they stay nearest. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    free(image);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

int nw_world_upload_textures(struct nw_world* world)
{
    if (nw_upload(&world->texture_ground, nw_texture_ground) != 0 ||
        nw_upload(&world->texture_bark, nw_texture_bark) != 0 ||
        nw_upload(&world->texture_foliage, nw_texture_foliage) != 0 ||
        nw_upload(&world->texture_stone, nw_texture_stone) != 0 ||
        nw_upload(&world->texture_wood, nw_texture_wood) != 0 ||
        nw_upload(&world->texture_metal, nw_texture_metal) != 0 ||
        nw_upload(&world->texture_photo, nw_texture_photo) != 0) {
        return -1;
    }
    /* The photographs are clamped: their frame must not wrap. */
    glBindTexture(GL_TEXTURE_2D, world->texture_photo);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, world->texture_foliage);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

/* ------------------------------------------------------------------------
 * The ground
 * ------------------------------------------------------------------------ */

float nw_ground_height(float x, float z)
{
    return 0.45f * sinf(x * 0.13f) * cosf(z * 0.11f) + 0.3f * sinf((x + z) * 0.07f) + 0.15f * cosf(x * 0.31f - z * 0.23f);
}

static void nw_ground_normal(float x, float z, float out[3])
{
    const float step = 0.25f;
    float dx = nw_ground_height(x + step, z) - nw_ground_height(x - step, z);
    float dz = nw_ground_height(x, z + step) - nw_ground_height(x, z - step);
    float nx = -dx;
    float ny = 2.0f * step;
    float nz = -dz;
    float length = sqrtf(nx * nx + ny * ny + nz * nz);
    out[0] = nx / length;
    out[1] = ny / length;
    out[2] = nz / length;
}

/* ------------------------------------------------------------------------
 * Generation
 * ------------------------------------------------------------------------ */

/* Collision circles of each kind, in its own frame: x along the landmark,
 * radius; up to three, a zero radius ends the list. */
static const float g_landmark_circles[NW_LANDMARK_KIND_COUNT][3][2] = {
    {{0.0f, 2.2f}, {0.0f, 0.0f}, {0.0f, 0.0f}}, /* rock */
    {{0.0f, 2.4f}, {0.0f, 0.0f}, {0.0f, 0.0f}}, /* shed */
    {{0.0f, 1.4f}, {0.0f, 0.0f}, {0.0f, 0.0f}}, /* tank */
    {{-1.4f, 1.05f}, {0.0f, 1.05f}, {1.4f, 1.05f}}, /* car */
    {{-2.2f, 1.1f}, {0.0f, 1.1f}, {2.2f, 1.1f}}, /* wall */
    {{0.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}}, /* oak */
    {{0.0f, 0.4f}, {0.0f, 0.0f}, {0.0f, 0.0f}}, /* sign */
    {{-2.0f, 0.75f}, {2.0f, 0.75f}, {0.0f, 0.0f}}, /* arch: the opening is walkable */
};

static const float g_landmark_radius[NW_LANDMARK_KIND_COUNT] = {
    2.2f, /* rock */
    2.6f, /* shed */
    2.0f, /* tank */
    2.4f, /* car */
    3.2f, /* wall */
    1.3f, /* oak */
    0.6f, /* sign */
    2.8f, /* arch */
};

/* Where the photograph goes on each kind, in the landmark's own frame: on a
 * face looking down local +z, at this height above the ground, this far in
 * front of the centre and this far to the side. */
static const float g_photo_offset[NW_LANDMARK_KIND_COUNT][3] = {
    {1.3f, 1.62f, 0.0f}, /* rock: height, distance, lateral */
    {1.4f, 1.62f, 0.0f}, /* shed */
    {1.5f, 1.23f, 0.0f}, /* tank: an octagon turned to show a flat face */
    {0.55f, 1.02f, 0.0f}, /* car: on the door, below the cabin */
    {1.4f, 0.32f, 0.0f}, /* wall */
    {1.6f, 0.86f, 0.0f}, /* oak */
    {1.45f, 0.06f, 0.0f}, /* sign: on the board */
    {1.5f, 0.47f, 2.0f}, /* arch: on the right pillar, not in the opening */
};

static int nw_too_close(const struct nw_world* world, float x, float z, float distance, int trees, int landmarks)
{
    int index;

    for (index = 0; index < trees; ++index) {
        float dx = world->trees[index].x - x;
        float dz = world->trees[index].z - z;
        if (dx * dx + dz * dz < distance * distance) {
            return 1;
        }
    }
    for (index = 0; index < landmarks; ++index) {
        float dx = world->landmarks[index].x - x;
        float dz = world->landmarks[index].z - z;
        float reach = distance + world->landmarks[index].radius;
        if (dx * dx + dz * dz < reach * reach) {
            return 1;
        }
    }
    return 0;
}

void nw_world_generate(struct nw_world* world, uint64_t seed)
{
    struct nw_rng rng;
    int index;
    int attempts;

    /* The textures outlive a new night: keep their names. */
    {
        struct nw_world kept = *world;
        memset(world, 0, sizeof(*world));
        world->texture_ground = kept.texture_ground;
        world->texture_bark = kept.texture_bark;
        world->texture_foliage = kept.texture_foliage;
        world->texture_stone = kept.texture_stone;
        world->texture_wood = kept.texture_wood;
        world->texture_metal = kept.texture_metal;
        world->texture_photo = kept.texture_photo;
    }
    rng.state = seed != 0 ? seed : 0x9E3779B97F4A7C15ULL;

    /* Eight landmarks, one per sector of a ring around the clearing where the
     * player starts, each kind once in a shuffled order: walking the ring
     * finds them all, and none can sit on the spawn. */
    {
        int order[NW_LANDMARK_KIND_COUNT];
        for (index = 0; index < NW_LANDMARK_KIND_COUNT; ++index) {
            order[index] = index;
        }
        for (index = NW_LANDMARK_KIND_COUNT - 1; index > 0; --index) {
            int other = (int)(nw_random(&rng) % (uint32_t)(index + 1));
            int swap = order[index];
            order[index] = order[other];
            order[other] = swap;
        }
        for (index = 0; index < NW_PHOTO_COUNT; ++index) {
            struct nw_landmark* landmark = &world->landmarks[index];
            float angle = ((float)index + 0.2f + 0.6f * nw_random_unit(&rng)) * (2.0f * NW_PI / (float)NW_PHOTO_COUNT);
            float distance = 20.0f + 28.0f * nw_random_unit(&rng);
            float yaw_radians;
            landmark->kind = order[index];
            landmark->x = sinf(angle) * distance;
            landmark->z = cosf(angle) * distance;
            /* Face roughly back towards the middle, so the photograph is found
             * by someone walking in from the clearing. */
            landmark->yaw = atan2f(-landmark->x, -landmark->z) * NW_DEGREES + (nw_random_unit(&rng) - 0.5f) * 90.0f;
            landmark->radius = g_landmark_radius[landmark->kind];
            yaw_radians = landmark->yaw * NW_RADIANS;
            landmark->photo_yaw = landmark->yaw;
            /* Local +z is (sin, cos) and local +x is (cos, -sin) once turned
             * by the yaw. */
            landmark->photo_x = landmark->x + sinf(yaw_radians) * g_photo_offset[landmark->kind][1] +
                cosf(yaw_radians) * g_photo_offset[landmark->kind][2];
            landmark->photo_z = landmark->z + cosf(yaw_radians) * g_photo_offset[landmark->kind][1] -
                sinf(yaw_radians) * g_photo_offset[landmark->kind][2];
            landmark->photo_y = nw_ground_height(landmark->x, landmark->z) + g_photo_offset[landmark->kind][0];
            {
                int circle;
                landmark->circle_count = 0;
                for (circle = 0; circle < 3; ++circle) {
                    float along = g_landmark_circles[landmark->kind][circle][0];
                    float reach = g_landmark_circles[landmark->kind][circle][1];
                    float* out;
                    if (reach <= 0.0f) {
                        break;
                    }
                    out = landmark->circles[landmark->circle_count++];
                    out[0] = landmark->x + cosf(yaw_radians) * along;
                    out[1] = landmark->z - sinf(yaw_radians) * along;
                    out[2] = reach;
                }
            }
        }
    }

    /* Trees: dart throwing with a minimum spacing, kept off the spawn
     * clearing and off the landmarks. */
    for (attempts = 0; attempts < 6000 && world->tree_count < NW_MAX_TREES; ++attempts) {
        float x = (nw_random_unit(&rng) * 2.0f - 1.0f) * (NW_FENCE - 1.5f);
        float z = (nw_random_unit(&rng) * 2.0f - 1.0f) * (NW_FENCE - 1.5f);
        struct nw_tree* tree;
        if (x * x + z * z < 36.0f) {
            continue;
        }
        if (nw_too_close(world, x, z, 3.4f, world->tree_count, NW_PHOTO_COUNT)) {
            continue;
        }
        tree = &world->trees[world->tree_count++];
        tree->x = x;
        tree->z = z;
        tree->radius = 0.18f + 0.16f * nw_random_unit(&rng);
        tree->height = 7.0f + 5.0f * nw_random_unit(&rng);
        tree->foliage_width = 2.6f + 1.4f * nw_random_unit(&rng);
        tree->dead = nw_random_unit(&rng) < 0.22f;
    }
}

/* ------------------------------------------------------------------------
 * Collision
 * ------------------------------------------------------------------------ */

static void nw_push_out(float* x, float* z, float cx, float cz, float reach)
{
    float dx = *x - cx;
    float dz = *z - cz;
    float distance_squared = dx * dx + dz * dz;
    float distance;

    if (distance_squared >= reach * reach) {
        return;
    }
    distance = sqrtf(distance_squared);
    if (distance < 1e-4f) {
        *x = cx + reach;
        return;
    }
    *x = cx + dx / distance * reach;
    *z = cz + dz / distance * reach;
}

void nw_world_collide(const struct nw_world* world, float* x, float* z, float radius)
{
    int index;

    for (index = 0; index < world->tree_count; ++index) {
        const struct nw_tree* tree = &world->trees[index];
        nw_push_out(x, z, tree->x, tree->z, tree->radius + radius);
    }
    for (index = 0; index < NW_PHOTO_COUNT; ++index) {
        const struct nw_landmark* landmark = &world->landmarks[index];
        int circle;
        for (circle = 0; circle < landmark->circle_count; ++circle) {
            const float* c = landmark->circles[circle];
            nw_push_out(x, z, c[0], c[1], c[2] + radius);
        }
    }
    if (*x < -NW_FENCE + radius) {
        *x = -NW_FENCE + radius;
    }
    if (*x > NW_FENCE - radius) {
        *x = NW_FENCE - radius;
    }
    if (*z < -NW_FENCE + radius) {
        *z = -NW_FENCE + radius;
    }
    if (*z > NW_FENCE - radius) {
        *z = NW_FENCE - radius;
    }
}

int nw_world_blocked(const struct nw_world* world, float x, float z, float radius)
{
    float moved_x = x;
    float moved_z = z;

    nw_world_collide(world, &moved_x, &moved_z, radius);
    return moved_x != x || moved_z != z;
}

int nw_world_occluded(const struct nw_world* world, float ax, float az, float bx, float bz)
{
    float dx = bx - ax;
    float dz = bz - az;
    float length_squared = dx * dx + dz * dz;
    int index;

    if (length_squared < 1e-6f) {
        return 0;
    }
    for (index = 0; index < world->tree_count; ++index) {
        const struct nw_tree* tree = &world->trees[index];
        float t = ((tree->x - ax) * dx + (tree->z - az) * dz) / length_squared;
        float px;
        float pz;
        if (t <= 0.0f || t >= 1.0f) {
            continue;
        }
        px = ax + dx * t - tree->x;
        pz = az + dz * t - tree->z;
        /* A trunk only hides what is behind its middle: thin trees do not
         * swallow a figure whole. */
        if (px * px + pz * pz < tree->radius * tree->radius * 0.5f) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Drawing helpers
 * ------------------------------------------------------------------------ */

/* One face of a box: a grid over origin + u * a + v * b for a, b in [0, 1],
 * cut into cells no larger than a metre. SxGL lights per vertex, so a large
 * face with its four corners outside the flashlight's cone would stay dark
 * even with the beam on its middle; the grid gives the beam vertices to land on. */
static void nw_face(const float origin[3], const float u[3], const float v[3], const float normal[3], float u_length,
    float v_length, float scale)
{
    int columns = (int)ceilf(u_length);
    int rows = (int)ceilf(v_length);
    int row;
    int column;

    if (columns < 1) {
        columns = 1;
    }
    if (rows < 1) {
        rows = 1;
    }
    glNormal3fv(normal);
    for (row = 0; row < rows; ++row) {
        float b0 = (float)row / (float)rows;
        float b1 = (float)(row + 1) / (float)rows;
        glBegin(GL_QUAD_STRIP);
        for (column = 0; column <= columns; ++column) {
            float a = (float)column / (float)columns;
            glTexCoord2f(a * u_length / scale, b1 * v_length / scale);
            glVertex3f(origin[0] + u[0] * a + v[0] * b1, origin[1] + u[1] * a + v[1] * b1, origin[2] + u[2] * a + v[2] * b1);
            glTexCoord2f(a * u_length / scale, b0 * v_length / scale);
            glVertex3f(origin[0] + u[0] * a + v[0] * b0, origin[1] + u[1] * a + v[1] * b0, origin[2] + u[2] * a + v[2] * b0);
        }
        glEnd();
    }
}

/* An axis-aligned box in the current frame: from y = 0 up to `height`,
 * `width` along x and `depth` along z, centred on the origin. Five faces (the
 * bottom is never seen), texture coordinates in metres over `scale`. */
static void nw_box(float width, float height, float depth, float scale)
{
    float hx = width * 0.5f;
    float hz = depth * 0.5f;
    const float up[3] = {0.0f, height, 0.0f};

    {
        const float origin[3] = {-hx, 0.0f, hz};
        const float u[3] = {width, 0.0f, 0.0f};
        const float n[3] = {0.0f, 0.0f, 1.0f};
        nw_face(origin, u, up, n, width, height, scale);
    }
    {
        const float origin[3] = {hx, 0.0f, -hz};
        const float u[3] = {-width, 0.0f, 0.0f};
        const float n[3] = {0.0f, 0.0f, -1.0f};
        nw_face(origin, u, up, n, width, height, scale);
    }
    {
        const float origin[3] = {hx, 0.0f, hz};
        const float u[3] = {0.0f, 0.0f, -depth};
        const float n[3] = {1.0f, 0.0f, 0.0f};
        nw_face(origin, u, up, n, depth, height, scale);
    }
    {
        const float origin[3] = {-hx, 0.0f, -hz};
        const float u[3] = {0.0f, 0.0f, depth};
        const float n[3] = {-1.0f, 0.0f, 0.0f};
        nw_face(origin, u, up, n, depth, height, scale);
    }
    {
        const float origin[3] = {-hx, height, hz};
        const float u[3] = {width, 0.0f, 0.0f};
        const float v[3] = {0.0f, 0.0f, -depth};
        const float n[3] = {0.0f, 1.0f, 0.0f};
        nw_face(origin, u, v, n, width, depth, scale);
    }
}

/* A vertical prism of `sides` around the y axis, from y0 to y1, in `rings`
 * bands so the flashlight can shade it along its length. */
static void nw_prism(int sides, float radius, float y0, float y1, int rings, float scale)
{
    int side;
    int ring;

    for (ring = 0; ring < rings; ++ring) {
        float bottom = y0 + (y1 - y0) * (float)ring / (float)rings;
        float top = y0 + (y1 - y0) * (float)(ring + 1) / (float)rings;
        glBegin(GL_QUAD_STRIP);
        for (side = 0; side <= sides; ++side) {
            float angle = (float)side * 2.0f * NW_PI / (float)sides;
            float c = cosf(angle);
            float s = sinf(angle);
            float u = (float)side / (float)sides * (2.0f * NW_PI * radius / scale) * 2.0f;
            glNormal3f(s, 0.0f, c);
            glTexCoord2f(u, top / scale);
            glVertex3f(s * radius, top, c * radius);
            glTexCoord2f(u, bottom / scale);
            glVertex3f(s * radius, bottom, c * radius);
        }
        glEnd();
    }
}

static void nw_bind(GLuint texture)
{
    glBindTexture(GL_TEXTURE_2D, texture);
}

/* ------------------------------------------------------------------------
 * The scene
 * ------------------------------------------------------------------------ */

static void nw_draw_ground(const struct nw_world* world, const struct nw_camera* camera)
{
    const float cell = 2.0f;
    const int reach = 14;
    int origin_x = (int)floorf(camera->x / cell);
    int origin_z = (int)floorf(camera->z / cell);
    int row;
    int column;
    float forward_x = -sinf(camera->yaw * NW_RADIANS);
    float forward_z = -cosf(camera->yaw * NW_RADIANS);

    nw_bind(world->texture_ground);
    glColor3f(1.0f, 1.0f, 1.0f);
    for (row = -reach; row < reach; ++row) {
        float z0 = (float)(origin_z + row) * cell;
        float z1 = z0 + cell;
        int strip_open = 0;
        for (column = -reach; column <= reach; ++column) {
            float x = (float)(origin_x + column) * cell;
            float centre_x = x + cell * 0.5f - camera->x;
            float centre_z = z0 + cell * 0.5f - camera->z;
            float normal[3];
            /* Cells well behind the camera are skipped; the strip breaks there
             * and starts again. */
            int behind = centre_x * forward_x + centre_z * forward_z < -3.0f * cell;
            if (behind || x < -NW_WORLD_HALF || x > NW_WORLD_HALF || z0 < -NW_WORLD_HALF || z1 > NW_WORLD_HALF) {
                if (strip_open) {
                    glEnd();
                    strip_open = 0;
                }
                continue;
            }
            if (!strip_open) {
                glBegin(GL_QUAD_STRIP);
                strip_open = 1;
            }
            /* The far row first: that winds every quad counter-clockwise
             * seen from above. */
            nw_ground_normal(x, z0, normal);
            glNormal3fv(normal);
            glTexCoord2f(x / 3.0f, z0 / 3.0f);
            glVertex3f(x, nw_ground_height(x, z0), z0);
            nw_ground_normal(x, z1, normal);
            glNormal3fv(normal);
            glTexCoord2f(x / 3.0f, z1 / 3.0f);
            glVertex3f(x, nw_ground_height(x, z1), z1);
        }
        if (strip_open) {
            glEnd();
        }
    }
}

static int nw_visible(const struct nw_camera* camera, float x, float z, float radius)
{
    float dx = x - camera->x;
    float dz = z - camera->z;
    float forward_x = -sinf(camera->yaw * NW_RADIANS);
    float forward_z = -cosf(camera->yaw * NW_RADIANS);
    float distance_squared = dx * dx + dz * dz;

    if (distance_squared > (NW_VIEW_DISTANCE + radius) * (NW_VIEW_DISTANCE + radius)) {
        return 0;
    }
    return dx * forward_x + dz * forward_z > -radius - 1.0f;
}

static void nw_draw_trunks(const struct nw_world* world, const struct nw_camera* camera)
{
    int index;

    nw_bind(world->texture_bark);
    glColor3f(1.0f, 1.0f, 1.0f);
    for (index = 0; index < world->tree_count; ++index) {
        const struct nw_tree* tree = &world->trees[index];
        if (!nw_visible(camera, tree->x, tree->z, tree->radius)) {
            continue;
        }
        glPushMatrix();
        glTranslatef(tree->x, nw_ground_height(tree->x, tree->z) - 0.2f, tree->z);
        nw_prism(6, tree->radius, 0.0f, tree->height, 3, 1.5f);
        glPopMatrix();
    }
}

/* Needles are crossed cut-out quads, unlit: a flashlight beam does not reach
 * six metres up, and the silhouettes against the fog are what makes it a
 * forest. They darken with distance on top of the fog. */
static void nw_draw_foliage(const struct nw_world* world, const struct nw_camera* camera)
{
    int index;

    nw_bind(world->texture_foliage);
    for (index = 0; index < world->tree_count; ++index) {
        const struct nw_tree* tree = &world->trees[index];
        float base;
        float top;
        float half;
        float dx;
        float dz;
        float near;
        if (tree->dead || !nw_visible(camera, tree->x, tree->z, tree->foliage_width)) {
            continue;
        }
        dx = tree->x - camera->x;
        dz = tree->z - camera->z;
        near = 1.0f - sqrtf(dx * dx + dz * dz) / NW_VIEW_DISTANCE;
        if (near < 0.2f) {
            near = 0.2f;
        }
        base = nw_ground_height(tree->x, tree->z) + tree->height * 0.3f;
        top = base + tree->height * 0.85f;
        half = tree->foliage_width * 0.5f;
        glColor3f(0.55f * near, 0.62f * near, 0.58f * near);
        glPushMatrix();
        glTranslatef(tree->x, 0.0f, tree->z);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f);
        glVertex3f(-half, base, 0.0f);
        glTexCoord2f(1.0f, 0.0f);
        glVertex3f(half, base, 0.0f);
        glTexCoord2f(1.0f, 1.0f);
        glVertex3f(half, top, 0.0f);
        glTexCoord2f(0.0f, 1.0f);
        glVertex3f(-half, top, 0.0f);
        glTexCoord2f(0.0f, 0.0f);
        glVertex3f(0.0f, base, -half);
        glTexCoord2f(1.0f, 0.0f);
        glVertex3f(0.0f, base, half);
        glTexCoord2f(1.0f, 1.0f);
        glVertex3f(0.0f, top, half);
        glTexCoord2f(0.0f, 1.0f);
        glVertex3f(0.0f, top, -half);
        glEnd();
        glPopMatrix();
    }
}

static void nw_draw_landmark(const struct nw_world* world, const struct nw_landmark* landmark)
{
    glPushMatrix();
    glTranslatef(landmark->x, nw_ground_height(landmark->x, landmark->z) - 0.15f, landmark->z);
    glRotatef(landmark->yaw, 0.0f, 1.0f, 0.0f);
    switch (landmark->kind) {
    case NW_LANDMARK_ROCK:
        nw_bind(world->texture_stone);
        nw_box(3.2f, 2.4f, 3.2f, 2.0f);
        glPushMatrix();
        glTranslatef(0.4f, 2.4f, -0.3f);
        glRotatef(20.0f, 0.0f, 1.0f, 0.0f);
        nw_box(1.8f, 0.9f, 1.6f, 2.0f);
        glPopMatrix();
        break;
    case NW_LANDMARK_SHED:
        nw_bind(world->texture_wood);
        nw_box(3.6f, 2.6f, 3.2f, 1.5f);
        glPushMatrix();
        glTranslatef(0.0f, 2.6f, 0.0f);
        nw_box(4.0f, 0.2f, 3.6f, 1.5f);
        glPopMatrix();
        break;
    case NW_LANDMARK_TANK:
        nw_bind(world->texture_metal);
        glPushMatrix();
        glRotatef(22.5f, 0.0f, 1.0f, 0.0f);
        nw_prism(8, 1.3f, 0.0f, 3.4f, 3, 2.0f);
        glPopMatrix();
        glPushMatrix();
        glTranslatef(0.0f, 3.4f, 0.0f);
        nw_box(2.6f, 0.15f, 2.6f, 2.0f);
        glPopMatrix();
        break;
    case NW_LANDMARK_CAR:
        nw_bind(world->texture_metal);
        nw_box(4.2f, 1.0f, 2.0f, 1.5f);
        glPushMatrix();
        glTranslatef(-0.3f, 1.0f, 0.0f);
        nw_box(2.2f, 0.8f, 1.8f, 1.5f);
        glPopMatrix();
        break;
    case NW_LANDMARK_WALL:
        nw_bind(world->texture_stone);
        nw_box(6.0f, 2.8f, 0.6f, 1.5f);
        glPushMatrix();
        glTranslatef(2.4f, 2.8f, 0.0f);
        nw_box(1.2f, 0.7f, 0.6f, 1.5f);
        glPopMatrix();
        break;
    case NW_LANDMARK_OAK:
        nw_bind(world->texture_bark);
        glRotatef(22.5f, 0.0f, 1.0f, 0.0f);
        nw_prism(8, 0.9f, 0.0f, 9.0f, 4, 1.5f);
        break;
    case NW_LANDMARK_SIGN:
        nw_bind(world->texture_wood);
        glPushMatrix();
        glTranslatef(0.0f, 0.0f, -0.05f);
        nw_box(0.15f, 2.3f, 0.1f, 1.0f);
        glPopMatrix();
        glPushMatrix();
        glTranslatef(0.0f, 1.2f, 0.0f);
        nw_box(1.2f, 0.75f, 0.05f, 1.0f);
        glPopMatrix();
        break;
    case NW_LANDMARK_ARCH:
    default:
        nw_bind(world->texture_stone);
        glPushMatrix();
        glTranslatef(-2.0f, 0.0f, 0.0f);
        nw_box(0.9f, 3.0f, 0.9f, 1.5f);
        glPopMatrix();
        glPushMatrix();
        glTranslatef(2.0f, 0.0f, 0.0f);
        nw_box(0.9f, 3.0f, 0.9f, 1.5f);
        glPopMatrix();
        glPushMatrix();
        glTranslatef(0.0f, 3.0f, 0.0f);
        nw_box(4.9f, 0.8f, 0.9f, 1.5f);
        glPopMatrix();
        break;
    }
    glPopMatrix();
}

static void nw_draw_photo(const struct nw_world* world, const struct nw_landmark* landmark)
{
    const float half_width = 0.22f;
    const float half_height = 0.26f;

    nw_bind(world->texture_photo);
    glPushMatrix();
    glTranslatef(landmark->photo_x, landmark->photo_y, landmark->photo_z);
    glRotatef(landmark->photo_yaw, 0.0f, 1.0f, 0.0f);
    glNormal3f(0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex3f(-half_width, -half_height, 0.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex3f(half_width, -half_height, 0.0f);
    glTexCoord2f(1.0f, 1.0f);
    glVertex3f(half_width, half_height, 0.0f);
    glTexCoord2f(0.0f, 1.0f);
    glVertex3f(-half_width, half_height, 0.0f);
    glEnd();
    glPopMatrix();
}

static void nw_draw_fence(const struct nw_world* world, const struct nw_camera* camera)
{
    int side;
    float post;

    nw_bind(world->texture_wood);
    for (side = 0; side < 4; ++side) {
        for (post = -NW_FENCE; post <= NW_FENCE; post += 4.0f) {
            float x = side < 2 ? post : (side == 2 ? -NW_FENCE : NW_FENCE);
            float z = side < 2 ? (side == 0 ? -NW_FENCE : NW_FENCE) : post;
            if (!nw_visible(camera, x, z, 2.0f)) {
                continue;
            }
            glPushMatrix();
            glTranslatef(x, nw_ground_height(x, z) - 0.1f, z);
            if (side >= 2) {
                glRotatef(90.0f, 0.0f, 1.0f, 0.0f);
            }
            nw_box(0.18f, 1.4f, 0.18f, 1.0f);
            glTranslatef(2.0f, 0.5f, 0.0f);
            nw_box(4.0f, 0.12f, 0.08f, 1.0f);
            glTranslatef(0.0f, 0.55f, 0.0f);
            nw_box(4.0f, 0.12f, 0.08f, 1.0f);
            glPopMatrix();
        }
    }
}

/* The Warden: taller than a man, pale as bone, a dark coat, a crown of
 * antlers. Lit by the flashlight like everything else, so it is a shape in the
 * fog until the beam finds it. */
static void nw_draw_warden(const struct nw_warden* warden)
{
    static const GLfloat bone[4] = {0.62f, 0.58f, 0.52f, 1.0f};
    static const GLfloat coat[4] = {0.05f, 0.05f, 0.06f, 1.0f};
    float ground = nw_ground_height(warden->x, warden->z);

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_COLOR_MATERIAL);
    glPushMatrix();
    glTranslatef(warden->x, ground, warden->z);
    glRotatef(warden->yaw, 0.0f, 1.0f, 0.0f);

    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, coat);
    /* Legs, then a long coat. */
    glPushMatrix();
    glTranslatef(-0.16f, 0.0f, 0.0f);
    nw_box(0.14f, 1.1f, 0.16f, 1.0f);
    glTranslatef(0.32f, 0.0f, 0.0f);
    nw_box(0.14f, 1.1f, 0.16f, 1.0f);
    glPopMatrix();
    glPushMatrix();
    glTranslatef(0.0f, 0.7f, 0.0f);
    nw_box(0.62f, 1.4f, 0.34f, 1.0f);
    glPopMatrix();

    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, bone);
    /* Arms, too long, hanging to the knees. */
    glPushMatrix();
    glTranslatef(-0.4f, 0.55f, 0.0f);
    nw_box(0.1f, 1.5f, 0.1f, 1.0f);
    glTranslatef(0.8f, 0.0f, 0.0f);
    nw_box(0.1f, 1.5f, 0.1f, 1.0f);
    glPopMatrix();
    /* Neck and skull. */
    glPushMatrix();
    glTranslatef(0.0f, 2.1f, 0.0f);
    nw_box(0.1f, 0.22f, 0.1f, 1.0f);
    glTranslatef(0.0f, 0.2f, 0.02f);
    nw_box(0.3f, 0.38f, 0.32f, 1.0f);
    glPopMatrix();
    /* Antlers: two tines a side. */
    {
        int side;
        for (side = -1; side <= 1; side += 2) {
            glPushMatrix();
            glTranslatef(0.12f * (float)side, 2.55f, 0.0f);
            glRotatef(-28.0f * (float)side, 0.0f, 0.0f, 1.0f);
            nw_box(0.05f, 0.6f, 0.05f, 1.0f);
            glTranslatef(0.0f, 0.35f, 0.0f);
            glRotatef(-35.0f * (float)side, 0.0f, 0.0f, 1.0f);
            nw_box(0.04f, 0.32f, 0.04f, 1.0f);
            glPopMatrix();
        }
    }
    glPopMatrix();
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_TEXTURE_2D);
}

/* Its eyes are the one thing in the forest that gives light: an additive glow
 * that ignores the fog, so they are what is seen first and from furthest. */
static void nw_draw_warden_eyes(const struct nw_warden* warden)
{
    float ground = nw_ground_height(warden->x, warden->z);
    int side;

    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_FOG);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glPushMatrix();
    glTranslatef(warden->x, ground, warden->z);
    glRotatef(warden->yaw, 0.0f, 1.0f, 0.0f);
    for (side = -1; side <= 1; side += 2) {
        float cx = 0.075f * (float)side;
        float cy = 2.48f;
        float cz = 0.19f;
        /* A hot core and a wide halo: at twenty metres the halo is what is
         * left, a smudge of red in the fog. */
        glBegin(GL_TRIANGLE_FAN);
        glColor3f(1.0f, 0.3f, 0.22f);
        glVertex3f(cx, cy, cz);
        glColor3f(0.5f, 0.0f, 0.0f);
        glVertex3f(cx - 0.035f, cy - 0.025f, cz);
        glVertex3f(cx + 0.035f, cy - 0.025f, cz);
        glVertex3f(cx + 0.035f, cy + 0.025f, cz);
        glVertex3f(cx - 0.035f, cy + 0.025f, cz);
        glVertex3f(cx - 0.035f, cy - 0.025f, cz);
        glEnd();
        glBegin(GL_TRIANGLE_FAN);
        glColor3f(0.35f, 0.04f, 0.02f);
        glVertex3f(cx, cy, cz + 0.005f);
        glColor3f(0.0f, 0.0f, 0.0f);
        glVertex3f(cx - 0.12f, cy - 0.09f, cz + 0.005f);
        glVertex3f(cx + 0.12f, cy - 0.09f, cz + 0.005f);
        glVertex3f(cx + 0.12f, cy + 0.09f, cz + 0.005f);
        glVertex3f(cx - 0.12f, cy + 0.09f, cz + 0.005f);
        glVertex3f(cx - 0.12f, cy - 0.09f, cz + 0.005f);
        glEnd();
    }
    glPopMatrix();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_FOG);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING);
}

static void nw_setup_frame(const struct nw_camera* camera, int flashlight, float aspect)
{
    /* The flashlight is held at the hip, slightly to the right, pointing
     * where the player looks: a spot light placed in eye space. */
    static const GLfloat torch_position[4] = {0.18f, -0.25f, 0.0f, 1.0f};
    static const GLfloat torch_direction[3] = {-0.05f, -0.28f, -1.0f};
    /* Brighter than 1: GL does not clamp a light's colour, only the result,
     * and a beam grazing the ground needs the headroom. */
    static const GLfloat torch_colour[4] = {2.2f, 2.05f, 1.75f, 1.0f};
    static const GLfloat moon_direction[4] = {-0.3f, 1.0f, 0.4f, 0.0f};
    static const GLfloat moon_colour[4] = {0.09f, 0.10f, 0.16f, 1.0f};
    static const GLfloat ambient[4] = {0.04f, 0.045f, 0.065f, 1.0f};
    static const GLfloat black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float near_half = 0.1f * tanf(36.0f * NW_RADIANS);

    glClearColor(g_fog_colour[0], g_fog_colour[1], g_fog_colour[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-near_half * aspect, near_half * aspect, -near_half, near_half, 0.1, NW_VIEW_DISTANCE + 4.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glLightfv(GL_LIGHT0, GL_POSITION, torch_position);
    glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, torch_direction);
    glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 38.0f);
    glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 6.0f);
    glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, 0.35f);
    glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0.10f);
    glLightf(GL_LIGHT0, GL_QUADRATIC_ATTENUATION, 0.025f);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, flashlight ? torch_colour : black);
    glLightfv(GL_LIGHT0, GL_AMBIENT, black);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);

    glRotatef(-camera->pitch, 1.0f, 0.0f, 0.0f);
    glRotatef(-camera->yaw, 0.0f, 1.0f, 0.0f);
    glTranslatef(-camera->x, -(nw_ground_height(camera->x, camera->z) + NW_EYE_HEIGHT + camera->bob), -camera->z);

    /* The moon is fixed in the world, so it is placed after the camera. */
    glLightfv(GL_LIGHT1, GL_POSITION, moon_direction);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, moon_colour);
    glLightfv(GL_LIGHT1, GL_AMBIENT, black);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, NW_FOG_START);
    glFogf(GL_FOG_END, NW_FOG_END);
    glFogfv(GL_FOG_COLOR, g_fog_colour);
}

void nw_world_render(const struct nw_world* world, const struct nw_camera* camera, const struct nw_warden* warden,
    int flashlight, float aspect)
{
    int index;

    nw_setup_frame(camera, flashlight, aspect);

    nw_draw_ground(world, camera);
    nw_draw_trunks(world, camera);
    for (index = 0; index < NW_PHOTO_COUNT; ++index) {
        const struct nw_landmark* landmark = &world->landmarks[index];
        if (nw_visible(camera, landmark->x, landmark->z, landmark->radius + 2.0f)) {
            nw_draw_landmark(world, landmark);
        }
    }
    nw_draw_fence(world, camera);

    /* Photographs: a faint emission so that the white frame catches the eye
     * at the edge of the beam. */
    {
        static const GLfloat glow[4] = {0.08f, 0.08f, 0.07f, 1.0f};
        static const GLfloat none[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, glow);
        glColor3f(1.0f, 1.0f, 1.0f);
        for (index = 0; index < NW_PHOTO_COUNT; ++index) {
            const struct nw_landmark* landmark = &world->landmarks[index];
            if (!landmark->photo_taken && nw_visible(camera, landmark->photo_x, landmark->photo_z, 1.0f)) {
                nw_draw_photo(world, landmark);
            }
        }
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, none);
    }

    if (warden != 0 && warden->present && nw_visible(camera, warden->x, warden->z, 1.0f)) {
        nw_draw_warden(warden);
    }

    /* Cut-out needles last among the opaque things: two-sided, unlit. */
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    nw_draw_foliage(world, camera);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);

    if (warden != 0 && warden->present && nw_visible(camera, warden->x, warden->z, 1.0f)) {
        nw_draw_warden_eyes(warden);
    }
}
