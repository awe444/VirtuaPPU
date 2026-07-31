#include "cpu/mode1.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtuappu.h"

virtuappu_mode1_pre_line_fn virtuappu_mode1_pre_line_callback = NULL;

typedef struct Mode1TilemapEntry {
    uint16_t raw;
} Mode1TilemapEntry;

typedef struct Mode1OAMAttr {
    uint16_t attr0;
    uint16_t attr1;
    uint16_t attr2;
} Mode1OAMAttr;

typedef enum Mode1BlendEffect {
    MODE1_BLEND_NONE = 0,
    MODE1_BLEND_ALPHA = 1,
    MODE1_BLEND_BRIGHTEN = 2,
    MODE1_BLEND_DARKEN = 3
} Mode1BlendEffect;

static uint8_t mode1_default_io_mem[MODE1_IO_MEM_SIZE];
static uint8_t mode1_default_vram[MODE1_VRAM_SIZE];
static uint16_t mode1_default_bg_palette[MODE1_PALETTE_COLORS];
static uint16_t mode1_default_obj_palette[MODE1_PALETTE_COLORS];
static uint16_t mode1_default_oam_mem[MODE1_OAM_HALFWORDS];

static VirtuaPPUMode1GbaMemory mode1_memory = {
    mode1_default_io_mem,
    mode1_default_vram,
    mode1_default_bg_palette,
    mode1_default_obj_palette,
    mode1_default_oam_mem
};

static const uint8_t mode1_obj_widths[3][4] = {
    {8, 16, 32, 64},
    {16, 32, 32, 64},
    {8, 8, 16, 32}
};

static const uint8_t mode1_obj_heights[3][4] = {
    {8, 16, 32, 64},
    {8, 8, 16, 32},
    {16, 32, 32, 64}
};

static uint16_t mode1_tile_index(Mode1TilemapEntry entry)
{
    return entry.raw & 0x03FFu;
}

static bool mode1_tile_hflip(Mode1TilemapEntry entry)
{
    return ((entry.raw >> 10u) & 1u) != 0u;
}

static bool mode1_tile_vflip(Mode1TilemapEntry entry)
{
    return ((entry.raw >> 11u) & 1u) != 0u;
}

static uint8_t mode1_tile_palette(Mode1TilemapEntry entry)
{
    return (uint8_t)((entry.raw >> 12u) & 0x0Fu);
}

static bool mode1_oam_affine(Mode1OAMAttr attr)
{
    return ((attr.attr0 >> 8u) & 1u) != 0u;
}

static bool mode1_oam_double_size(Mode1OAMAttr attr)
{
    return mode1_oam_affine(attr) && (((attr.attr0 >> 9u) & 1u) != 0u);
}

static bool mode1_oam_hidden(Mode1OAMAttr attr)
{
    return !mode1_oam_affine(attr) && (((attr.attr0 >> 9u) & 1u) != 0u);
}

static bool mode1_oam_bpp8(Mode1OAMAttr attr)
{
    return ((attr.attr0 >> 13u) & 1u) != 0u;
}

static int mode1_oam_y(Mode1OAMAttr attr)
{
    return attr.attr0 & 0xFF;
}

/* Resolved on-screen y for OAM slot `index`, before the global OBJ offset.
 *
 * The hardware field is 8 bits, so a sprite above the top edge is encoded by
 * wrapping and recovered here by reading the band [MODE1_GBA_HEIGHT,255] as
 * negative. That band has to be wider than the tallest sprite for the
 * recovery to be unambiguous, and at 240 lines it is 16 px against a
 * double-size affine sprite's 128 — so tall sprites straddling the top edge
 * would be pulled back onto the visible screen.
 *
 * Where the host supplies the untruncated y it wins. It is cross-checked
 * against attr0's low byte first: any writer that sets a sprite's y without
 * updating the channel disagrees there and gets the hardware reading, which
 * is what a slot recycled behind the host's back should fall back to.
 */
static int mode1_obj_y(int index, Mode1OAMAttr attr)
{
    int packed = mode1_oam_y(attr);

    if (mode1_memory.oam_y_ext != NULL) {
        int ext = mode1_memory.oam_y_ext[index];
        if ((ext & 0xFF) == packed) {
            return ext;
        }
    }
    return (packed >= MODE1_GBA_HEIGHT) ? (packed - 256) : packed;
}

static uint8_t mode1_oam_shape(Mode1OAMAttr attr)
{
    return (uint8_t)((attr.attr0 >> 14u) & 3u);
}

static int mode1_oam_x(Mode1OAMAttr attr)
{
    return attr.attr1 & 0x1FF;
}

static bool mode1_oam_hflip(Mode1OAMAttr attr)
{
    return !mode1_oam_affine(attr) && (((attr.attr1 >> 12u) & 1u) != 0u);
}

static bool mode1_oam_vflip(Mode1OAMAttr attr)
{
    return !mode1_oam_affine(attr) && (((attr.attr1 >> 13u) & 1u) != 0u);
}

static uint8_t mode1_oam_affine_index(Mode1OAMAttr attr)
{
    return (uint8_t)((attr.attr1 >> 9u) & 0x1Fu);
}

static uint8_t mode1_oam_size(Mode1OAMAttr attr)
{
    return (uint8_t)((attr.attr1 >> 14u) & 3u);
}

static uint16_t mode1_oam_tile_index(Mode1OAMAttr attr)
{
    return attr.attr2 & 0x03FFu;
}

static uint8_t mode1_oam_priority(Mode1OAMAttr attr)
{
    return (uint8_t)((attr.attr2 >> 10u) & 3u);
}

static uint8_t mode1_oam_palette(Mode1OAMAttr attr)
{
    return (uint8_t)((attr.attr2 >> 12u) & 0x0Fu);
}

static bool mode1_is_first_target(uint16_t bldcnt, int layer_id)
{
    return ((bldcnt >> layer_id) & 1u) != 0u;
}

static bool mode1_is_second_target(uint16_t bldcnt, int layer_id)
{
    return ((bldcnt >> (layer_id + 8)) & 1u) != 0u;
}

static uint32_t mode1_alpha_blend(uint32_t top_abgr, uint32_t bottom_abgr, int eva, int evb)
{
    int top_r = (int)((top_abgr >> 0u) & 0xFFu);
    int top_g = (int)((top_abgr >> 8u) & 0xFFu);
    int top_b = (int)((top_abgr >> 16u) & 0xFFu);
    int bottom_r = (int)((bottom_abgr >> 0u) & 0xFFu);
    int bottom_g = (int)((bottom_abgr >> 8u) & 0xFFu);
    int bottom_b = (int)((bottom_abgr >> 16u) & 0xFFu);
    int out_r = (top_r * eva + bottom_r * evb) / 16;
    int out_g = (top_g * eva + bottom_g * evb) / 16;
    int out_b = (top_b * eva + bottom_b * evb) / 16;

    if (out_r > 255) {
        out_r = 255;
    }
    if (out_g > 255) {
        out_g = 255;
    }
    if (out_b > 255) {
        out_b = 255;
    }

    return 0xFF000000u | ((uint32_t)out_b << 16u) | ((uint32_t)out_g << 8u) | (uint32_t)out_r;
}

static uint32_t mode1_brighten(uint32_t abgr, int evy)
{
    int r = (int)((abgr >> 0u) & 0xFFu);
    int g = (int)((abgr >> 8u) & 0xFFu);
    int b = (int)((abgr >> 16u) & 0xFFu);

    r = r + ((255 - r) * evy) / 16;
    g = g + ((255 - g) * evy) / 16;
    b = b + ((255 - b) * evy) / 16;

    if (r > 255) {
        r = 255;
    }
    if (g > 255) {
        g = 255;
    }
    if (b > 255) {
        b = 255;
    }

    return 0xFF000000u | ((uint32_t)b << 16u) | ((uint32_t)g << 8u) | (uint32_t)r;
}

static uint32_t mode1_darken(uint32_t abgr, int evy)
{
    int r = (int)((abgr >> 0u) & 0xFFu);
    int g = (int)((abgr >> 8u) & 0xFFu);
    int b = (int)((abgr >> 16u) & 0xFFu);

    r -= (r * evy) / 16;
    g -= (g * evy) / 16;
    b -= (b * evy) / 16;

    if (r < 0) {
        r = 0;
    }
    if (g < 0) {
        g = 0;
    }
    if (b < 0) {
        b = 0;
    }

    return 0xFF000000u | ((uint32_t)b << 16u) | ((uint32_t)g << 8u) | (uint32_t)r;
}

void virtuappu_mode1_bind_gba_memory(const VirtuaPPUMode1GbaMemory *memory)
{
    mode1_memory.io_mem = (memory != NULL && memory->io_mem != NULL) ? memory->io_mem : mode1_default_io_mem;
    mode1_memory.vram = (memory != NULL && memory->vram != NULL) ? memory->vram : mode1_default_vram;
    mode1_memory.bg_palette = (memory != NULL && memory->bg_palette != NULL) ? memory->bg_palette : mode1_default_bg_palette;
    mode1_memory.obj_palette = (memory != NULL && memory->obj_palette != NULL) ? memory->obj_palette : mode1_default_obj_palette;
    mode1_memory.oam_mem = (memory != NULL && memory->oam_mem != NULL) ? memory->oam_mem : mode1_default_oam_mem;
    mode1_memory.oam_y_ext = (memory != NULL) ? memory->oam_y_ext : NULL;
}

void virtuappu_mode1_get_bound_gba_memory(VirtuaPPUMode1GbaMemory *memory)
{
    if (memory == NULL) {
        return;
    }

    *memory = mode1_memory;
}

uint16_t virtuappu_mode1_io_read16(uint16_t offset)
{
    return (uint16_t)mode1_memory.io_mem[offset] | ((uint16_t)mode1_memory.io_mem[offset + 1u] << 8u);
}

uint32_t virtuappu_mode1_io_read32(uint16_t offset)
{
    return (uint32_t)virtuappu_mode1_io_read16(offset) |
           ((uint32_t)virtuappu_mode1_io_read16((uint16_t)(offset + 2u)) << 16u);
}

uint32_t virtuappu_mode1_rgb555_to_abgr8888(uint16_t color)
{
    uint8_t r = (uint8_t)((color & 0x1Fu) << 3u);
    uint8_t g = (uint8_t)(((color >> 5u) & 0x1Fu) << 3u);
    uint8_t b = (uint8_t)(((color >> 10u) & 0x1Fu) << 3u);

    return 0xFF000000u | ((uint32_t)b << 16u) | ((uint32_t)g << 8u) | (uint32_t)r;
}

int mode1_map_source_audit = 0;
unsigned long mode1_map_source_audit_total = 0;
unsigned long mode1_map_source_audit_bad = 0;

static VirtuaPPUMode1MapSource mode1_map_sources[MODE1_GBA_BG_COUNT];
static bool mode1_map_source_active[MODE1_GBA_BG_COUNT];

static int mode1_obj_clip_left = 0;
static int mode1_obj_clip_right = MODE1_GBA_WIDTH;
static int mode1_obj_clip_top = 0;
static int mode1_obj_clip_bottom = MODE1_GBA_HEIGHT;

void virtuappu_mode1_set_obj_clip(int left, int right)
{
    mode1_obj_clip_left = (left < 0) ? 0 : left;
    mode1_obj_clip_right = (right > MODE1_GBA_WIDTH) ? MODE1_GBA_WIDTH : right;
}

void virtuappu_mode1_set_obj_clip_v(int top, int bottom)
{
    mode1_obj_clip_top = (top < 0) ? 0 : top;
    mode1_obj_clip_bottom = (bottom > MODE1_GBA_HEIGHT) ? MODE1_GBA_HEIGHT : bottom;
}

static int mode1_obj_offset_x = 0;
static int mode1_obj_offset_y = 0;

void virtuappu_mode1_set_obj_offset(int dx, int dy)
{
    mode1_obj_offset_x = dx;
    mode1_obj_offset_y = dy;
}

static VirtuaPPUMode1BgClip mode1_bg_clips[MODE1_GBA_BG_COUNT];
static bool mode1_bg_clip_active[MODE1_GBA_BG_COUNT];

void virtuappu_mode1_set_bg_clip(int bg_index, const VirtuaPPUMode1BgClip *clip)
{
    if (bg_index < 0 || bg_index >= MODE1_GBA_BG_COUNT) {
        return;
    }
    if (clip == NULL || clip->content_width <= 0) {
        mode1_bg_clip_active[bg_index] = false;
        return;
    }
    mode1_bg_clips[bg_index] = *clip;
    mode1_bg_clip_active[bg_index] = true;
}

void virtuappu_mode1_clear_bg_clips(void)
{
    int i;
    for (i = 0; i < MODE1_GBA_BG_COUNT; ++i) {
        mode1_bg_clip_active[i] = false;
    }
}

static VirtuaPPUMode1WindowBounds mode1_window_bounds[2];
static bool mode1_window_bounds_active[2];

void virtuappu_mode1_set_window_bounds(int window_index, const VirtuaPPUMode1WindowBounds *bounds)
{
    if (window_index < 0 || window_index > 1) {
        return;
    }
    if (bounds == NULL) {
        mode1_window_bounds_active[window_index] = false;
        return;
    }
    mode1_window_bounds[window_index] = *bounds;
    mode1_window_bounds_active[window_index] = true;
}

void virtuappu_mode1_set_window_h_bounds(int window_index, int left, int right)
{
    if (window_index < 0 || window_index > 1) {
        return;
    }
    mode1_window_bounds[window_index].left = left;
    mode1_window_bounds[window_index].right = right;
    mode1_window_bounds_active[window_index] = true;
}

void virtuappu_mode1_clear_window_bounds(void)
{
    mode1_window_bounds_active[0] = false;
    mode1_window_bounds_active[1] = false;
}

void virtuappu_mode1_set_map_source(int bg_index, const VirtuaPPUMode1MapSource *source)
{
    if (bg_index < 0 || bg_index >= MODE1_GBA_BG_COUNT) {
        return;
    }
    if (source == NULL || source->map == NULL || source->stride <= 0 ||
        source->width_tiles <= 0 || source->height_tiles <= 0) {
        mode1_map_source_active[bg_index] = false;
        return;
    }
    mode1_map_sources[bg_index] = *source;
    mode1_map_source_active[bg_index] = true;
}

bool virtuappu_mode1_has_map_source(int bg_index)
{
    if (bg_index < 0 || bg_index >= MODE1_GBA_BG_COUNT) {
        return false;
    }
    return mode1_map_source_active[bg_index];
}

void virtuappu_mode1_clear_map_sources(void)
{
    int i;
    for (i = 0; i < MODE1_GBA_BG_COUNT; ++i) {
        mode1_map_source_active[i] = false;
    }
}

void virtuappu_mode1_render_text_bg_line(int bg_index, int line, uint32_t *line_buffer, uint8_t *priority_buffer)
{
    uint16_t bgcnt = virtuappu_mode1_io_read16((uint16_t)(MODE1_IO_BG0CNT + bg_index * 2));
    uint8_t priority = (uint8_t)(bgcnt & 3u);
    uint32_t char_base = (uint32_t)((bgcnt >> 2u) & 3u) * 0x4000u;
    bool mosaic_on = ((bgcnt >> 6u) & 1u) != 0u;
    bool bpp8 = ((bgcnt >> 7u) & 1u) != 0u;
    uint32_t screen_base = (uint32_t)((bgcnt >> 8u) & 0x1Fu) * 0x800u;
    uint16_t size_flag = (uint16_t)((bgcnt >> 14u) & 3u);
    int map_width_tiles = (size_flag & 1u) ? 64 : 32;
    int map_height_tiles = (size_flag & 2u) ? 64 : 32;
    int scroll_x = virtuappu_mode1_io_read16((uint16_t)(MODE1_IO_BG0HOFS + bg_index * 4)) & 0x1FF;
    int scroll_y = virtuappu_mode1_io_read16((uint16_t)(MODE1_IO_BG0VOFS + bg_index * 4)) & 0x1FF;
    uint16_t mosaic_reg = virtuappu_mode1_io_read16(MODE1_IO_MOSAIC);
    mosaic_on = mosaic_on && getenv("TMC_ENABLE_MOSAIC") != NULL;
    int mosaic_h = mosaic_on ? (int)((mosaic_reg & 0x0Fu) + 1u) : 1;
    int mosaic_v = mosaic_on ? (int)(((mosaic_reg >> 4u) & 0x0Fu) + 1u) : 1;
    const VirtuaPPUMode1MapSource *map_src =
        mode1_map_source_active[bg_index] ? &mode1_map_sources[bg_index] : NULL;
    int eff_line = (line / mosaic_v) * mosaic_v;
    const VirtuaPPUMode1BgClip *clip =
        mode1_bg_clip_active[bg_index] ? &mode1_bg_clips[bg_index] : NULL;
    int clipped_line = eff_line;
    int src_y;
    int tile_row;
    int pixel_y;
    int x;

    /* Rows outside the clip's vertical span contribute nothing at all, so
     * the whole line is backdrop and there is no column loop to run. */
    if (clip != NULL) {
        clipped_line = eff_line - clip->offset_y;
        if (clipped_line < 0 || clipped_line >= clip->content_height) {
            return;
        }
    }

    /* Map-source layers address an absolute room position and do not wrap;
     * screenblock layers wrap modulo the hardware map size. */
    src_y = map_src ? (clipped_line + map_src->origin_y)
                    : ((clipped_line + scroll_y) % (map_height_tiles * 8));
    tile_row = src_y / 8;
    pixel_y = src_y % 8;

    if (map_src && (src_y < 0 || tile_row >= map_src->height_tiles)) {
        return; /* line lies outside the room: leave backdrop */
    }

    for (x = 0; x < MODE1_GBA_WIDTH; ++x) {
        int eff_x = (x / mosaic_h) * mosaic_h;
        int clipped_x = eff_x;
        int src_x;
        if (clip != NULL) {
            clipped_x = eff_x - clip->offset_x;
            if (clipped_x < 0 || clipped_x >= clip->content_width) {
                continue; /* outside the layer's content: show backdrop */
            }
        }
        src_x = map_src ? (clipped_x + map_src->origin_x)
                        : ((clipped_x + scroll_x) % (map_width_tiles * 8));
        int tile_col = src_x / 8;
        int pixel_x = src_x % 8;
        Mode1TilemapEntry tile_entry;
        int tile_pixel_x;
        int tile_pixel_y;
        uint8_t color_index;
        uint16_t rgb555;

        if (map_src) {
            if (src_x < 0 || tile_col >= map_src->width_tiles) {
                continue; /* column outside the room */
            }
            tile_entry.raw = map_src->map[(size_t)tile_row * (size_t)map_src->stride +
                                          (size_t)tile_col];
            /* The audit is an equivalence check against the hardware
             * path, so it is only meaningful at GBA-native width. Wider
             * than that the engine still streams a 32-tile screenblock, but
             * from a camera the wide build has clamped differently, so the
             * buffer-column-to-map-column relation this compares against no
             * longer holds — and beyond the streamed window there is no
             * data at all. Report nothing rather than a misleading number;
             * `--mapsource-audit` is a 240 regression gate. */
            if (mode1_map_source_audit && MODE1_GBA_WIDTH <= 240) {
                int sb_y = (eff_line + scroll_y) % (map_height_tiles * 8);
                int sb_x = (eff_x + scroll_x) % (map_width_tiles * 8);
                int r = sb_y / 8, c = sb_x / 8;
                int bi = (c / 32) + (r / 32) * (map_width_tiles / 32);
                uint32_t a = screen_base + (uint32_t)bi * 0x800u +
                             (uint32_t)((r % 32) * 32 + (c % 32)) * 2u;
                uint16_t hw = (uint16_t)mode1_memory.vram[a] |
                              ((uint16_t)mode1_memory.vram[a + 1u] << 8u);
                mode1_map_source_audit_total++;
                if (hw != tile_entry.raw) {
                    if (mode1_map_source_audit_bad < 6) {
                        fprintf(stderr,
                                "[mapsrc-audit] bg=%d line=%d x=%d origin=(%d,%d) "
                                "hofs=%d vofs=%d map[%d,%d]=0x%04X hw[%d,%d]=0x%04X "
                                "expect_col=%d expect_row=%d\n",
                                bg_index, line, x, map_src->origin_x, map_src->origin_y,
                                scroll_x, scroll_y, tile_row, tile_col, tile_entry.raw,
                                r, c, hw,
                                (map_src->origin_x >> 3) + c - (scroll_x >> 3),
                                (map_src->origin_y >> 3) + r - (scroll_y >> 3));
                    }
                    mode1_map_source_audit_bad++;
                }
            }
        } else {
            int screen_block_x = tile_col / 32;
            int screen_block_y = tile_row / 32;
            int screen_block_index = screen_block_x + screen_block_y * (map_width_tiles / 32);
            int local_col = tile_col % 32;
            int local_row = tile_row % 32;
            uint32_t map_addr = screen_base + (uint32_t)screen_block_index * 0x800u +
                                (uint32_t)(local_row * 32 + local_col) * 2u;
            tile_entry.raw = (uint16_t)mode1_memory.vram[map_addr] |
                             ((uint16_t)mode1_memory.vram[map_addr + 1u] << 8u);
        }
        tile_pixel_x = mode1_tile_hflip(tile_entry) ? (7 - pixel_x) : pixel_x;
        tile_pixel_y = mode1_tile_vflip(tile_entry) ? (7 - pixel_y) : pixel_y;

        if (bpp8) {
            uint32_t addr = char_base + (uint32_t)mode1_tile_index(tile_entry) * 64u +
                            (uint32_t)tile_pixel_y * 8u + (uint32_t)tile_pixel_x;
            color_index = (addr < MODE1_VRAM_SIZE) ? mode1_memory.vram[addr] : 0u;
        } else {
            uint32_t addr = char_base + (uint32_t)mode1_tile_index(tile_entry) * 32u +
                            (uint32_t)tile_pixel_y * 4u + (uint32_t)(tile_pixel_x / 2);
            uint8_t packed = (addr < MODE1_VRAM_SIZE) ? mode1_memory.vram[addr] : 0u;
            color_index = (tile_pixel_x & 1) ? (packed >> 4u) : (packed & 0x0Fu);
        }

        if (color_index == 0u) {
            continue;
        }

        if (bpp8) {
            rgb555 = mode1_memory.bg_palette[color_index];
        } else {
            rgb555 = mode1_memory.bg_palette[(size_t)mode1_tile_palette(tile_entry) * 16u + color_index];
        }

        line_buffer[x] = virtuappu_mode1_rgb555_to_abgr8888(rgb555);
        priority_buffer[x] = priority;
    }
}

void virtuappu_mode1_render_obj_line(int line, bool obj_1d, uint32_t *line_buffer, uint8_t *priority_buffer)
{
    const uint32_t obj_tile_base = 0x10000u;
    int i;

    /* Rows outside the OBJ clip are border, not content — same reasoning as
     * the horizontal pair, and cheaper to reject a whole line at once. */
    if (line < mode1_obj_clip_top || line >= mode1_obj_clip_bottom) {
        return;
    }

    for (i = MODE1_GBA_OAM_COUNT - 1; i >= 0; --i) {
        Mode1OAMAttr attr;
        uint8_t shape;
        uint8_t size;
        int obj_width;
        int obj_height;
        bool is_affine;
        int bounds_width;
        int bounds_height;
        int obj_y;
        int obj_x;
        bool bpp8;
        uint8_t priority;
        uint16_t base_tile;
        int tiles_w;
        int16_t pa = 0x100;
        int16_t pb = 0;
        int16_t pc = 0;
        int16_t pd = 0x100;
        int half_width;
        int half_height;
        int sprite_half_width;
        int sprite_half_height;
        int input_rel_y;
        int sx;

        attr.attr0 = mode1_memory.oam_mem[i * 4];
        attr.attr1 = mode1_memory.oam_mem[i * 4 + 1];
        attr.attr2 = mode1_memory.oam_mem[i * 4 + 2];

        if (mode1_oam_hidden(attr)) {
            continue;
        }

        shape = mode1_oam_shape(attr);
        size = mode1_oam_size(attr);
        obj_width = mode1_obj_widths[shape][size];
        obj_height = mode1_obj_heights[shape][size];
        is_affine = mode1_oam_affine(attr);
        bounds_width = obj_width;
        bounds_height = obj_height;

        if (is_affine && mode1_oam_double_size(attr)) {
            bounds_width *= 2;
            bounds_height *= 2;
        }

        obj_y = mode1_obj_y(i, attr);
        obj_y += mode1_obj_offset_y;
        if (line < obj_y || line >= obj_y + bounds_height) {
            continue;
        }

        obj_x = mode1_oam_x(attr);
        if (obj_x >= MODE1_GBA_WIDTH) {
            obj_x -= 512;
        }
        /* Applied after the hardware wrap fixups so the offset shifts the
         * resolved on-screen position, not the encoded field. */
        obj_x += mode1_obj_offset_x;

        bpp8 = mode1_oam_bpp8(attr);
        priority = mode1_oam_priority(attr);
        base_tile = mode1_oam_tile_index(attr);
        tiles_w = obj_width / 8;

        if (is_affine) {
            int affine_group = mode1_oam_affine_index(attr);
            pa = (int16_t)mode1_memory.oam_mem[affine_group * 16 + 3];
            pb = (int16_t)mode1_memory.oam_mem[affine_group * 16 + 7];
            pc = (int16_t)mode1_memory.oam_mem[affine_group * 16 + 11];
            pd = (int16_t)mode1_memory.oam_mem[affine_group * 16 + 15];
        }

        half_width = bounds_width / 2;
        half_height = bounds_height / 2;
        sprite_half_width = obj_width / 2;
        sprite_half_height = obj_height / 2;
        input_rel_y = line - obj_y - half_height;

        for (sx = 0; sx < bounds_width; ++sx) {
            int screen_x = obj_x + sx;
            if (screen_x < mode1_obj_clip_left || screen_x >= mode1_obj_clip_right) {
                continue; /* border, not world: hardware would never draw here */
            }
            int tex_x;
            int tex_y;
            int tile_row;
            int pixel_y;
            int tile_col;
            int pixel_x;
            uint16_t tile_index;
            uint8_t color_index;
            uint16_t rgb555;

            if (screen_x < 0 || screen_x >= MODE1_GBA_WIDTH) {
                continue;
            }

            if (is_affine) {
                int input_rel_x = sx - half_width;
                tex_x = ((pa * input_rel_x + pb * input_rel_y) >> 8) + sprite_half_width;
                tex_y = ((pc * input_rel_x + pd * input_rel_y) >> 8) + sprite_half_height;
                if (tex_x < 0 || tex_x >= obj_width || tex_y < 0 || tex_y >= obj_height) {
                    continue;
                }
            } else {
                int draw_x = mode1_oam_hflip(attr) ? (obj_width - 1 - sx) : sx;
                int draw_y = line - obj_y;
                if (mode1_oam_vflip(attr)) {
                    draw_y = obj_height - 1 - draw_y;
                }
                tex_x = draw_x;
                tex_y = draw_y;
            }

            tile_row = tex_y / 8;
            pixel_y = tex_y % 8;
            tile_col = tex_x / 8;
            pixel_x = tex_x % 8;

            if (obj_1d) {
                tile_index = (uint16_t)(base_tile + tile_row * tiles_w + tile_col);
                if (bpp8) {
                    tile_index = (uint16_t)(base_tile + (tile_row * tiles_w + tile_col) * 2);
                }
            } else {
                tile_index = (uint16_t)(base_tile + tile_row * 32 + tile_col);
                if (bpp8) {
                    tile_index = (uint16_t)(base_tile + tile_row * 32 + tile_col * 2);
                }
            }

            if (bpp8) {
                uint32_t addr = obj_tile_base + (uint32_t)tile_index * 32u + (uint32_t)pixel_y * 8u + (uint32_t)pixel_x;
                color_index = (addr < MODE1_VRAM_SIZE) ? mode1_memory.vram[addr] : 0u;
            } else {
                uint32_t addr = obj_tile_base + (uint32_t)tile_index * 32u + (uint32_t)pixel_y * 4u + (uint32_t)(pixel_x / 2);
                uint8_t packed = (addr < MODE1_VRAM_SIZE) ? mode1_memory.vram[addr] : 0u;
                color_index = (pixel_x & 1) ? (packed >> 4u) : (packed & 0x0Fu);
            }

            if (color_index == 0u) {
                continue;
            }

            if (line_buffer[screen_x] != 0u && priority_buffer[screen_x] < priority) {
                continue;
            }

            if (bpp8) {
                rgb555 = mode1_memory.obj_palette[color_index];
            } else {
                rgb555 = mode1_memory.obj_palette[(size_t)mode1_oam_palette(attr) * 16u + color_index];
            }

            line_buffer[screen_x] = virtuappu_mode1_rgb555_to_abgr8888(rgb555);
            priority_buffer[screen_x] = priority;
        }
    }
}

void virtuappu_mode1_composite_line(
    int line,
    uint32_t bg_layers[MODE1_GBA_BG_COUNT][MODE1_GBA_WIDTH],
    uint8_t bg_priority[MODE1_GBA_BG_COUNT][MODE1_GBA_WIDTH],
    uint32_t obj_layer[MODE1_GBA_WIDTH],
    uint8_t obj_priority[MODE1_GBA_WIDTH],
    uint16_t dispcnt)
{
    uint32_t backdrop_color = virtuappu_mode1_rgb555_to_abgr8888(mode1_memory.bg_palette[0]);
    bool bg_enabled[MODE1_GBA_BG_COUNT] = {
        (dispcnt & MODE1_DISP_BG0_ON) != 0u,
        (dispcnt & MODE1_DISP_BG1_ON) != 0u,
        (dispcnt & MODE1_DISP_BG2_ON) != 0u,
        (dispcnt & MODE1_DISP_BG3_ON) != 0u
    };
    bool obj_enabled = (dispcnt & MODE1_DISP_OBJ_ON) != 0u;
    uint16_t bldcnt = virtuappu_mode1_io_read16(MODE1_IO_BLDCNT);
    uint16_t bldalpha = virtuappu_mode1_io_read16(MODE1_IO_BLDALPHA);
    uint16_t bldy = virtuappu_mode1_io_read16(MODE1_IO_BLDY);
    Mode1BlendEffect effect = (Mode1BlendEffect)((bldcnt >> 6u) & 3u);
    int eva = bldalpha & 0x1Fu;
    int evb = (bldalpha >> 8u) & 0x1Fu;
    int evy = bldy & 0x1Fu;
    uint8_t bg_order[MODE1_GBA_BG_COUNT] = {0, 1, 2, 3};
    uint8_t bg_order_priority[MODE1_GBA_BG_COUNT];
    bool win0_on = (dispcnt & MODE1_DISP_WIN0_ON) != 0u;
    bool win1_on = (dispcnt & MODE1_DISP_WIN1_ON) != 0u;
    bool any_window = win0_on || win1_on;
    uint16_t winin = virtuappu_mode1_io_read16(MODE1_IO_WININ);
    uint16_t winout = virtuappu_mode1_io_read16(MODE1_IO_WINOUT);
    uint16_t win0h = virtuappu_mode1_io_read16(MODE1_IO_WIN0H);
    uint16_t win0v = virtuappu_mode1_io_read16(MODE1_IO_WIN0V);
    int win0_left = mode1_window_bounds_active[0] ? mode1_window_bounds[0].left : (int)(win0h >> 8u);
    int win0_right = mode1_window_bounds_active[0] ? mode1_window_bounds[0].right : (int)(win0h & 0xFFu);
    int win0_top = mode1_window_bounds_active[0] ? mode1_window_bounds[0].top : (int)(win0v >> 8u);
    int win0_bottom = mode1_window_bounds_active[0] ? mode1_window_bounds[0].bottom : (int)(win0v & 0xFFu);
    bool win0_h_wrap;
    bool win0_v_active;
    uint16_t win1h = virtuappu_mode1_io_read16(MODE1_IO_WIN1H);
    uint16_t win1v = virtuappu_mode1_io_read16(MODE1_IO_WIN1V);
    int win1_left = mode1_window_bounds_active[1] ? mode1_window_bounds[1].left : (int)(win1h >> 8u);
    int win1_right = mode1_window_bounds_active[1] ? mode1_window_bounds[1].right : (int)(win1h & 0xFFu);
    int win1_top = mode1_window_bounds_active[1] ? mode1_window_bounds[1].top : (int)(win1v >> 8u);
    int win1_bottom = mode1_window_bounds_active[1] ? mode1_window_bounds[1].bottom : (int)(win1v & 0xFFu);
    bool win1_h_wrap;
    bool win1_v_active;
    uint8_t win0_ctrl = (uint8_t)(winin & 0x3Fu);
    uint8_t win1_ctrl = (uint8_t)((winin >> 8u) & 0x3Fu);
    uint8_t outside_ctrl = (uint8_t)(winout & 0x3Fu);
    int i;
    int x;

    (void)bg_priority;

    if (eva > 16) {
        eva = 16;
    }
    if (evb > 16) {
        evb = 16;
    }
    if (evy > 16) {
        evy = 16;
    }

    if (win0_right > MODE1_GBA_WIDTH) {
        win0_right = MODE1_GBA_WIDTH;
    }
    if (win0_bottom > MODE1_GBA_HEIGHT) {
        win0_bottom = MODE1_GBA_HEIGHT;
    }
    if (win1_right > MODE1_GBA_WIDTH) {
        win1_right = MODE1_GBA_WIDTH;
    }
    if (win1_bottom > MODE1_GBA_HEIGHT) {
        win1_bottom = MODE1_GBA_HEIGHT;
    }

    win0_h_wrap = win0_left > win0_right;
    win0_v_active = win0_on && win0_top <= win0_bottom && line >= win0_top && line < win0_bottom;
    win1_h_wrap = win1_left > win1_right;
    win1_v_active = win1_on && win1_top <= win1_bottom && line >= win1_top && line < win1_bottom;

    for (i = 0; i < MODE1_GBA_BG_COUNT; ++i) {
        bg_order_priority[i] = (uint8_t)(virtuappu_mode1_io_read16((uint16_t)(MODE1_IO_BG0CNT + i * 2)) & 3u);
    }

    for (i = 0; i < MODE1_GBA_BG_COUNT - 1; ++i) {
        int j;
        for (j = i + 1; j < MODE1_GBA_BG_COUNT; ++j) {
            if (bg_order_priority[bg_order[j]] < bg_order_priority[bg_order[i]]) {
                uint8_t tmp = bg_order[i];
                bg_order[i] = bg_order[j];
                bg_order[j] = tmp;
            }
        }
    }

    for (x = 0; x < MODE1_GBA_WIDTH; ++x) {
        uint8_t win_ctrl = 0x3Fu;
        bool visible_bg[MODE1_GBA_BG_COUNT];
        bool visible_obj;
        bool allow_sfx;
        uint32_t top_color = backdrop_color;
        int top_layer = 5;
        uint32_t bottom_color = backdrop_color;
        int bottom_layer = 5;
        bool found_top = false;
        bool found_bottom = false;
        int priority;

        if (any_window) {
            win_ctrl = outside_ctrl;
            if (win1_v_active) {
                bool in_h = win1_h_wrap ? (x >= win1_left || x < win1_right) : (x >= win1_left && x < win1_right);
                if (in_h) {
                    win_ctrl = win1_ctrl;
                }
            }
            if (win0_v_active) {
                bool in_h = win0_h_wrap ? (x >= win0_left || x < win0_right) : (x >= win0_left && x < win0_right);
                if (in_h) {
                    win_ctrl = win0_ctrl;
                }
            }
        }

        visible_bg[0] = (win_ctrl & 0x01u) != 0u;
        visible_bg[1] = (win_ctrl & 0x02u) != 0u;
        visible_bg[2] = (win_ctrl & 0x04u) != 0u;
        visible_bg[3] = (win_ctrl & 0x08u) != 0u;
        visible_obj = (win_ctrl & 0x10u) != 0u;
        allow_sfx = (win_ctrl & 0x20u) != 0u;

        for (priority = 0; priority <= 3 && !found_bottom; ++priority) {
            int order_index;

            if (obj_enabled && visible_obj && obj_layer[x] != 0u && obj_priority[x] == priority) {
                if (!found_top) {
                    top_color = obj_layer[x];
                    top_layer = 4;
                    found_top = true;
                } else if (!found_bottom) {
                    bottom_color = obj_layer[x];
                    bottom_layer = 4;
                    found_bottom = true;
                }
            }

            for (order_index = 0; order_index < MODE1_GBA_BG_COUNT; ++order_index) {
                int bg = bg_order[order_index];
                if (!bg_enabled[bg] || !visible_bg[bg]) {
                    continue;
                }
                if (bg_order_priority[bg] != priority) {
                    continue;
                }
                if (bg_layers[bg][x] == 0u) {
                    continue;
                }

                if (!found_top) {
                    top_color = bg_layers[bg][x];
                    top_layer = bg;
                    found_top = true;
                } else if (!found_bottom) {
                    bottom_color = bg_layers[bg][x];
                    bottom_layer = bg;
                    found_bottom = true;
                    break;
                }
            }
        }

        if (allow_sfx) {
            switch (effect) {
            case MODE1_BLEND_ALPHA:
                if (mode1_is_first_target(bldcnt, top_layer) && mode1_is_second_target(bldcnt, bottom_layer)) {
                    top_color = mode1_alpha_blend(top_color, bottom_color, eva, evb);
                }
                break;
            case MODE1_BLEND_BRIGHTEN:
                if (mode1_is_first_target(bldcnt, top_layer)) {
                    top_color = mode1_brighten(top_color, evy);
                }
                break;
            case MODE1_BLEND_DARKEN:
                if (mode1_is_first_target(bldcnt, top_layer)) {
                    top_color = mode1_darken(top_color, evy);
                }
                break;
            default:
                break;
            }
        }

        virtuappu_frame_buffer[(size_t)line * MODE1_GBA_WIDTH + (size_t)x] = top_color;
    }
}

/* Sub-pixel overlay for OAM affine sprites at internal-render-scale > 1.
 *
 * Called by the PC port AFTER the standard 240x160 render has already been
 * S*S nearest-replicated into `dst` (a 240*S by 160*S buffer). For each
 * affine OAM entry we re-run the matrix at sub-pixel density and write
 * straight to `dst`, which produces visibly smoother diagonals on rotated
 * sprites (Vaati's tornado, screen-shrink cinematic, every spinning enemy).
 *
 * Caveats:
 *   * No priority/blend layering — affine pixels overwrite whatever's at
 *     the target. Acceptable for a first pass because the affine sprite
 *     was already top-of-stack at scale 1; in TMC there are very few
 *     scenes where a non-affine sprite occludes an affine one.
 *   * Source texture is still 1x pixel art — no information to "recover".
 *     What you gain is sub-pixel sampling at the screen-space rotation,
 *     which trades the 240-grid staircase for an S*240-grid one.
 */
void virtuappu_mode1_render_affine_obj_overlay(uint32_t *dst, int dst_w, int dst_h, int scale)
{
    if (dst == NULL || scale <= 1) {
        return;
    }
    if (dst_w != MODE1_GBA_WIDTH * scale || dst_h != MODE1_GBA_HEIGHT * scale) {
        return;
    }

    uint16_t dispcnt = virtuappu_mode1_io_read16(MODE1_IO_DISPCNT);
    if ((dispcnt & MODE1_DISP_FORCED_BLANK) != 0u) {
        return;
    }
    if ((dispcnt & MODE1_DISP_OBJ_ON) == 0u) {
        return;
    }
    bool obj_1d = (dispcnt & MODE1_DISP_OBJ_1D) != 0u;
    const uint32_t obj_tile_base = 0x10000u;

    /* OAM is iterated lowest-priority-first so higher-priority entries
     * overwrite — same convention as virtuappu_mode1_render_obj_line, just
     * applied at sub-pixel resolution. */
    for (int i = MODE1_GBA_OAM_COUNT - 1; i >= 0; --i) {
        Mode1OAMAttr attr;
        attr.attr0 = mode1_memory.oam_mem[i * 4];
        attr.attr1 = mode1_memory.oam_mem[i * 4 + 1];
        attr.attr2 = mode1_memory.oam_mem[i * 4 + 2];

        if (!mode1_oam_affine(attr)) continue;
        if (mode1_oam_hidden(attr))  continue;

        uint8_t shape = mode1_oam_shape(attr);
        uint8_t size  = mode1_oam_size(attr);
        int obj_width  = mode1_obj_widths [shape][size];
        int obj_height = mode1_obj_heights[shape][size];
        int bounds_width  = obj_width;
        int bounds_height = obj_height;
        if (mode1_oam_double_size(attr)) {
            bounds_width  *= 2;
            bounds_height *= 2;
        }

        int obj_y = mode1_obj_y(i, attr);
        int obj_x = mode1_oam_x(attr);
        if (obj_x >= MODE1_GBA_WIDTH)  obj_x -= 512;

        bool bpp8 = mode1_oam_bpp8(attr);
        uint16_t base_tile = mode1_oam_tile_index(attr);
        int tiles_w = obj_width / 8;

        int affine_group = mode1_oam_affine_index(attr);
        int16_t pa = (int16_t)mode1_memory.oam_mem[affine_group * 16 + 3];
        int16_t pb = (int16_t)mode1_memory.oam_mem[affine_group * 16 + 7];
        int16_t pc = (int16_t)mode1_memory.oam_mem[affine_group * 16 + 11];
        int16_t pd = (int16_t)mode1_memory.oam_mem[affine_group * 16 + 15];

        int half_width        = bounds_width  / 2;
        int half_height       = bounds_height / 2;
        int sprite_half_w     = obj_width  / 2;
        int sprite_half_h     = obj_height / 2;

        /* Iterate output sub-pixels. (sx_sub, sy_sub) are in 1/scale source-
         * pixel units; the matrix math at scale=1 reproduces the integer
         * mode1_render_obj_line behaviour exactly. */
        for (int sy_sub = 0; sy_sub < bounds_height * scale; ++sy_sub) {
            /* line_sub = output row = sprite-top output row + sub-row within sprite. */
            int line_sub = obj_y * scale + sy_sub;
            if (line_sub < 0 || line_sub >= MODE1_GBA_HEIGHT * scale) continue;

            /* input_rel_y in (1/scale) source-pixel units. */
            int input_rel_y_subS = sy_sub - half_height * scale;

            for (int sx_sub = 0; sx_sub < bounds_width * scale; ++sx_sub) {
                int screen_x_sub = obj_x * scale + sx_sub;
                if (screen_x_sub < 0 || screen_x_sub >= MODE1_GBA_WIDTH * scale) continue;

                int input_rel_x_subS = sx_sub - half_width * scale;
                /* tex_x / tex_y in source-pixel units. pa is 8.8 fixed,
                 * input_rel_*_subS is 1/scale source-pixels, so the product
                 * is (8.8 / scale) source-pixel units. >>8 trims the fixed
                 * fraction; / scale converts back to integer source-pixels. */
                int tex_x = (((int)pa * input_rel_x_subS + (int)pb * input_rel_y_subS) >> 8) / scale + sprite_half_w;
                int tex_y = (((int)pc * input_rel_x_subS + (int)pd * input_rel_y_subS) >> 8) / scale + sprite_half_h;
                if (tex_x < 0 || tex_x >= obj_width)  continue;
                if (tex_y < 0 || tex_y >= obj_height) continue;

                int tile_row = tex_y / 8;
                int pixel_y  = tex_y % 8;
                int tile_col = tex_x / 8;
                int pixel_x  = tex_x % 8;

                uint16_t tile_index;
                if (obj_1d) {
                    tile_index = (uint16_t)(base_tile + tile_row * tiles_w + tile_col);
                    if (bpp8) tile_index = (uint16_t)(base_tile + (tile_row * tiles_w + tile_col) * 2);
                } else {
                    tile_index = (uint16_t)(base_tile + tile_row * 32 + tile_col);
                    if (bpp8) tile_index = (uint16_t)(base_tile + tile_row * 32 + tile_col * 2);
                }

                uint8_t color_index;
                if (bpp8) {
                    uint32_t addr = obj_tile_base + (uint32_t)tile_index * 32u + (uint32_t)pixel_y * 8u + (uint32_t)pixel_x;
                    color_index = (addr < MODE1_VRAM_SIZE) ? mode1_memory.vram[addr] : 0u;
                } else {
                    uint32_t addr = obj_tile_base + (uint32_t)tile_index * 32u + (uint32_t)pixel_y * 4u + (uint32_t)(pixel_x / 2);
                    uint8_t packed = (addr < MODE1_VRAM_SIZE) ? mode1_memory.vram[addr] : 0u;
                    color_index = (pixel_x & 1) ? (packed >> 4u) : (packed & 0x0Fu);
                }
                if (color_index == 0u) continue;

                uint16_t rgb555;
                if (bpp8) {
                    rgb555 = mode1_memory.obj_palette[color_index];
                } else {
                    rgb555 = mode1_memory.obj_palette[(size_t)mode1_oam_palette(attr) * 16u + color_index];
                }

                dst[(size_t)line_sub * (size_t)dst_w + (size_t)screen_x_sub] =
                    virtuappu_mode1_rgb555_to_abgr8888(rgb555);
            }
        }
    }
}
void virtuappu_mode1_render_frame(const PPUMemory *ppu)
{
    uint16_t dispcnt;
    int line;

    (void)ppu;

    dispcnt = virtuappu_mode1_io_read16(MODE1_IO_DISPCNT);
    if ((dispcnt & MODE1_DISP_FORCED_BLANK) != 0u) {
        memset(virtuappu_frame_buffer, 0xFF, MODE1_GBA_WIDTH * MODE1_GBA_HEIGHT * sizeof(uint32_t));
        return;
    }

    for (line = 0; line < MODE1_GBA_HEIGHT; ++line) {
        if (virtuappu_mode1_pre_line_callback != NULL) {
            virtuappu_mode1_pre_line_callback(line);
        }
        uint32_t bg_layers[MODE1_GBA_BG_COUNT][MODE1_GBA_WIDTH];
        uint8_t bg_priority[MODE1_GBA_BG_COUNT][MODE1_GBA_WIDTH];
        uint32_t obj_layer[MODE1_GBA_WIDTH];
        uint8_t obj_priority[MODE1_GBA_WIDTH];
        bool obj_1d = (dispcnt & MODE1_DISP_OBJ_1D) != 0u;

        memset(bg_layers, 0, sizeof(bg_layers));
        memset(bg_priority, 0, sizeof(bg_priority));
        memset(obj_layer, 0, sizeof(obj_layer));
        memset(obj_priority, 0xFF, sizeof(obj_priority));

        if ((dispcnt & MODE1_DISP_BG0_ON) != 0u) {
            virtuappu_mode1_render_text_bg_line(0, line, bg_layers[0], bg_priority[0]);
        }
        if ((dispcnt & MODE1_DISP_BG1_ON) != 0u) {
            virtuappu_mode1_render_text_bg_line(1, line, bg_layers[1], bg_priority[1]);
        }
        if ((dispcnt & MODE1_DISP_BG2_ON) != 0u) {
            virtuappu_mode1_render_text_bg_line(2, line, bg_layers[2], bg_priority[2]);
        }
        if ((dispcnt & MODE1_DISP_BG3_ON) != 0u) {
            virtuappu_mode1_render_text_bg_line(3, line, bg_layers[3], bg_priority[3]);
        }
        if ((dispcnt & MODE1_DISP_OBJ_ON) != 0u) {
            virtuappu_mode1_render_obj_line(line, obj_1d, obj_layer, obj_priority);
        }

        virtuappu_mode1_composite_line(line, bg_layers, bg_priority, obj_layer, obj_priority, dispcnt);
    }
}
