#include <stdint.h>
#include <stdarg.h>
#include "font.h"
#include "ff.h"
#include "boot_screen.h"
#include "lwip/dhcp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "netif/etharp.h"
#include "lwip/ip.h"
#include "lwip/icmp.h"
#include "lwip/raw.h"
#include "lwip/inet_chksum.h" // <-- CRITICAL FIX 1: Resolves inet_chksum warning
// Forward declarations for Nuklear structures and hooks
#define NK_PRIVATE
#include "nuklear.h"

// Global declarations to prevent scope compilation issues
extern void render_naoview(struct nk_context* ctx, int* active_drag_window_id);
extern void netif_driver_poll(struct netif *netif);

extern char keyboard_pop_char(void);
extern uint32_t* gfx_framebuffer;
extern uint32_t  gfx_width;
extern uint32_t  gfx_height;
extern uint32_t  gfx_pitch;
extern int naoedit_active;
extern int naoedit_minimized;
extern void render_naoedit(struct nk_context* ctx, int* active_drag_window_id);
extern void init_serial(void);
extern void serial_printf(const char* format, ...);
extern void init_gdt(void);
extern void init_idt(void);
extern void load_system_cursor(const char* path);
extern void draw_custom_cursor(void);
extern void ani_init(void);
extern int ani_load(const char* path);
extern void ani_play(void);
extern void ani_stop(void);
extern int ani_update(uint32_t current_tick);
extern void ani_draw(int mouse_x, int mouse_y);
extern int ani_is_active(void);
extern int init_rtl8139(void);
extern uint8_t mac_address[6];
struct netif my_netif;
static int start_menu_open = 0;
static int system_shutdown = 0;
// Add these to the top of kernel.c (alongside other extern lines)
extern int naoview_active;
extern int naoview_minimized;
extern void restart_system(void);
// Application states
static int sys_monitor_active = 0;    
static int sys_monitor_minimized = 0; 
static int color_mixer_active = 0;
static int color_mixer_minimized = 0;

// Non-blocking app launch animation state machine
#define APP_LAUNCH_NONE 0
#define APP_LAUNCH_SYS_MONITOR 1
#define APP_LAUNCH_THEME_MIXER 2
#define APP_LAUNCH_FILE_EXPLORER 3
#define APP_LAUNCH_NAOEDIT 4
#define APP_LAUNCH_NAOVIEW 5
static int pending_app_launch = APP_LAUNCH_NONE;
static uint32_t pending_app_start_tick = 0;
#define ANIMATION_DELAY_TICKS 36  // ~2 seconds at 18 ticks/sec
extern int file_explorer_active;
extern int file_explorer_minimized;
extern void explorer_trigger_refresh(void);
extern void render_file_explorer(struct nk_context* ctx, int* active_drag_window_id);

// --- ASYNCHRONOUS PING & ICON TRACKING DEFINITIONS ---
typedef enum {
    NET_STATUS_UNKNOWN,
    NET_STATUS_CONNECTED,
    NET_STATUS_DISCONNECTED
} net_status_t;

static uint16_t icmp_checksum(void *addr, int count) {
    uint32_t sum = 0;
    uint16_t *ptr = (uint16_t *)addr;
    while (count > 1) { sum += *ptr++; count -= 2; }
    if (count > 0) { sum += *(uint8_t *)ptr; }
    while (sum >> 16) { sum = (sum & 0xFFFF) + (sum >> 16); }
    return (uint16_t)(~sum);
}
static net_status_t current_net_status = NET_STATUS_UNKNOWN;
static uint32_t last_ping_tick = 0;
static int pings_sent = 0;
static int pings_received = 0;
static struct raw_pcb *ping_pcb = NULL;

// Nuklear image handles initialized via storage parsing loop
static struct nk_image img_connected;
static struct nk_image img_noconnect;

// Memory pixel pools mapped for the taskbar widgets (e.g. 64x64 frames)
static uint32_t connected_pixels[64 * 64];
static uint32_t noconnect_pixels[64 * 64];

// Boot image pixel buffer (max 1024x768)
#define BOOT_IMG_MAX_W 1024
#define BOOT_IMG_MAX_H 768
static uint32_t boot_img_pixels[BOOT_IMG_MAX_W * BOOT_IMG_MAX_H];

// File explorer properties
#define MAX_FILES_PER_DIR 32
#define MAX_FILENAME_LEN  64

typedef struct {
    char name[MAX_FILENAME_LEN];
    uint32_t size;
    int is_directory;
} FileEntry;

static FileEntry current_dir_items[MAX_FILES_PER_DIR];
static int current_item_count = 0;
static char current_path[128] = "0:";
static int explorer_needs_refresh = 1;

struct ui_theme_config {
    int win_r, win_g, win_b, win_a;     // Window Surfaces + Alpha
    int title_r, title_g, title_b, title_a; // Header Bars + Alpha
    int btn_r, btn_g, btn_b, btn_a;     // Active Widgets + Alpha
    int bg_r, bg_g, bg_b;               // Desktop Background Wallpaper
    int use_transparency;               // Checkbox state: 1 = Enabled, 0 = Opaque
    int round_corners;
};

static struct ui_theme_config current_theme;

// Legacy variables mapping directly into graphics redraw loop
static int bg_r = 0;
static int bg_g = 42;  
static int bg_b = 54;  
static struct nk_colorf background_color = {0.0f, 0.168f, 0.212f, 1.0f};

extern void nk_os_render_target(struct nk_context *ctx, uint32_t* target_buffer);

struct nk_context ctx;
struct nk_user_font nk_font;
extern void init_nk_backend(struct nk_context *ctx, struct nk_user_font *nk_font);

extern int mouse_x;
extern int mouse_y;
extern int mouse_left_clicked;
extern int mouse_right_clicked;
uint32_t back_buffer[1024 * 768];

struct multiboot_info {
    uint32_t flags; uint32_t mem_lower; uint32_t mem_upper; uint32_t boot_device;
    uint32_t cmdline; uint32_t mods_count; uint32_t mods_addr; uint32_t syms[4]; 
    uint32_t mmap_length; uint32_t mmap_addr; uint32_t drives_length; uint32_t drives_addr;
    uint32_t config_table; uint32_t boot_loader_name; uint32_t apm_table;
    uint32_t vbe_control_info; uint32_t vbe_mode_info; uint16_t vbe_mode;
    uint16_t vbe_interface_seg; uint16_t vbe_interface_off; uint16_t vbe_interface_len;
    uint64_t framebuffer_addr; uint32_t framebuffer_pitch; uint32_t framebuffer_width;
    uint32_t framebuffer_height; uint8_t  framebuffer_bpp; uint8_t  framebuffer_type;
} __attribute__((packed));

void putpixel(uint32_t* fb, int x, int y, uint32_t color, uint32_t pitch) {
    volatile uint32_t* pixel_address = (volatile uint32_t*)((uint8_t*)fb + y * pitch + x * 4);
    *pixel_address = color;
}

static void mini_itoa(uint32_t value, char* buffer) {
    char temp[12]; int i = 0;
    if (value == 0) { buffer[0] = '0'; buffer[1] = '\0'; return; }
    while (value > 0) { temp[i++] = (value % 10) + '0'; value /= 10; }
    int j = 0;
    while (i > 0) { buffer[j++] = temp[--i]; }
    buffer[j] = '\0';
}

extern long strtol(const char* nptr, char** endptr, int base);
extern int strncmp(const char* s1, const char* s2, uint32_t n);
extern int mini_snprintf(char* buf, uint32_t buffer_size, const char* format, ...);
extern char* strncpy(char* dest, const char* src, uint32_t n);

// --- THEME SAVE ROUTINE ---
void theme_save_to_disk(void) {
    FIL file;
    UINT bw;
    char write_buffer[256];
    
    int len = mini_snprintf(write_buffer, sizeof(write_buffer),
        "trans=%d\n"
        "win=%d,%d,%d,%d\n"
        "title=%d,%d,%d,%d\n"
        "btn=%d,%d,%d,%d\n"
        "bg=%d,%d,%d\n",
        current_theme.use_transparency,
        current_theme.win_r, current_theme.win_g, current_theme.win_b, current_theme.win_a,
        current_theme.title_r, current_theme.title_g, current_theme.title_b, current_theme.title_a,
        current_theme.btn_r, current_theme.btn_g, current_theme.btn_b, current_theme.btn_a,
        current_theme.bg_r, current_theme.bg_g, current_theme.bg_b
    );

    if (f_open(&file, "0:/color.conf", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        f_write(&file, write_buffer, len, &bw);
        f_close(&file);
        serial_printf("[Theme] Saved configurations successfully to 0:/color.conf\n");
    }
}

void theme_load_from_disk(void) {
    FIL file;
    UINT br;
    char read_buffer[256];
    for (int i = 0; i < 256; i++) read_buffer[i] = 0;

    // Default Fallbacks
    current_theme.use_transparency = 0;
    current_theme.win_r = 45;   current_theme.win_g = 45;   current_theme.win_b = 45;   current_theme.win_a = 220;
    current_theme.title_r = 30; current_theme.title_g = 30; current_theme.title_b = 30; current_theme.title_a = 240;
    current_theme.btn_r = 60;   current_theme.btn_g = 60;   current_theme.btn_b = 60;   current_theme.btn_a = 255;
    current_theme.bg_r = 0;     current_theme.bg_g = 42;    current_theme.bg_b = 54;

    if (f_open(&file, "0:/color.conf", FA_READ) == FR_OK) {
        f_read(&file, read_buffer, sizeof(read_buffer) - 1, &br);
        f_close(&file);

        char *p = read_buffer;
        while (*p != '\0') {
            if (strncmp(p, "trans=", 6) == 0) {
                p += 6; current_theme.use_transparency = (int)strtol(p, &p, 10);
            } else if (strncmp(p, "win=", 4) == 0) {
                p += 4; current_theme.win_r = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.win_g = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.win_b = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.win_a = (int)strtol(p, &p, 10);
            } else if (strncmp(p, "title=", 6) == 0) {
                p += 6; current_theme.title_r = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.title_g = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.title_b = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.title_a = (int)strtol(p, &p, 10);
            } else if (strncmp(p, "btn=", 4) == 0) {
                p += 4; current_theme.btn_r = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.btn_g = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.btn_b = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.btn_a = (int)strtol(p, &p, 10);
            } else if (strncmp(p, "bg=", 3) == 0) {
                p += 3; current_theme.bg_r = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.bg_g = (int)strtol(p, &p, 10); if (*p == ',') p++;
                current_theme.bg_b = (int)strtol(p, &p, 10);
            }
            while (*p != '\0' && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }
    bg_r = current_theme.bg_r;
    bg_g = current_theme.bg_g;
    bg_b = current_theme.bg_b;
}

void apply_theme_to_nuklear(struct nk_context *ctx) {
    struct nk_style *s = &ctx->style;
    
    // Explicitly clamp alphas to 255 if transparency is turned off
    uint8_t wa = current_theme.use_transparency ? current_theme.win_a : 255;
    uint8_t ta = current_theme.use_transparency ? current_theme.title_a : 255;
    uint8_t ba = current_theme.use_transparency ? current_theme.btn_a : 255;

    struct nk_color c_win   = nk_rgba(current_theme.win_r, current_theme.win_g, current_theme.win_b, wa);
    struct nk_color c_title = nk_rgba(current_theme.title_r, current_theme.title_g, current_theme.title_b, ta);
    struct nk_color c_btn   = nk_rgba(current_theme.btn_r, current_theme.btn_g, current_theme.btn_b, ba);

    // --- Dynamic Corner Rounding Application ---
    if (current_theme.round_corners) {
        s->window.rounding = 10.0f; // This sets the bottom and top bar curves
        s->button.rounding = 6.0f;  // Clean radius curve for buttons
    } else {
        s->window.rounding = 0.0f;
        s->button.rounding = 0.0f;
    }

    // Apply main frame background color
    s->window.background = c_win;
    
    // CRITICAL FIX: Use simple color style hide instead of solid item overrides
    // This allows the drawing backend to fallback and clip to s->window.rounding!
    s->window.fixed_background.type = NK_STYLE_ITEM_COLOR;
    s->window.fixed_background.data.color = c_win;

    // Apply title headers
    s->window.header.normal.type = NK_STYLE_ITEM_COLOR;
    s->window.header.normal.data.color = c_title;
    s->window.header.active.type = NK_STYLE_ITEM_COLOR;
    s->window.header.active.data.color = c_title;
    
    s->window.header.label_normal = nk_rgb(255, 255, 255);
    s->window.header.label_active = nk_rgb(255, 255, 255);

    // Apply Buttons
    struct nk_color c_btn_hover = nk_rgba(
        current_theme.btn_r + 25 > 255 ? 255 : current_theme.btn_r + 25,
        current_theme.btn_g + 25 > 255 ? 255 : current_theme.btn_g + 25,
        current_theme.btn_b + 25 > 255 ? 255 : current_theme.btn_b + 25,
        ba
    );

    s->button.normal.type = NK_STYLE_ITEM_COLOR;
    s->button.normal.data.color = c_btn;
    s->button.hover.type = NK_STYLE_ITEM_COLOR;
    s->button.hover.data.color = c_btn_hover;
    s->button.active.type = NK_STYLE_ITEM_COLOR;
    s->button.active.data.color = c_btn_hover;

    // Slider handles - vertical rectangles
    s->slider.cursor_size = nk_vec2(12, 24); 
    s->slider.cursor_normal = nk_style_item_color(nk_rgb(200, 200, 200)); 
    s->slider.cursor_hover  = nk_style_item_color(nk_rgb(255, 255, 255)); 
    s->slider.cursor_active = nk_style_item_color(nk_rgb(140, 180, 240)); 
}

// ============================================================================
// IMAGE HELPER SECTION: FREESTANDING STB_IMAGE INTEGRATION ENGINE
// ============================================================================
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCAL
#define STBI_ASSERT(x)            ((void)0)

#define STBI_NO_HDR
#define STBI_NO_JPEG
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_LINEAR

extern void* memcpy(void* dest, const void* src, uint32_t n);
#define STBI_MEMCPY(dst,src,n)    memcpy(dst,src,n)

// --- EXPANDED WORKSPACE FOR LARGE IMAGES ---
uint8_t stb_arena[4 * 1024 * 1024]; // Expanded to 4MB to support any image size safely
uint32_t stb_arena_offset = 0;

extern void* kmalloc(uint32_t size);
extern void kfree(void* ptr);

 void* stbi_malloc_arena(uint32_t size) {
    size = (size + 3) & ~3;
    if (stb_arena_offset + size > sizeof(stb_arena)) {
        serial_printf("[STB ARENA ERROR] Allocation out of bounds! Requested: %u bytes\n", size);
        return (void*)0;
    }
    void* ptr = &stb_arena[stb_arena_offset];
    stb_arena_offset += size;
    return ptr;
}

 void stbi_free_arena(void* ptr) {
    (void)ptr;
}

 void* stbi_realloc_sized_arena(void* old_ptr, uint32_t old_size, uint32_t new_size) {
    if (new_size <= old_size) return old_ptr;
    void* new_ptr = stbi_malloc_arena(new_size);
    if (!new_ptr) return (void*)0;
    if (old_ptr && old_size > 0) {
        memcpy(new_ptr, old_ptr, old_size);
    }
    return new_ptr;
}

#define STBI_MALLOC(sz)                 stbi_malloc_arena(sz)
#define STBI_FREE(ptr)                  stbi_free_arena(ptr)
#define STBI_REALLOC_SIZED(ptr,oldsz,newsz) stbi_realloc_sized_arena(ptr,oldsz,newsz)

#include "stb_image.h"

struct nk_image load_image_via_stb(const char* path, uint32_t* out_pixel_buffer, int expected_w, int expected_h) {
    FIL file;
    UINT br;
    struct nk_image img = {0};

    stb_arena_offset = 0;
    serial_printf("\n============================================\n");
    serial_printf("[STB TRACE] Loading asset: %s\n", path);

    if (f_open(&file, path, FA_READ) != FR_OK) {
        serial_printf("[STB ERROR] Failed to open %s\n", path);
        return img;
    }

    DWORD file_size = f_size(&file);
    serial_printf("[STB DBG] File Size: %lu bytes\n", file_size);

    uint8_t* file_scratch_buffer = (uint8_t*)stbi_malloc_arena(file_size);
    if (!file_scratch_buffer) {
        serial_printf("[STB ERROR] Insufficient workspace arena depth.\n");
        f_close(&file);
        return img;
    }

    if (f_read(&file, file_scratch_buffer, file_size, &br) != FR_OK || br != file_size) {
        serial_printf("[STB ERROR] Failed reading file into scratch memory payload\n");
        f_close(&file);
        return img;
    }
    f_close(&file);

    if (file_size >= 4) {
        serial_printf("[STB DBG] Magic Bytes: %02X %02X %02X %02X\n", 
            file_scratch_buffer[0], file_scratch_buffer[1], file_scratch_buffer[2], file_scratch_buffer[3]);
    }

    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(1);
    
    unsigned char* decoded_pixels = stbi_load_from_memory(
        file_scratch_buffer, 
        file_size, 
        &width, 
        &height, 
        &channels, 
        4
    );

    if (!decoded_pixels) {
        serial_printf("[STB ERROR] Core parsing pass failed for: %s\n", path);
        return img;
    }

    serial_printf("[STB DBG] Decoded Resolution: %dx%d | Source Channels: %d\n", width, height, channels);

    if (width != expected_w || height != expected_h) {
        serial_printf("[STB ERROR] Coordinate bounds mismatch for %s.\n", path);
        return img;
    }

    uint32_t total_alpha = 0;
    int total_pixels = width * height;

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

            total_alpha += a;
            out_pixel_buffer[dest_idx] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    if ((total_alpha / total_pixels) < 5) {
        serial_printf("[STB WARNING] Image %s has low alpha. Injecting opaque layer...\n", path);
        for (int i = 0; i < total_pixels; i++) {
            out_pixel_buffer[i] |= 0xFF000000; 
        }
    }

    img = nk_image_ptr(out_pixel_buffer);
    img.w = width;
    img.h = height;

    serial_printf("[STB SUCCESS] Loaded image safely: %s\n============================================\n\n", path);
    return img;
}



// --- LWIP ASYNCHRONOUS ICMP MONITOR ENGINE ---
static uint8_t ping_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    (void)arg; (void)pcb; (void)addr;
    if (p->len >= (PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr))) {
        struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)((char *)p->payload + PBUF_IP_HLEN);
        if (iecho->type == ICMP_ER) {
            pings_received++;
        }
    }
    pbuf_free(p);
    return 1;
}

void send_ping_packet(const ip_addr_t *target_ip) {
    struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr), PBUF_RAM);
    if (!p) return;

    struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(iecho, ICMP_ECHO);
    ICMPH_CODE_SET(iecho, 0);
    iecho->chksum = 0;
    iecho->id     = 0xAABB;
    iecho->seqno  = lwip_htons(pings_sent + 1);

    iecho->chksum = inet_chksum(iecho, p->len);
    
    err_t err = raw_sendto(ping_pcb, p, target_ip);
    pbuf_free(p);
    pings_sent++;
}

void update_network_monitor(uint32_t current_tick) {
    static ip_addr_t target_ip = IPADDR4_INIT_BYTES(10, 0, 2, 2);
    static uint32_t last_status_print_tick = 0;
    
    if (!ping_pcb) {
        ping_pcb = raw_new(IP_PROTO_ICMP);
        raw_recv(ping_pcb, ping_recv_cb, NULL);
        raw_bind(ping_pcb, IP_ADDR_ANY);
    }

    uint32_t ms_since_last_batch = (last_ping_tick == 0) ? 999999 : ((current_tick - last_ping_tick) * 1000 / 18);
    uint32_t ms_threshold = 15000; 

    if (pings_sent == 0) {
        if (last_ping_tick != 0 && (current_tick - last_status_print_tick >= 18)) {
            last_status_print_tick = current_tick;
        }
        if (ms_since_last_batch >= ms_threshold || last_ping_tick == 0) {
            pings_received = 0;
            send_ping_packet(&target_ip);
        }
        return;
    }

    if (pings_sent < 4) {
        static uint32_t last_packet_sent_tick = 0;
        if (current_tick - last_packet_sent_tick > 15) { 
            send_ping_packet(&target_ip);
            last_packet_sent_tick = current_tick;
        }
    } else {
        static uint32_t wait_start = 0;
        if (wait_start == 0) { 
            wait_start = current_tick; 
            return; 
        }
        if (current_tick - last_status_print_tick >= 5) {
            last_status_print_tick = current_tick;
        }
        if (pings_received == 4) {
            current_net_status = NET_STATUS_CONNECTED;
            pings_sent = 0; 
            wait_start = 0; 
            last_ping_tick = current_tick;
        } 
        else if (current_tick - wait_start > 100) { 
            current_net_status = NET_STATUS_DISCONNECTED;
            pings_sent = 0; 
            wait_start = 0; 
            last_ping_tick = current_tick;
        }
    }
}

static FATFS fs;
void kernel_main(struct multiboot_info* mbinfo) {
    init_serial();
    init_gdt();
    
    if ((mbinfo->flags & (1 << 11)) == 0) { 
        while(1); 
    }

    gfx_framebuffer = (uint32_t*)((uint32_t)mbinfo->framebuffer_addr);
    gfx_width = mbinfo->framebuffer_width;
    gfx_height = mbinfo->framebuffer_height;
    gfx_pitch = mbinfo->framebuffer_pitch;

    init_idt();
    asm volatile("sti");
    
    init_nk_backend(&ctx, &nk_font);
    static int active_drag_window_id = 0;

    FRESULT mount_res = f_mount(&fs, "0:", 1);
    serial_printf("[STORAGE] Storage mount evaluation code: %d\n", (int)mount_res);

    // Try to load boot.png from VFS for the boot screen (no dimension restriction)
    int boot_img_w = 0, boot_img_h = 0;
    {
        FIL boot_file;
        UINT boot_br;
        stb_arena_offset = 0;
        if (f_open(&boot_file, "0:/boot.png", FA_READ) == FR_OK) {
            DWORD boot_fsize = f_size(&boot_file);
            uint8_t* boot_fbuf = (uint8_t*)stbi_malloc_arena(boot_fsize);
            if (boot_fbuf && f_read(&boot_file, boot_fbuf, boot_fsize, &boot_br) == FR_OK && boot_br == boot_fsize) {
                stbi_set_flip_vertically_on_load(1);
                int ch = 0;
                unsigned char* decoded = stbi_load_from_memory(boot_fbuf, (int)boot_fsize, &boot_img_w, &boot_img_h, &ch, 4);
                if (decoded && boot_img_w > 0 && boot_img_h > 0 && boot_img_w <= BOOT_IMG_MAX_W && boot_img_h <= BOOT_IMG_MAX_H) {
                    stbi_set_flip_vertically_on_load(0);
                    for (int y = 0; y < boot_img_h; y++) {
                        int target_y = (boot_img_h - 1) - y;
                        for (int x = 0; x < boot_img_w; x++) {
                            int si = (y * boot_img_w + x) * 4;
                            boot_img_pixels[target_y * boot_img_w + x] =
                                ((uint32_t)decoded[si+3] << 24) | ((uint32_t)decoded[si] << 16) |
                                ((uint32_t)decoded[si+1] << 8) | decoded[si+2];
                        }
                    }
                } else {
                    boot_img_w = 0; boot_img_h = 0;
                }
            }
            f_close(&boot_file);
        }
    }

    // Show boot screen with progress bar while we initialize
    show_boot_screen((boot_img_w > 0) ? boot_img_pixels : NULL, boot_img_w, boot_img_h);
    
    theme_load_from_disk();
    apply_theme_to_nuklear(&ctx);

    load_system_cursor("0:/arrow.cur");

    // Initialize animated cursor system and load the loading animation
    ani_init();
    ani_load("0:/arrow_load.ani");

    img_connected = load_image_via_stb("0:/connected.png", connected_pixels, 64, 64);
    img_noconnect = load_image_via_stb("0:/noconnect.png", noconnect_pixels, 64, 64);
    
    if (init_rtl8139()) {
        serial_printf("[NETWORK] RTL8139 hardware found and initialized successfully!\n");
        serial_printf("[NETWORK] Hardware MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac_address[0], mac_address[1], mac_address[2],
                      mac_address[3], mac_address[4], mac_address[5]);
    } else {
        serial_printf("[NETWORK ERROR] RTL8139 PCI Card could not be detected on the bus!\n");
    }

    lwip_init();
    ip4_addr_t ipaddr, netmask, gw;
    ip4_addr_set_zero(&ipaddr);
    ip4_addr_set_zero(&netmask);
    ip4_addr_set_zero(&gw);

    extern err_t netif_driver_init(struct netif *netif);
    netif_add(&my_netif, &ipaddr, &netmask, &gw, NULL, netif_driver_init, netif_input);
    netif_set_default(&my_netif);
    
    my_netif.hwaddr_len = 6;
    memcpy(my_netif.hwaddr, mac_address, 6);

    netif_set_up(&my_netif);
    netif_set_link_up(&my_netif);
    
    dhcp_start(&my_netif);
    
    static uint8_t dhcp_logged = 0;
    extern volatile uint32_t timer_ticks;

    while(1) {
        // --- PENDING APP LAUNCH ANIMATION HANDLER (non-blocking) ---
        if (pending_app_launch != APP_LAUNCH_NONE) {
            // Start the timer on the first frame we see a pending launch
            if (pending_app_start_tick == 0) {
                pending_app_start_tick = timer_ticks;
                // Try to start animation (ani_play checks internally if frames exist)
                ani_play();
                serial_printf("[SHELL] Animation started for pending app launch\n");
            }
            
            // Always launch after delay, even if animation failed to start
            if ((timer_ticks - pending_app_start_tick) >= ANIMATION_DELAY_TICKS) {
                // Animation delay expired (or animation not available) - launch the app
                ani_stop();
                switch (pending_app_launch) {
                    case APP_LAUNCH_SYS_MONITOR:
                        sys_monitor_active = 1; sys_monitor_minimized = 0;
                        nk_window_show(&ctx, "System Monitor", NK_SHOWN);
                        nk_window_set_focus(&ctx, "System Monitor");
                        serial_printf("[SHELL] Launched System Monitor\n");
                        break;
                    case APP_LAUNCH_THEME_MIXER:
                        color_mixer_active = 1; color_mixer_minimized = 0;
                        nk_window_show(&ctx, "Color Mixer", NK_SHOWN);
                        nk_window_set_focus(&ctx, "Color Mixer");
                        serial_printf("[SHELL] Launched Theme Mixer\n");
                        break;
                    case APP_LAUNCH_FILE_EXPLORER:
                        file_explorer_active = 1; file_explorer_minimized = 0; explorer_trigger_refresh();
                        nk_window_show(&ctx, "File Explorer", NK_SHOWN);
                        nk_window_set_focus(&ctx, "File Explorer");
                        serial_printf("[SHELL] Launched File Explorer\n");
                        break;
                    case APP_LAUNCH_NAOEDIT:
                        naoedit_active = 1; naoedit_minimized = 0;
                        nk_window_show(&ctx, "NaoEdit", NK_SHOWN);
                        nk_window_set_focus(&ctx, "NaoEdit");
                        serial_printf("[SHELL] Launched NaoEdit\n");
                        break;
                    case APP_LAUNCH_NAOVIEW:
                        naoview_active = 1; naoview_minimized = 0;
                        nk_window_show(&ctx, "NaoView - Image Viewer", NK_SHOWN);
                        nk_window_set_focus(&ctx, "NaoView - Image Viewer");
                        serial_printf("[SHELL] Launched NaoView\n");
                        break;
                }
                pending_app_launch = APP_LAUNCH_NONE;
            }
        }

        uint32_t back_pitch = gfx_width * 4;
        uint32_t current_bg_color = (bg_r << 16) | (bg_g << 8) | bg_b;

        for (uint32_t y = 0; y < gfx_height; y++) {
            for (uint32_t x = 0; x < gfx_width; x++) {
                putpixel(back_buffer, x, y, current_bg_color, back_pitch);
            }
        }

        if (my_netif.flags & NETIF_FLAG_LINK_UP) {
            static uint32_t report_timer = 0;
            if (timer_ticks - report_timer > 90) {
                report_timer = timer_ticks;
                serial_printf("[NETWORK LOOP REPORT] Interface Flags: 0x%X | Configured IP Match: %d.%d.%d.%d\n",
                              my_netif.flags,
                              (my_netif.ip_addr.addr & 0xFF),
                              ((my_netif.ip_addr.addr >> 8) & 0xFF),
                              ((my_netif.ip_addr.addr >> 16) & 0xFF),
                              ((my_netif.ip_addr.addr >> 24) & 0xFF));
            }

            netif_driver_poll(&my_netif);
            sys_check_timeouts();
            if (!dhcp_logged && dhcp_supplied_address(&my_netif)) {
                serial_printf("[NETWORK] DHCP bound!\n");
                dhcp_logged = 1;
            }
            update_network_monitor(timer_ticks);
        }

        nk_input_begin(&ctx);
        nk_input_motion(&ctx, mouse_x, mouse_y);
        nk_input_button(&ctx, NK_BUTTON_LEFT, mouse_x, mouse_y, mouse_left_clicked);
        nk_input_button(&ctx, NK_BUTTON_RIGHT, mouse_x, mouse_y, mouse_right_clicked);
        char incoming_char;
        while ((incoming_char = keyboard_pop_char()) != 0) {
            if (incoming_char == '\b') {
                nk_input_key(&ctx, NK_KEY_BACKSPACE, 1);
                nk_input_key(&ctx, NK_KEY_BACKSPACE, 0);
            } else {
                nk_input_char(&ctx, incoming_char);
            }
        }
        nk_input_end(&ctx);
        
        if (!mouse_left_clicked) active_drag_window_id = 0;

        // ==========================================================
        // WINDOW A: THE TASKBAR
        // ==========================================================
        int taskbar_height = 72; 
        nk_style_push_vec2(&ctx, &ctx.style.window.spacing, nk_vec2(4, 4));
        nk_style_push_float(&ctx, &ctx.style.window.border, 1.0f); 
        
        static uint32_t debug_frame_counter = 0;
        int trace_this_frame = ((debug_frame_counter++ % 300) == 0);

        if (nk_begin(&ctx, "TASKBAR", 
            nk_rect(0, gfx_height - taskbar_height, gfx_width, taskbar_height),
            NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND | NK_WINDOW_BORDER)) 
        {
            int cols = 3; 
            float occupied_width = 80.0f + 64.0f; 
            
            if (sys_monitor_active)   { cols++; occupied_width += 110.0f; }
            if (color_mixer_active)   { cols++; occupied_width += 110.0f; }
            if (file_explorer_active) { cols++; occupied_width += 110.0f; }
            if (naoedit_active)       { cols++; occupied_width += 110.0f; }
            if (naoview_active)       { cols++; occupied_width += 110.0f; }
            
            float total_gaps_width = (float)(cols - 1) * 4.0f;
            float blank_spacer = (float)gfx_width - occupied_width - total_gaps_width - 8.0f; 
            if (blank_spacer < 10.0f) blank_spacer = 10.0f;

            nk_layout_row_begin(&ctx, NK_STATIC, 64, cols);
            
            nk_layout_row_push(&ctx, 80);
            if (nk_button_label(&ctx, start_menu_open ? "[START]" : "START")) {
                start_menu_open = !start_menu_open;
            }
            
            if (sys_monitor_active) {
                nk_layout_row_push(&ctx, 110);
                if (nk_button_label(&ctx, sys_monitor_minimized ? "Sys Monitor" : "[Sys Monitor]")) {
                    sys_monitor_minimized = !sys_monitor_minimized;
                }
            }
            if (color_mixer_active) {
                nk_layout_row_push(&ctx, 110);
                if (nk_button_label(&ctx, color_mixer_minimized ? "Theme Mixer" : "[Theme Mixer]")) {
                    color_mixer_minimized = !color_mixer_minimized;
                }
            }
            if (file_explorer_active) {
                nk_layout_row_push(&ctx, 110);
                if (nk_button_label(&ctx, file_explorer_minimized ? "File Explorer" : "[File Explorer]")) {
                    file_explorer_minimized = !file_explorer_minimized;
                }
            }
            if (naoedit_active) {
                nk_layout_row_push(&ctx, 110);
                if (nk_button_label(&ctx, naoedit_minimized ? "NaoEdit" : "[NaoEdit]")) {
                    naoedit_minimized = !naoedit_minimized;
                }
            }
            if (naoview_active) {
                nk_layout_row_push(&ctx, 110);
                if (nk_button_label(&ctx, naoview_minimized ? "NaoView" : "[NaoView]")) {
                    naoview_minimized = !naoview_minimized;
                }
            }
            
            nk_layout_row_push(&ctx, blank_spacer); 
            nk_label(&ctx, "", NK_TEXT_LEFT);

            nk_layout_row_push(&ctx, 64);
            struct nk_rect icon_bounds = nk_widget_bounds(&ctx);
            struct nk_image target_icon = (current_net_status == NET_STATUS_CONNECTED) ? img_connected : img_noconnect;

            if (target_icon.handle.ptr == NULL) {
                if (trace_this_frame) serial_printf("[TASKBAR ERROR] Target Icon Pointer is NULL!\n");
                nk_fill_rect(&ctx.current->buffer, icon_bounds, 0.0f, nk_rgb(255, 0, 0)); 
            } else {
                nk_image(&ctx, target_icon);
                nk_fill_rect(&ctx.current->buffer, nk_rect(icon_bounds.x, icon_bounds.y, 8, 8), 0.0f, nk_rgb(0, 255, 0)); 
            }

            nk_layout_row_end(&ctx);
        }
        nk_end(&ctx);
        nk_style_pop_float(&ctx);
        nk_style_pop_vec2(&ctx);

        if (start_menu_open) {
            int menu_w = 200, menu_h = 320; 
            if (nk_begin(&ctx, "START_MENU", nk_rect(5, gfx_height - taskbar_height - menu_h - 5, menu_w, menu_h),
                NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_TITLE)) 
            {
                nk_layout_row_dynamic(&ctx, 30, 1);
                if (nk_button_label(&ctx, "System Monitor")) {
                    if (pending_app_launch == APP_LAUNCH_NONE) {
                        pending_app_launch = APP_LAUNCH_SYS_MONITOR;
                        start_menu_open = 0;
                        serial_printf("[SHELL] Queuing System Monitor launch with animation...\n");
                    }
                }
                if (nk_button_label(&ctx, "Theme Mixer")) {
                    if (pending_app_launch == APP_LAUNCH_NONE) {
                        pending_app_launch = APP_LAUNCH_THEME_MIXER;
                        start_menu_open = 0;
                        serial_printf("[SHELL] Queuing Theme Mixer launch with animation...\n");
                    }
                }
                if (nk_button_label(&ctx, "File Explorer")) {
                    if (pending_app_launch == APP_LAUNCH_NONE) {
                        pending_app_launch = APP_LAUNCH_FILE_EXPLORER;
                        start_menu_open = 0;
                        serial_printf("[SHELL] Queuing File Explorer launch with animation...\n");
                    }
                }
                if (nk_button_label(&ctx, "NaoEdit")) {
                    if (pending_app_launch == APP_LAUNCH_NONE) {
                        pending_app_launch = APP_LAUNCH_NAOEDIT;
                        start_menu_open = 0;
                        serial_printf("[SHELL] Queuing NaoEdit launch with animation...\n");
                    }
                }
                if (nk_button_label(&ctx, "NaoView Viewer")) {
                    if (pending_app_launch == APP_LAUNCH_NONE) {
                        pending_app_launch = APP_LAUNCH_NAOVIEW;
                        start_menu_open = 0;
                        serial_printf("[SHELL] Queuing NaoView launch with animation...\n");
                    }
                }
                nk_layout_row_dynamic(&ctx, 35, 1);
                if (nk_button_label(&ctx, "  Restart  ")) {
                    start_menu_open = 0;
                    restart_system();
                }
                nk_layout_row_dynamic(&ctx, 35, 1);
                if (nk_button_label(&ctx, "  Shutdown  ")) {
                    system_shutdown = 1;
                    start_menu_open = 0;
                }
            }
            nk_end(&ctx);
        }
        
        // Handle shutdown
        if (system_shutdown) {
            // Clear screen to blank (black)
            for (uint32_t y_sh = 0; y_sh < gfx_height; y_sh++) {
                for (uint32_t x_sh = 0; x_sh < gfx_width; x_sh++) {
                    putpixel(gfx_framebuffer, x_sh, y_sh, 0x00000000, gfx_pitch);
                }
            }
            
            // Draw shutdown text on display
            uint32_t shutdown_text_color = 0x00FFFFFF;
            char* msg1 = "It is now safe to turn off";
            char* msg2 = "the computer.";
            
            // Calculate center position
            int text_x = (gfx_width - (32 * 8)) / 2;  // 32 characters * 8px width
            int text_y = gfx_height / 2 - 16;
            
            // Draw the two lines of text
            int x_pos = text_x;
            int y_pos = text_y;
            char* p = msg1;
            while (*p) {
                draw_char(gfx_framebuffer, x_pos, y_pos, *p, shutdown_text_color, gfx_pitch, putpixel);
                x_pos += 8;
                p++;
            }
            
            x_pos = text_x;
            y_pos = text_y + 10;
            p = msg2;
            while (*p) {
                draw_char(gfx_framebuffer, x_pos, y_pos, *p, shutdown_text_color, gfx_pitch, putpixel);
                x_pos += 8;
                p++;
            }
            
            // Display shutdown message via serial too
            serial_printf("\n\n========================================\n");
            serial_printf("  It is now safe to turn off the computer.\n");
            serial_printf("========================================\n\n");
            
            // Halt the system
            asm volatile("cli");
            while(1) {
                asm volatile("hlt");
            }
        }

        if (sys_monitor_active && !sys_monitor_minimized) {
            if (nk_begin(&ctx, "System Monitor", nk_rect(150, 100, 280, 200),
                NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) 
            {
                uint32_t seconds = timer_ticks / 18;
                char ticks_str[32] = "System Ticks: ", secs_str[32] = "Uptime (Sec): ", num_buf[16];
                mini_itoa(timer_ticks, num_buf);
                int idx = 14; for(int n=0; num_buf[n] != '\0' && idx < 31; n++) ticks_str[idx++] = num_buf[n]; ticks_str[idx] = '\0';
                mini_itoa(seconds, num_buf);
                idx = 14; for(int n=0; num_buf[n] != '\0' && idx < 31; n++) secs_str[idx++] = num_buf[n]; secs_str[idx] = '\0';
                nk_layout_row_dynamic(&ctx, 25, 1);
                nk_label(&ctx, ticks_str, NK_TEXT_LEFT); nk_label(&ctx, secs_str, NK_TEXT_LEFT);
                nk_layout_row_dynamic(&ctx, 30, 1);
                nk_label(&ctx, "Status: Kernel Operational", NK_TEXT_LEFT);
            }
            if (nk_window_has_focus(&ctx) || active_drag_window_id == 1) {
                if (mouse_left_clicked) active_drag_window_id = 1;
            } else if (nk_window_is_hovered(&ctx) && mouse_left_clicked && active_drag_window_id == 0) {
                if (!nk_item_is_any_active(&ctx)) { active_drag_window_id = 1; nk_window_set_focus(&ctx, "System Monitor"); }
            }
            nk_end(&ctx);
            if (nk_window_is_hidden(&ctx, "System Monitor")) sys_monitor_active = 0;
        }

        // ==========================================================
        // FIXED WINDOW B: THE THEME CONFIGURATION MIXER PANEL
        // ==========================================================
        if (color_mixer_active && !color_mixer_minimized) {
        if (nk_begin(&ctx, "Color Mixer", nk_rect(420, 40, 360, 500),
            NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) 
        {
            int changed = 0;
            char label_buffer[48];

            // Global Layout Modifier Checkboxes
            nk_layout_row_dynamic(&ctx, 22, 1);
            if (nk_checkbox_label(&ctx, "Enable Window Transparency Effects", &current_theme.use_transparency)) {
                changed = 1;
            }
            
            if (nk_checkbox_label(&ctx, "Enable Rounded Window & Button Corners", &current_theme.round_corners)) {
                changed = 1;
            }

            nk_layout_row_dynamic(&ctx, 10, 1); // Spacer separator

            // --- Category 1: Desktop Background ---
            mini_snprintf(label_buffer, sizeof(label_buffer), "Wallpaper [R:%d G:%d B:%d]", current_theme.bg_r, current_theme.bg_g, current_theme.bg_b);
            nk_layout_row_dynamic(&ctx, 18, 1); nk_label(&ctx, label_buffer, NK_TEXT_LEFT);
            
            nk_layout_row_dynamic(&ctx, 24, 3);
            changed |= nk_slider_int(&ctx, 0, &current_theme.bg_r, 255, 1);
            changed |= nk_slider_int(&ctx, 0, &current_theme.bg_g, 255, 1);
            changed |= nk_slider_int(&ctx, 0, &current_theme.bg_b, 255, 1);

            // --- Category 2: Window Base Background + Alpha ---
            if (current_theme.use_transparency) {
                mini_snprintf(label_buffer, sizeof(label_buffer), "UI Window [R:%d G:%d B:%d] Alpha:%d (0-255)", current_theme.win_r, current_theme.win_g, current_theme.win_b, current_theme.win_a);
            } else {
                mini_snprintf(label_buffer, sizeof(label_buffer), "UI Window [R:%d G:%d B:%d] Opaque", current_theme.win_r, current_theme.win_g, current_theme.win_b);
            }
            nk_layout_row_dynamic(&ctx, 18, 1); nk_label(&ctx, label_buffer, NK_TEXT_LEFT);
            
            int win_cols = current_theme.use_transparency ? 4 : 3;
            nk_layout_row_dynamic(&ctx, 24, win_cols);
            changed |= nk_slider_int(&ctx, 0, &current_theme.win_r, 255, 1);
            changed |= nk_slider_int(&ctx, 0, &current_theme.win_g, 255, 1);
            changed |= nk_slider_int(&ctx, 0, &current_theme.win_b, 255, 1);
            if (current_theme.use_transparency) {
                changed |= nk_slider_int(&ctx, 0, &current_theme.win_a, 255, 1);
            }

            // --- Category 3: Title Bar Header Surface + Alpha ---
            if (current_theme.use_transparency) {
                mini_snprintf(label_buffer, sizeof(label_buffer), "Header Bar [R:%d G:%d B:%d] Alpha:%d (0-255)", current_theme.title_r, current_theme.title_g, current_theme.title_b, current_theme.title_a);
            } else {
                mini_snprintf(label_buffer, sizeof(label_buffer), "Header Bar [R:%d G:%d B:%d] Opaque", current_theme.title_r, current_theme.title_g, current_theme.title_b);
            }
            nk_layout_row_dynamic(&ctx, 18, 1); nk_label(&ctx, label_buffer, NK_TEXT_LEFT);
            
            nk_layout_row_dynamic(&ctx, 24, win_cols);
            changed |= nk_slider_int(&ctx, 0, &current_theme.title_r, 255, 1);
            changed |= nk_slider_int(&ctx, 0, &current_theme.title_g, 255, 1);
            changed |= nk_slider_int(&ctx, 0, &current_theme.title_b, 255, 1);
            if (current_theme.use_transparency) {
                changed |= nk_slider_int(&ctx, 0, &current_theme.title_a, 255, 1);
            }

            // --- Category 4: Clickable Widget Button Nodes + Alpha ---
            if (current_theme.use_transparency) {
                mini_snprintf(label_buffer, sizeof(label_buffer), "Buttons [R:%d G:%d B:%d] Alpha:%d (0-255)", current_theme.btn_r, current_theme.btn_g, current_theme.btn_b, current_theme.btn_a);
            } else {
                mini_snprintf(label_buffer, sizeof(label_buffer), "Buttons [R:%d G:%d B:%d] Opaque", current_theme.btn_r, current_theme.btn_g, current_theme.btn_b);
            }
            nk_layout_row_dynamic(&ctx, 18, 1); nk_label(&ctx, label_buffer, NK_TEXT_LEFT);
            
            nk_layout_row_dynamic(&ctx, 24, win_cols);
            changed |= nk_slider_int(&ctx, 0, &current_theme.btn_r, 255, 1);
            changed |= nk_slider_int(&ctx, 0, &current_theme.btn_g, 255, 1);
            changed |= nk_slider_int(&ctx, 0, &current_theme.btn_b, 255, 1);
            if (current_theme.use_transparency) {
                changed |= nk_slider_int(&ctx, 0, &current_theme.btn_a, 255, 1);
            }

            if (changed) {
                bg_r = current_theme.bg_r;
                bg_g = current_theme.bg_g;
                bg_b = current_theme.bg_b;
                apply_theme_to_nuklear(&ctx);
            }

            nk_layout_row_dynamic(&ctx, 15, 1); // Row Padding Spacer
            
            nk_layout_row_dynamic(&ctx, 32, 2);
            if (nk_button_label(&ctx, "Save Theme")) {
                theme_save_to_disk();
            }
            if (nk_button_label(&ctx, "Reload Config")) {
                theme_load_from_disk();
                apply_theme_to_nuklear(&ctx);
            }
        }
        // Handle focus routines matching remaining UI wrappers
        if (nk_window_has_focus(&ctx) || active_drag_window_id == 2) {
            if (mouse_left_clicked) active_drag_window_id = 2;
        } else if (nk_window_is_hovered(&ctx) && mouse_left_clicked && active_drag_window_id == 0) {
            if (!nk_item_is_any_active(&ctx)) { active_drag_window_id = 2; nk_window_set_focus(&ctx, "Color Mixer"); }
        }
        nk_end(&ctx);
        if (nk_window_is_hidden(&ctx, "Color Mixer")) color_mixer_active = 0;
    }   

        render_file_explorer(&ctx, &active_drag_window_id);
        render_naoedit(&ctx, &active_drag_window_id);
        render_naoview(&ctx, &active_drag_window_id);
        
        nk_os_render_target(&ctx, back_buffer);
        draw_custom_cursor();
        
        // Draw animated cursor on top if animation is active (non-blocking)
        ani_update(timer_ticks);
        ani_draw(mouse_x, mouse_y);
        
        netif_driver_poll(&my_netif);
        sys_check_timeouts();

        for (uint32_t y = 0; y < gfx_height; y++) {
            for (uint32_t x = 0; x < gfx_width; x++) {
                gfx_framebuffer[y * (gfx_pitch/4) + x] = back_buffer[y * gfx_width + x];
            }
        }
    }
}

// --- MISSING MEMORY & STRING LIB C COMPLIANCE ---
void* memcpy(void* dest, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dest; const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}
void* memmove(void* dest, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dest; const uint8_t* s = (const uint8_t*)src;
    if (d < s) { for (uint32_t i = 0; i < n; i++) d[i] = s[i]; }
    else { for (uint32_t i = n; i > 0; i--) d[i-1] = s[i-1]; }
    return dest;
}
uint32_t strlen(const char* s) { uint32_t len = 0; while (s[len] != '\0') len++; return len; }
int strncmp(const char* s1, const char* s2, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') return (s1[i] < s2[i]) ? -1 : (s1[i] > s2[i]);
    }
    return 0;
}
long strtol(const char* nptr, char** endptr, int base) {
    (void)base; long result = 0;
    while (*nptr >= '0' && *nptr <= '9') { result = result * 10 + (*nptr - '0'); nptr++; }
    if (endptr) *endptr = (char*)nptr;
    return result;
}

struct ip_globals ip_data;
static const unsigned short ctype_b_data[256] = {
    ['0'] = 0x0800, ['1'] = 0x0800, ['2'] = 0x0800, ['3'] = 0x0800, ['4'] = 0x0800,
    ['5'] = 0x0800, ['6'] = 0x0800, ['7'] = 0x0800, ['8'] = 0x0800, ['9'] = 0x0800
};
const unsigned short int **__ctype_b_loc(void) {
    static const unsigned short int *ptr = ctype_b_data; return &ptr;
}
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int abs(int j) {
    return (j < 0) ? -j : j;
}
double pow(double base, double exponent) {
    if (exponent == 0.0) return 1.0;
    if (exponent < 0.0) {
        base = 1.0 / base;
        exponent = -exponent;
    }
    double result = 1.0;
    long long int_exp = (long long)exponent;
    while (int_exp > 0) {
        if (int_exp & 1) result *= base;
        base *= base;
        int_exp >>= 1;
    }
    return result;
}

double ldexp(double x, int exp) {
    if (exp > 0) {
        while (exp--) x *= 2.0;
    } else if (exp < 0) {
        while (exp++) x /= 2.0;
    }
    return x;
}
char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

int snprintf(char* buf, uint32_t size, const char* format, ...) {
    if (size == 0) return 0;
    uint32_t buf_idx = 0;
    const char* p = format;
    uint8_t* args = (uint8_t*)&format + 4; 

    while (*p && buf_idx < (size - 1)) {
        if (*p == '%' && *(p + 1) != '\0') {
            p++; 
            if (*p == 's') {
                const char* str_arg = *(const char**)args;
                args += 4;
                while (str_arg && *str_arg && buf_idx < (size - 1)) {
                    buf[buf_idx++] = *str_arg++;
                }
            } else if (*p == 'd' || *p == 'u') {
                uint32_t num_arg = *(uint32_t*)args;
                args += 4;
                char num_buf[16];
                mini_itoa(num_arg, num_buf);
                for (int n = 0; num_buf[n] != '\0' && buf_idx < (size - 1); n++) {
                    buf[buf_idx++] = num_buf[n];
                }
            } else {
                buf[buf_idx++] = *p;
            }
        } else {
            buf[buf_idx++] = *p;
        }
        p++;
    }
    buf[buf_idx] = '\0';
    return buf_idx;
}

char* strncpy(char* dest, const char* src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for ( ; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

int mini_snprintf(char* buf, uint32_t buffer_size, const char* format, ...) {
    if (!buf || buffer_size == 0) return 0;

    __builtin_va_list args;
    __builtin_va_start(args, format);

    uint32_t write_idx = 0;
    const char* p = format;

    while (*p != '\0' && write_idx < (buffer_size - 1)) {
        if (*p == '%') {
            p++; 
            if (*p == '\0') break;

            if (*p == 's') {
                const char* str_arg = __builtin_va_arg(args, const char*);
                if (!str_arg) str_arg = "(null)";
                while (*str_arg != '\0' && write_idx < (buffer_size - 1)) {
                    buf[write_idx++] = *str_arg++;
                }
            } 
            else if (*p == 'd' || *p == 'u') {
                long val;
                if (*p == 'd') {
                    int sval = __builtin_va_arg(args, int);
                    if (sval < 0) {
                        if (write_idx < (buffer_size - 1)) {
                            buf[write_idx++] = '-';
                        }
                        val = -sval;
                    } else {
                        val = sval;
                    }
                } else {
                    val = (long)__builtin_va_arg(args, unsigned int);
                }

                char num_scratch[12];
                int digits_count = 0;
                if (val == 0) {
                    num_scratch[digits_count++] = '0';
                } else {
                    while (val > 0) {
                        num_scratch[digits_count++] = (val % 10) + '0';
                        val /= 10;
                    }
                }
                while (digits_count > 0 && write_idx < (buffer_size - 1)) {
                    buf[write_idx++] = num_scratch[--digits_count];
                }
            } 
            else {
                buf[write_idx++] = *p;
            }
        } else {
            buf[write_idx++] = *p;
        }
        p++;
    }

    buf[write_idx] = '\0'; 
    __builtin_va_end(args);
    return (int)write_idx;
}