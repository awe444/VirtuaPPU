#include "cpu/mode2.h"

#include <string.h>

#include "cpu/mode1.h"
#include "virtuappu.h"

void virtuappu_mode2_render_frame(const PPUMemory *ppu)
{
    static const int affine_sizes[4] = {128, 256, 512, 1024};
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
        VirtuaPPUMode1GbaMemory memory;
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
            uint16_t bgcnt;
            uint8_t bg_priority_value;
            uint32_t char_base;
            uint32_t screen_base;
            bool wrap;
            uint16_t size_flag;
            int map_size;
            int map_tiles;
            int16_t pa;
            int16_t pb;
            int16_t pc;
            int16_t pd;
            int32_t ref_x;
            int32_t ref_y;
            int x;

            virtuappu_mode1_get_bound_gba_memory(&memory);
            bgcnt = virtuappu_mode1_io_read16(MODE1_IO_BG2CNT);
            bg_priority_value = (uint8_t)(bgcnt & 3u);
            char_base = (uint32_t)((bgcnt >> 2u) & 3u) * 0x4000u;
            screen_base = (uint32_t)((bgcnt >> 8u) & 0x1Fu) * 0x800u;
            wrap = ((bgcnt >> 13u) & 1u) != 0u;
            size_flag = (uint16_t)((bgcnt >> 14u) & 3u);
            map_size = affine_sizes[size_flag];
            map_tiles = map_size / 8;
            pa = (int16_t)virtuappu_mode1_io_read16(0x20u);
            pb = (int16_t)virtuappu_mode1_io_read16(0x22u);
            pc = (int16_t)virtuappu_mode1_io_read16(0x24u);
            pd = (int16_t)virtuappu_mode1_io_read16(0x26u);
            ref_x = (int32_t)virtuappu_mode1_io_read32(0x28u);
            ref_y = (int32_t)virtuappu_mode1_io_read32(0x2Cu);

            if ((ref_x & 0x08000000u) != 0u) {
                ref_x |= (int32_t)0xF0000000u;
            }
            if ((ref_y & 0x08000000u) != 0u) {
                ref_y |= (int32_t)0xF0000000u;
            }

            /* Honour the same clip the text path honours.
             *
             * An affine layer is as much a 240x160-authored surface as a text
             * one — on the title screen it carries the sword, while the rest
             * of the artwork is on BG0/BG1 and is clipped and centred. Left
             * out, the sword stays where a 240x160 screen would have put it
             * while everything around it moves, and its off-map edges wrap
             * into the border.
             *
             * Shifting an affine image is not a pixel shift: what moves is
             * the texture coordinate the transform starts from. Sampling at
             * screen (x - offset_x, line - offset_y) instead of (x, line) is
             * the whole of it, and it stays correct under rotation and scale
             * because the transform is applied to the shifted coordinate
             * rather than to the result. */
            {
                const VirtuaPPUMode1BgClip *clip = virtuappu_mode1_get_bg_clip(2);
                int clip_left = 0;
                int clip_right = MODE1_GBA_WIDTH;
                int rel_line = line;

                if (clip != NULL) {
                    rel_line = line - clip->offset_y;
                    if (rel_line < 0 || rel_line >= clip->content_height) {
                        goto affine_done; /* whole line outside: leave backdrop */
                    }
                    clip_left = clip->offset_x;
                    clip_right = clip->offset_x + clip->content_width;
                    if (clip_left < 0) clip_left = 0;
                    if (clip_right > MODE1_GBA_WIDTH) clip_right = MODE1_GBA_WIDTH;
                }

            for (x = clip_left; x < clip_right; ++x) {
                int rel_x = (clip != NULL) ? (x - clip->offset_x) : x;
                int32_t tex_x = ref_x + pb * rel_line + pa * rel_x;
                int32_t tex_y = ref_y + pd * rel_line + pc * rel_x;
                int32_t src_x = tex_x >> 8;
                int32_t src_y = tex_y >> 8;
                int tile_col;
                int tile_row;
                int pixel_x;
                int pixel_y;
                uint32_t map_addr;
                uint8_t tile_index;
                uint32_t tile_addr;
                uint8_t color_index;

                if (wrap) {
                    src_x = ((src_x % map_size) + map_size) % map_size;
                    src_y = ((src_y % map_size) + map_size) % map_size;
                } else if (src_x < 0 || src_x >= map_size || src_y < 0 || src_y >= map_size) {
                    continue;
                }

                tile_col = src_x / 8;
                tile_row = src_y / 8;
                pixel_x = src_x % 8;
                pixel_y = src_y % 8;
                map_addr = screen_base + (uint32_t)(tile_row * map_tiles + tile_col);
                tile_index = (map_addr < MODE1_VRAM_SIZE) ? memory.vram[map_addr] : 0u;
                tile_addr = char_base + (uint32_t)tile_index * 64u + (uint32_t)pixel_y * 8u + (uint32_t)pixel_x;
                color_index = (tile_addr < MODE1_VRAM_SIZE) ? memory.vram[tile_addr] : 0u;

                if (color_index == 0u) {
                    continue;
                }

                bg_layers[2][x] = virtuappu_mode1_rgb555_to_abgr8888(memory.bg_palette[color_index]);
                bg_priority[2][x] = bg_priority_value;
            }
            }
        affine_done:;
        }

        if ((dispcnt & MODE1_DISP_OBJ_ON) != 0u) {
            virtuappu_mode1_render_obj_line(line, obj_1d, obj_layer, obj_priority);
        }

        virtuappu_mode1_composite_line(line, bg_layers, bg_priority, obj_layer, obj_priority, dispcnt);
    }
}
