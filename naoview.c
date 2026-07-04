// ============================================================================
// NAOVIEW MODULE: DYNAMIC IMAGE VIEWER WITH PROMPT FUNCTIONALITY
// ============================================================================
#include <stdint.h>
#include <string.h>
#include "ff.h" // For FIL, f_open, etc.

// Define Nuklear configuration exactly like kernel.c
#define NK_PRIVATE
#include "nuklear.h"

// Bring in variables and utilities owned by the kernel frame loop
extern int mouse_left_clicked;
extern void* kmalloc(uint32_t size);
extern void kfree(void* ptr);
extern void serial_printf(const char* format, ...);
extern int mini_snprintf(char *buf, size_t buffer_size, const char *format, ...);

// Reset helper tracking from kernel
extern size_t stb_arena_offset;
extern void* stbi_malloc_arena(size_t size);
extern void stbi_set_flip_vertically_on_load(int flag);
extern unsigned char* stbi_load_from_memory(unsigned char const *buffer, int len, int *x, int *y, int *channels_in_file, int desired_channels);
// Add these to the top of naoview.c


// ============================================================================
// NAOVIEW MODULE: DYNAMIC IMAGE VIEWER WITH STATUS ALERTS
// ============================================================================

int naoview_active = 0;
int naoview_minimized = 0;
static int naoview_show_open_dialog = 0;

// Dialog warning alert states
typedef enum {
    NAOVIEW_STATUS_OK = 0,
    NAOVIEW_STATUS_ERR_NOT_FOUND,
    NAOVIEW_STATUS_ERR_BAD_FORMAT
} naoview_status_t;

static naoview_status_t naoview_alert = NAOVIEW_STATUS_OK;

char naoview_current_file[128] = "0:/";
uint32_t* naoview_pixels = NULL;
int naoview_width = 0;
int naoview_height = 0;
struct nk_image naoview_img;

void naoview_close_image(void) {
    if (naoview_pixels) {
        kfree(naoview_pixels);
        naoview_pixels = NULL;
    }
    naoview_width = 0;
    naoview_height = 0;
    naoview_img.handle.ptr = NULL;
}

int naoview_load_image(const char* path) {
    naoview_close_image();
    naoview_alert = NAOVIEW_STATUS_OK; // Reset errors
    
    FIL file;
    UINT br;
    stb_arena_offset = 0; 

    if (f_open(&file, path, FA_READ) != FR_OK) {
        serial_printf("[NaoView Error] Failed to open file: %s\n", path);
        naoview_alert = NAOVIEW_STATUS_ERR_NOT_FOUND;
        return 0;
    }

    DWORD file_size = f_size(&file);
    uint8_t* file_scratch_buffer = (uint8_t*)stbi_malloc_arena(file_size);
    if (!file_scratch_buffer) {
        f_close(&file);
        naoview_alert = NAOVIEW_STATUS_ERR_BAD_FORMAT;
        return 0;
    }

    if (f_read(&file, file_scratch_buffer, file_size, &br) != FR_OK || br != file_size) {
        f_close(&file);
        naoview_alert = NAOVIEW_STATUS_ERR_BAD_FORMAT;
        return 0;
    }
    f_close(&file);

    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(1);
    
    unsigned char* decoded_pixels = stbi_load_from_memory(
        file_scratch_buffer, file_size, &width, &height, &channels, 4
    );

    if (!decoded_pixels) {
        serial_printf("[NaoView Error] stb decoding failed.\n");
        stbi_set_flip_vertically_on_load(0);
        naoview_alert = NAOVIEW_STATUS_ERR_BAD_FORMAT;
        return 0;
    }

    naoview_pixels = (uint32_t*)kmalloc(width * height * 4);
    if (!naoview_pixels) {
        stbi_set_flip_vertically_on_load(0);
        return 0;
    }

    naoview_width = width;
    naoview_height = height;
    stbi_set_flip_vertically_on_load(0);

    for (int y = 0; y < height; y++) {
        int target_y = (height - 1) - y;
        for (int x = 0; x < width; x++) {
            int src_idx = (y * width + x) * 4;
            int dest_idx = target_y * width + x;

            uint8_t r = decoded_pixels[src_idx + 0];
            uint8_t g = decoded_pixels[src_idx + 1];
            uint8_t b = decoded_pixels[src_idx + 2];
            uint8_t a = decoded_pixels[src_idx + 3];

            naoview_pixels[dest_idx] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    naoview_img = nk_image_ptr(naoview_pixels);
    naoview_img.w = width;
    naoview_img.h = height;
    
    return 1;
}

void naoview_open_file(const char* path) {
    naoview_active = 1;
    naoview_minimized = 0;
    strncpy(naoview_current_file, path, sizeof(naoview_current_file) - 1);
    naoview_load_image(naoview_current_file);
}

void render_naoview(struct nk_context* ctx, int* active_drag_window_id) {
    if (!naoview_active || naoview_minimized) return;

    if (nk_begin(ctx, "NaoView - Image Viewer", nk_rect(220, 120, 480, 420),
        NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) 
    {
        // 1. Menu Bar Layout
        nk_menubar_begin(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 25, 1);
        nk_layout_row_push(ctx, 45);
        if (nk_menu_begin_label(ctx, "File", NK_TEXT_LEFT, nk_vec2(120, 200))) {
            nk_layout_row_dynamic(ctx, 25, 1);
            if (nk_menu_item_label(ctx, "Open PNG...", NK_TEXT_LEFT)) {
                naoview_show_open_dialog = 1;
                naoview_alert = NAOVIEW_STATUS_OK; 
            }
            if (nk_menu_item_label(ctx, "Close Image", NK_TEXT_LEFT)) {
                naoview_close_image();
                strncpy(naoview_current_file, "0:/", sizeof(naoview_current_file) - 1);
                naoview_alert = NAOVIEW_STATUS_OK;
            }
            nk_menu_end(ctx);
        }
        nk_layout_row_end(ctx);
        nk_menubar_end(ctx);

        // 2. Main Window Baseline Render (The Image Canvas)
        if (naoview_pixels && naoview_img.handle.ptr) {
            nk_layout_row_dynamic(ctx, 20, 1);
            char info_text[140];
            mini_snprintf(info_text, sizeof(info_text), "Source: %s [%dx%d]", naoview_current_file, naoview_width, naoview_height);
            nk_label(ctx, info_text, NK_TEXT_LEFT);

            // Use the current window height so the image panel fills the available
            // space and the vertical scrollbar reaches the bottom of the window.
            struct nk_vec2 window_size = nk_window_get_size(ctx);
            int image_view_height = (int)window_size.y - 80;
            if (image_view_height < 120) image_view_height = 120;
            nk_layout_row_dynamic(ctx, (float)image_view_height, 1);
            if (nk_group_begin(ctx, "NaoViewImageGroup", NK_WINDOW_SCROLL_AUTO_HIDE)) {
                nk_layout_row_static(ctx, (float)naoview_height, naoview_width, 1);
                nk_image(ctx, naoview_img);
                nk_group_end(ctx);
            }
        } else {
            nk_layout_row_dynamic(ctx, 100, 1);
            nk_label(ctx, "No picture loaded. Go to File > Open PNG...", NK_TEXT_CENTERED);
        }

        // ==========================================================
        // POP-UP DIALOG 1: FILE OPEN PROMPT
        // ==========================================================
        if (naoview_show_open_dialog) {
            // Generates a sub-window popup overlay relative to parent dimensions
            struct nk_rect popup_bounds = nk_rect(40, 80, 400, 150);
            if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Open File", NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE, popup_bounds)) {
                
                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, "Enter Target PNG Path:", NK_TEXT_LEFT);
                
                nk_layout_row_dynamic(ctx, 30, 1);
                nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, naoview_current_file, sizeof(naoview_current_file) - 1, nk_filter_default);
                
                nk_layout_row_dynamic(ctx, 30, 2);
                if (nk_button_label(ctx, "Load")) {
                    naoview_load_image(naoview_current_file);
                    // Automatically dismisses file window popup if no explicit error occurred
                    if (naoview_alert == NAOVIEW_STATUS_OK) {
                        naoview_show_open_dialog = 0;
                        nk_popup_close(ctx);
                    }
                }
                if (nk_button_label(ctx, "Cancel")) {
                    naoview_show_open_dialog = 0;
                    nk_popup_close(ctx);
                }
                nk_popup_end(ctx);
            } else {
                naoview_show_open_dialog = 0;
            }
        }

        // ==========================================================
        // POP-UP DIALOG 2: WARNING/ERROR ALERTS
        // ==========================================================
        if (naoview_alert != NAOVIEW_STATUS_OK) {
            struct nk_rect alert_bounds = nk_rect(60, 100, 360, 160);
            if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Application Alert", NK_WINDOW_BORDER | NK_WINDOW_TITLE, alert_bounds)) {
                
                nk_layout_row_dynamic(ctx, 20, 1);
                if (naoview_alert == NAOVIEW_STATUS_ERR_NOT_FOUND) {
                    nk_label(ctx, "[!] Error: File path not found!", NK_TEXT_LEFT);
                } else if (naoview_alert == NAOVIEW_STATUS_ERR_BAD_FORMAT) {
                    nk_label(ctx, "[!] Error: Invalid or corrupt PNG format.", NK_TEXT_LEFT);
                }

                nk_layout_row_dynamic(ctx, 20, 1);
                char target_lbl[160];
                mini_snprintf(target_lbl, sizeof(target_lbl), "Path: %s", naoview_current_file);
                nk_label(ctx, target_lbl, NK_TEXT_LEFT);

                nk_layout_row_dynamic(ctx, 30, 3);
                nk_spacing(ctx, 1);
                if (nk_button_label(ctx, "OK")) {
                    naoview_alert = NAOVIEW_STATUS_OK; // Dismiss error state
                    nk_popup_close(ctx);
                }
                nk_spacing(ctx, 1);
                
                nk_popup_end(ctx);
            } else {
                naoview_alert = NAOVIEW_STATUS_OK;
            }
        }
    }
    
    // Process window focusing structures
    if (nk_window_has_focus(ctx) || *active_drag_window_id == 5) {
        if (mouse_left_clicked) *active_drag_window_id = 5;
    } else if (nk_window_is_hovered(ctx) && mouse_left_clicked && *active_drag_window_id == 0) {
        if (!nk_item_is_any_active(ctx)) { *active_drag_window_id = 5; nk_window_set_focus(ctx, "NaoView - Image Viewer"); }
    }
    
    nk_end(ctx);
    if (nk_window_is_hidden(ctx, "NaoView - Image Viewer")) {
        naoview_active = 0;
        naoview_close_image();
    }
}