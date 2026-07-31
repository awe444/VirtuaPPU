#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../ppu_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*virtuappu_mode1_pre_line_fn)(int line);
extern virtuappu_mode1_pre_line_fn virtuappu_mode1_pre_line_callback;

/* Map-source override for text BG layers (non-GBA extension).
 *
 * A GBA text BG reads its tilemap from a 32x32-entry VRAM screenblock and
 * wraps modulo the map size, which caps the visible region at the hardware
 * window regardless of how much world data exists. When a caller already
 * holds a complete, room-sized tilemap it can bind it here instead: the
 * layer then samples `map` directly at an absolute pixel origin, with no
 * screenblock and no wrap, so the viewport can be any width or height the
 * frame buffer allows.
 *
 * Everything downstream of the tilemap fetch — character data, palette,
 * flip bits, priority, mosaic, blending — is unchanged, so with
 * origin_x/origin_y set to the same scroll position the hardware path
 * would produce, output is bit-identical to the screenblock path. That
 * equivalence is the intended way to validate a binding.
 *
 * Tiles outside [0,width_tiles) x [0,height_tiles) render as transparent
 * (backdrop shows through), matching how an undersized room letterboxes.
 *
 * Bindings are per BG index and persist until changed; pass NULL to
 * restore normal screenblock behaviour for that layer. */
typedef struct {
    const uint16_t *map;  /* row-major tilemap entries, GBA text-BG format */
    int stride;           /* entries per row (may exceed width_tiles)      */
    int width_tiles;      /* valid map extent, in 8x8 tiles                */
    int height_tiles;
    int origin_x;         /* pixel coordinate of screen column 0 */
    int origin_y;         /* pixel coordinate of screen line 0   */
} VirtuaPPUMode1MapSource;

void virtuappu_mode1_set_map_source(int bg_index, const VirtuaPPUMode1MapSource *source);
void virtuappu_mode1_clear_map_sources(void);
/* Whether a map source is currently bound for a layer. A layer without one
 * is reading a hardware screenblock and so cannot cover more than 256 px. */
bool virtuappu_mode1_has_map_source(int bg_index);

/* Window bounds override (non-GBA extension).
 *
 * WIN0H/WIN1H pack each edge into 8 bits, so no window can describe an
 * edge past 255 — a hard ceiling for any viewport wider than that. A host
 * rendering wider can push the true bounds here instead; when an override
 * is set for a window, the PPU uses it in place of the packed registers.
 *
 * Semantics otherwise match the hardware exactly, including the
 * left > right / top > bottom wrap-around behaviour, so an override
 * carrying the same values the registers held renders identically.
 *
 * Pass NULL to return a window to register-decoded bounds. */
typedef struct {
    int left;
    int right;
    int top;
    int bottom;
} VirtuaPPUMode1WindowBounds;

void virtuappu_mode1_set_window_bounds(int window_index, const VirtuaPPUMode1WindowBounds *bounds);
void virtuappu_mode1_clear_window_bounds(void);

/* Replace only a window's horizontal pair, keeping the vertical one.
 *
 * Once a host supplies bounds through set_window_bounds, those win over the
 * packed 8-bit WIN0H/WIN1H registers for the rest of the frame — which is
 * correct for a whole-frame window and wrong for one an HBlank DMA rewrites
 * every scanline, because the DMA's writes land in the registers the host
 * bounds are overriding. A host driving a per-scanline window calls this
 * from its pre-line callback so each line's edges reach the raster. */
void virtuappu_mode1_set_window_h_bounds(int window_index, int left, int right);

/* Layer clip + offset (non-GBA extension).
 *
 * A text BG wraps modulo its map size, so content authored for a 256-px
 * map cannot simply be shifted right on a wider display: the wrapped
 * columns reappear on the opposite edge instead of leaving a border. This
 * lets a host say "this layer's content is `content_width` pixels wide and
 * starts at `offset_x`" — inside that span the layer samples as if the
 * screen began at offset_x, and outside it the layer contributes nothing,
 * so the backdrop shows through.
 *
 * `offset_y` / `content_height` are the same statement about rows, for a
 * display taller than the content. The two axes are independent: a host
 * centring a 240x160 surface on a 320x240 display sets both, while one
 * that only wants a layer pinned to the top of a taller display sets
 * offset_y = 0 and content_height = the full frame.
 *
 * Unlike a map source this works with the layer's normal screenblock
 * fetch, which matters for content loaded straight into VRAM rather than
 * staged in a buffer the host could hand over.
 *
 * Pass NULL to remove a clip. */
typedef struct {
    int offset_x;
    int content_width;
    int offset_y;
    int content_height;
} VirtuaPPUMode1BgClip;

void virtuappu_mode1_set_bg_clip(int bg_index, const VirtuaPPUMode1BgClip *clip);
void virtuappu_mode1_clear_bg_clips(void);

/* Global OBJ offset (non-GBA extension).
 *
 * Shifts every sprite by (dx, dy) at composite time. Its purpose is to keep
 * sprites with a shifted BG layer: a host centring 240-wide UI content on a
 * wider display moves the backgrounds, and the sprites drawn on top of them
 * (menu cursors, item icons, the title sword) have to travel the same
 * distance or they detach from what they belong to. Zero restores hardware
 * behaviour. */
void virtuappu_mode1_set_obj_offset(int dx, int dy);

/* OBJ horizontal clip (non-GBA extension).
 *
 * Suppresses sprite pixels outside [left, right). On hardware the screen is
 * exactly the world view, so a sprite is either on it or off it; once the
 * viewport can be wider than the room being shown, the leftover columns are
 * border rather than world, and a sprite standing in them is an object that
 * hardware would simply never have drawn. Defaults to the full frame. */
void virtuappu_mode1_set_obj_clip(int left, int right);

/* The same suppression for rows, for a display taller than the content.
 * Independent of the horizontal pair; defaults to the full frame. */
void virtuappu_mode1_set_obj_clip_v(int top, int bottom);

/* Rendered viewport. GBA-native by default; a host that drives the PPU
 * with non-hardware BG sources (see VirtuaPPUMode1MapSource) can override
 * these at build time to render a larger area. Must not exceed
 * VIRTUAPPU_MAX_FRAME_WIDTH/HEIGHT. */
#ifndef MODE1_GBA_WIDTH
#define MODE1_GBA_WIDTH 240
#endif
#ifndef MODE1_GBA_HEIGHT
#define MODE1_GBA_HEIGHT 160
#endif

enum {
    MODE1_GBA_BG_COUNT = 4,
    MODE1_GBA_OAM_COUNT = 128,
    MODE1_IO_MEM_SIZE = 0x400,
    MODE1_VRAM_SIZE = 0x18000,
    MODE1_PALETTE_COLORS = 256,
    MODE1_OAM_HALFWORDS = 512
};

enum {
    MODE1_IO_DISPCNT = 0x00,
    MODE1_IO_BG0CNT = 0x08,
    MODE1_IO_BG1CNT = 0x0A,
    MODE1_IO_BG2CNT = 0x0C,
    MODE1_IO_BG3CNT = 0x0E,
    MODE1_IO_BG0HOFS = 0x10,
    MODE1_IO_BG0VOFS = 0x12,
    MODE1_IO_BG1HOFS = 0x14,
    MODE1_IO_BG1VOFS = 0x16,
    MODE1_IO_BG2HOFS = 0x18,
    MODE1_IO_BG2VOFS = 0x1A,
    MODE1_IO_BG3HOFS = 0x1C,
    MODE1_IO_BG3VOFS = 0x1E,
    MODE1_IO_WIN0H = 0x40,
    MODE1_IO_WIN1H = 0x42,
    MODE1_IO_WIN0V = 0x44,
    MODE1_IO_WIN1V = 0x46,
    MODE1_IO_WININ = 0x48,
    MODE1_IO_WINOUT = 0x4A,
    MODE1_IO_MOSAIC = 0x4C,
    MODE1_IO_BLDCNT = 0x50,
    MODE1_IO_BLDALPHA = 0x52,
    MODE1_IO_BLDY = 0x54
};

enum {
    MODE1_DISP_OBJ_1D = 0x0040,
    MODE1_DISP_FORCED_BLANK = 0x0080,
    MODE1_DISP_BG0_ON = 0x0100,
    MODE1_DISP_BG1_ON = 0x0200,
    MODE1_DISP_BG2_ON = 0x0400,
    MODE1_DISP_BG3_ON = 0x0800,
    MODE1_DISP_OBJ_ON = 0x1000,
    MODE1_DISP_WIN0_ON = 0x2000,
    MODE1_DISP_WIN1_ON = 0x4000,
    MODE1_DISP_OBJWIN_ON = 0x8000
};

typedef struct VirtuaPPUMode1GbaMemory {
    uint8_t *io_mem;
    uint8_t *vram;
    uint16_t *bg_palette;
    uint16_t *obj_palette;
    uint16_t *oam_mem;
    /* Optional per-slot untruncated OBJ y, 128 entries, parallel to oam_mem.
     * attr0 holds y in 8 bits and hardware recovers "above the top edge" by
     * wrapping mod 256; an emulator reads values >= the screen height as
     * negative, which needs the band [height,255] to be wider than the most
     * negative y a sprite can take. That holds at 160 lines (96 px of band)
     * and fails at 240 (16 px). A host that positions its own sprites can
     * supply the real y here and the wrap heuristic is bypassed.
     * NULL — the default — keeps the pure-hardware interpretation. */
    const int16_t *oam_y_ext;
} VirtuaPPUMode1GbaMemory;

void virtuappu_mode1_bind_gba_memory(const VirtuaPPUMode1GbaMemory *memory);
void virtuappu_mode1_get_bound_gba_memory(VirtuaPPUMode1GbaMemory *memory);
uint16_t virtuappu_mode1_io_read16(uint16_t offset);
uint32_t virtuappu_mode1_io_read32(uint16_t offset);
uint32_t virtuappu_mode1_rgb555_to_abgr8888(uint16_t color);
void virtuappu_mode1_render_text_bg_line(int bg_index, int line, uint32_t *line_buffer, uint8_t *priority_buffer);
void virtuappu_mode1_render_obj_line(int line, bool obj_1d, uint32_t *line_buffer, uint8_t *priority_buffer);
void virtuappu_mode1_composite_line(
    int line,
    uint32_t bg_layers[MODE1_GBA_BG_COUNT][MODE1_GBA_WIDTH],
    uint8_t bg_priority[MODE1_GBA_BG_COUNT][MODE1_GBA_WIDTH],
    uint32_t obj_layer[MODE1_GBA_WIDTH],
    uint8_t obj_priority[MODE1_GBA_WIDTH],
    uint16_t dispcnt);
void virtuappu_mode1_render_frame(const PPUMemory *ppu);
/* Sub-pixel re-render of OAM affine sprites into a (240*scale x 160*scale)
 * buffer. Called by the PC port at internal-render-scale > 1 after the
 * standard frame has been S*S nearest-replicated into `dst`. */
void virtuappu_mode1_render_affine_obj_overlay(uint32_t *dst, int dst_w, int dst_h, int scale);

#ifdef __cplusplus
}
#endif
