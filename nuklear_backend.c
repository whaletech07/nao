#include <stdint.h>

#define NK_PRIVATE
#include "nuklear.h"

// Nuklear implementation is provided by kernel.c - do not define NK_IMPLEMENTATION here

extern uint32_t* gfx_framebuffer;
extern uint32_t  gfx_width;
extern uint32_t  gfx_height;
extern uint32_t  gfx_pitch;

extern void putpixel(uint32_t* fb, int x, int y, uint32_t color, uint32_t pitch);
extern void idt_draw_rect(int x, int y, int w, int h, uint32_t color);

// Bridging Nuklear's commands to our raw frame buffer
void nk_os_render(struct nk_context *ctx) {
    const struct nk_command *cmd;
    
    nk_foreach(cmd, ctx) {
        switch (cmd->type) {
            case NK_COMMAND_SCISSOR:
                // For now, we can ignore advanced clipping/scissors 
                break;
                
            case NK_COMMAND_RECT: {
                const struct nk_command_rect *r = (const struct nk_command_rect*)cmd;
                uint32_t color = (r->color.r << 16) | (r->color.g << 8) | r->color.b;
                idt_draw_rect(r->x, r->y, r->w, r->h, color);
                break;
            }
            
            case NK_COMMAND_RECT_FILLED: {
                const struct nk_command_rect_filled *r = (const struct nk_command_rect_filled*)cmd;
                uint32_t color = (r->color.r << 16) | (r->color.g << 8) | r->color.b;
                idt_draw_rect(r->x, r->y, r->w, r->h, color);
                break;
            }
            
            case NK_COMMAND_IMAGE: {
                const struct nk_command_image *img_cmd = (const struct nk_command_image*)cmd;
                if (img_cmd->img.handle.ptr) {
                    uint32_t* pixels = (uint32_t*)img_cmd->img.handle.ptr;
                    int src_w = img_cmd->img.w;
                    int src_h = img_cmd->img.h;
                    int dst_w = (int)img_cmd->w;
                    int dst_h = (int)img_cmd->h;
                    
                    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) break;
                    
                    // Nearest-neighbor scaling: map each destination pixel to source with rounding
                    for (int y = 0; y < dst_h; y++) {
                        int src_y = (y * src_h) / dst_h;
                        if (src_y >= src_h) src_y = src_h - 1;
                        
                        for (int x = 0; x < dst_w; x++) {
                            int src_x = (x * src_w) / dst_w;
                            if (src_x >= src_w) src_x = src_w - 1;
                            
                            uint32_t pixel = pixels[src_y * src_w + src_x];
                            int dst_x = (int)(img_cmd->x + x);
                            int dst_y = (int)(img_cmd->y + y);
                            
                            if (dst_x >= 0 && dst_x < (int)gfx_width && dst_y >= 0 && dst_y < (int)gfx_height) {
                                putpixel(gfx_framebuffer, dst_x, dst_y, pixel, gfx_pitch);
                            }
                        }
                    }
                }
                break;
            }

            // We will expand this to handle text, lines, and circles!
            default:
                break;
        }
    }
    nk_clear(ctx);
}