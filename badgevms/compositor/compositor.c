/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "badgevms/compositor.h"

#include "badgevms/device.h"
#include "badgevms/event.h"
#include "badgevms/pixel_formats.h"
#include "badgevms/process.h"
#include "badgevms_config.h"
#include "compositor_private.h"
#if CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA
#include "driver/ppa.h"
#endif
#if CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA && CONFIG_CJ_BADGEVMS_COMPOSITOR_PPA_SW_RGB_SWAP
#include "esp_heap_caps.h"
#endif
#include "esp_cache.h"
#include "esp_ipc.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "font.h"
#include "memory.h"
#include "pixel_functions.h"
#include "task.h"
#include "window_decorations.h"

#include <stdatomic.h>

#include <math.h>
#include <sys/time.h>

#define TAG "compositor"

#define SWAP(x, y)                                                                                                     \
    do {                                                                                                               \
        typeof(x) SWAP = x;                                                                                            \
        x              = y;                                                                                            \
        y              = SWAP;                                                                                         \
    } while (0)

static TaskHandle_t  compositor_handle;
static lcd_device_t *lcd_device;
static device_t     *keyboard_device;

static window_t     *window_stack = NULL;
static QueueHandle_t compositor_queue;

static int        cur_fb = 0;
static atomic_int cur_num_windows;
static uint16_t  *framebuffers[DISPLAY_FRAMEBUFFERS];

static int  background_damaged    = 7;
static int  decoration_damaged    = 7;
static bool visible_regions_valid = false;

typedef enum {
    WINDOW_CREATE,
    WINDOW_DESTROY,
    WINDOW_FLAGS,
    WINDOW_MOVE,
    WINDOW_RESIZE,
    FRAMEBUFFER_SWAP
} compositor_command_t;

typedef struct {
    compositor_command_t   command;
    window_t              *window;
    window_flag_t          flags;
    window_coords_t        coords;
    window_size_t          size;
    managed_framebuffer_t *fb_a;
    managed_framebuffer_t *fb_b;
    TaskHandle_t           caller;
} compositor_message_t;

rotation_angle_t rotation = ROTATION_ANGLE_270;

#define WINDOW_MAX_W (FRAMEBUFFER_MAX_W - (2 * BORDER_PX))
#define WINDOW_MAX_H (FRAMEBUFFER_MAX_H - BORDER_TOP_PX - BORDER_PX)

#define ALL_DISPLAY_FB_MASK 7 // (1 + 2 + 4)

#define WINDOW_MOVE_STEP          10
#define WINDOW_COMMANDS_PER_FRAME 5
#define KEYBOARD_EVENTS_PER_FRAME 10

static inline void mark_scene_damaged(void) {
    visible_regions_valid = false;
    decoration_damaged    = ALL_DISPLAY_FB_MASK;
    background_damaged    = ALL_DISPLAY_FB_MASK;
}

#if CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA
__attribute__((always_inline)) static inline ppa_srm_rotation_angle_t rotation_to_srm(rotation_angle_t rotation) {
    switch (rotation) {
        case ROTATION_ANGLE_270: return PPA_SRM_ROTATION_ANGLE_90;
        case ROTATION_ANGLE_180: return PPA_SRM_ROTATION_ANGLE_180;
        case ROTATION_ANGLE_90: return PPA_SRM_ROTATION_ANGLE_270;
        default:
    }
    return PPA_SRM_ROTATION_ANGLE_0;
}
#endif

__attribute__((always_inline)) static inline window_size_t window_clamp_size(window_t *window, window_size_t size) {
    window_size_t ret;

    size.w = size.w < 0 ? 0 : size.w;
    size.h = size.h < 0 ? 0 : size.h;

    if (window->flags & WINDOW_FLAG_FULLSCREEN) {
        ret.w = size.w > FRAMEBUFFER_MAX_W ? FRAMEBUFFER_MAX_W : size.w;
        ret.h = size.h > FRAMEBUFFER_MAX_H ? FRAMEBUFFER_MAX_H : size.h;
    } else {
        ret.w = size.w > WINDOW_MAX_W ? WINDOW_MAX_W : size.w;
        ret.h = size.h > WINDOW_MAX_H ? WINDOW_MAX_H : size.h;
    }

    return ret;
}

__attribute__((always_inline)) static inline window_coords_t
    window_clamp_position(window_t *window, window_coords_t position) {
    window_coords_t ret;

    position.x = position.x < 0 ? 0 : position.x;
    position.y = position.y < 0 ? 0 : position.y;

    if (window->flags & WINDOW_FLAG_FULLSCREEN) {
        ret.x = 0;
        ret.y = 0;
    } else {
        int max_x = FRAMEBUFFER_MAX_W - (window->rect.w + (2 * BORDER_PX));
        int max_y = FRAMEBUFFER_MAX_H - (window->rect.h + (BORDER_TOP_PX + BORDER_PX));
        ret.x     = position.x > max_x ? max_x : position.x;
        ret.y     = position.y > max_y ? max_y : position.y;
    }

    return ret;
}

__attribute__((always_inline)) static inline void push_window(window_t *window) {
    window_t *head = window_stack;

    if (head) {
        window_t *tail = head->prev;

        window->next = head;
        window->prev = tail;
        head->prev   = window;
        tail->next   = window;
    } else {
        window->prev = window;
        window->next = window;
    }

    window_stack = window;
    atomic_fetch_add(&cur_num_windows, 1);
}

__attribute__((always_inline)) static inline void remove_window(window_t *window) {
    // If we were forcibly removed we won't be in the list. This guard only
    // catches a double-remove if the FIRST remove_window() call actually
    // clears prev/next on the way out (below) -- previously it didn't, so
    // this guard only worked at call sites that also explicitly nulled
    // both fields themselves right after calling remove_window() (as the
    // ALT-TAB close-window handler does), while other call sites (e.g. the
    // render loop's window_destroy-detected-via-NULL-task_info path) could
    // remove the same window twice -- once there, once again when the
    // deferred WINDOW_DESTROY message for it was later drained -- each
    // decrementing cur_num_windows, which run_init()'s launcher-respawn
    // watchdog depends on via compositor_window_count().
    if (!window->prev || !window->next) {
        return;
    }

    // We are the only window
    if (window->prev == window) {
        window_stack = NULL;
        goto out;
    }
    window_t *prev = window->prev;
    window_t *next = window->next;

    // Only one window left
    if (prev == next) {
        prev->next   = prev;
        prev->prev   = prev;
        window_stack = prev;
        goto out;
    }

    next->prev   = prev;
    prev->next   = next;
    window_stack = next;
out:
    // Make the guard at the top of this function actually work regardless
    // of which call site removed us -- see the comment there.
    window->prev = NULL;
    window->next = NULL;
    atomic_fetch_sub(&cur_num_windows, 1);
}

void window_calculate_visible_regions(window_t *window, window_t *window_list_head, float scale) {
    window->visible.count    = 1;
    window->visible.rects[0] = window->rect;

    if (!(window->flags & WINDOW_FLAG_FULLSCREEN)) {
        // For occlusion we only care about our OWN content region
        window->visible.rects[0].x += BORDER_PX;
        window->visible.rects[0].y += BORDER_TOP_PX;
    }

    window_t *occluder = window_list_head;

    while (occluder != NULL && occluder != window) {
        rect_array_t new_visible = {0};

        for (int j = 0; j < window->visible.count; j++) {
            window_rect_t occluder_rect = occluder->rect;
            if (!(occluder->flags & WINDOW_FLAG_FULLSCREEN)) {
                // But other window decorations do occlude us
                occluder_rect.w += (BORDER_PX * 2);
                occluder_rect.h += BORDER_TOP_PX + BORDER_PX;
            }

            small_rect_array_t pieces = rect_subtract(window->visible.rects[j], occluder_rect);

            for (int k = 0; k < pieces.count; k++) {
                if (new_visible.count < MAX_VISIBLE_RECTS) {
                    new_visible.rects[new_visible.count++] = pieces.rects[k];
                }
            }
        }

        window->visible = new_visible;

        if (window->visible.count == 0) {
            break;
        }

        occluder = occluder->next;
    }

    merge_rectangles(&window->visible);
#if CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA
    int dropped = 0;
    while (ppa_workaround_split_rects(&window->visible, scale, &dropped)) {
    }
    if (dropped) {
        ESP_LOGW(TAG, "Dropped %d damage rect(s) past MAX_VISIBLE_RECTS -- expect stale pixels", dropped);
    }
#endif
}

window_rect_t content_to_framebuffer_rect(window_rect_t content_rect, window_t *window, float scale) {
    if (!(window->flags & WINDOW_FLAG_FULLSCREEN)) {
        content_rect.x -= (window->rect.x + BORDER_PX);
        content_rect.y -= (window->rect.y + BORDER_TOP_PX);
    }

    managed_framebuffer_t *framebuffer = window->framebuffers[window->front_fb];

    int start_x = (int)(content_rect.x / scale);
    int start_y = (int)(content_rect.y / scale);
    int end_x   = (int)((content_rect.x + content_rect.w) / scale);
    int end_y   = (int)((content_rect.y + content_rect.h) / scale);

    start_x = MAX(0, MIN(start_x, (int)framebuffer->w - 1));
    start_y = MAX(0, MIN(start_y, (int)framebuffer->h - 1));
    end_x   = MAX(start_x, MIN(end_x, (int)framebuffer->w));
    end_y   = MAX(start_y, MIN(end_y, (int)framebuffer->h));

    window_rect_t fb_rect = {.x = start_x, .y = start_y, .w = end_x - start_x, .h = end_y - start_y};

    return fb_rect;
}

static void reassign_vaddr(uintptr_t new_vaddr_start, size_t num_pages, allocation_range_t *head) {
    // we go backwards because we start from the highest address
    // and don't forget our guard page
    uintptr_t cur_vaddr_offset = new_vaddr_start + ((num_pages - 1) * SOC_MMU_PAGE_SIZE);

    allocation_range_t *r = head;
    while (r) {
        cur_vaddr_offset -= r->size;
        // ESP_LOGW(TAG, "Swapping vaddr %08lx with %08lx", r->vaddr_start, cur_vaddr_offset);
        r->vaddr_start    = cur_vaddr_offset;
        r                 = r->next;
    }
}

static void framebuffer_swap(managed_framebuffer_t *fb_a, managed_framebuffer_t *fb_b) {
    if (!fb_a || !fb_b || !fb_a->framebuffer.pixels || !fb_b->framebuffer.pixels) {
        return;
    }

    // ESP_LOGV(TAG, "Attempting to swap framebuffers %p and %p", fb_a, fb_b);

    esp_cache_msync(
        fb_a->framebuffer.pixels,
        fb_a->num_pages * SOC_MMU_PAGE_SIZE,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE
    );
    esp_cache_msync(
        fb_b->framebuffer.pixels,
        fb_b->num_pages * SOC_MMU_PAGE_SIZE,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE
    );

    framebuffer_unmap_pages(fb_a->head_pages);
    framebuffer_unmap_pages(fb_b->head_pages);

    uintptr_t fb_a_vaddr = fb_a->tail_pages->vaddr_start;
    uintptr_t fb_b_vaddr = fb_b->tail_pages->vaddr_start;

    reassign_vaddr(fb_b_vaddr, fb_a->num_pages, fb_a->head_pages);
    reassign_vaddr(fb_a_vaddr, fb_b->num_pages, fb_b->head_pages);

    // Caches are already invalidated
    framebuffer_map_pages(fb_a->head_pages, fb_a->tail_pages);
    framebuffer_map_pages(fb_b->head_pages, fb_b->tail_pages);
}

framebuffer_t *framebuffer_allocate(uint32_t w, uint32_t h, pixel_format_t format) {
    // Don't use BADGEVMS_BYTESPERPIXEL here because we also want to
    // clamp the pixel formats we want to support here anyway

    short framebuffer_bpp = 2;
    switch (format) {
        case BADGEVMS_PIXELFORMAT_BGRA8888: // fallthrough
        case BADGEVMS_PIXELFORMAT_RGBA8888: // fallthrough
        case BADGEVMS_PIXELFORMAT_ARGB8888: // fallthrough
        case BADGEVMS_PIXELFORMAT_ABGR8888: framebuffer_bpp = 4; break;
        case BADGEVMS_PIXELFORMAT_RGB565: // fallthrough
        case BADGEVMS_PIXELFORMAT_BGR565: break;
        default: format = BADGEVMS_PIXELFORMAT_RGB565;
    }

    size_t    num_pages        = 0;
    size_t    framebuffer_size = (w * h * framebuffer_bpp) + SOC_MMU_PAGE_SIZE;
    uintptr_t vaddr_start      = framebuffer_vaddr_allocate(framebuffer_size, &num_pages);

    if (!vaddr_start) {
        ESP_LOGE(TAG, "No vaddr space for frame buffer");
        return NULL;
    }

    managed_framebuffer_t *framebuffer = calloc(1, sizeof(managed_framebuffer_t));
    if (!framebuffer) {
        ESP_LOGE(TAG, "No kernel RAM for frame buffer container");
        framebuffer_vaddr_deallocate(vaddr_start);
        return NULL;
    }

    allocation_range_t *tail_range = NULL;
    if (!pages_allocate(vaddr_start, num_pages - 1, &framebuffer->head_pages, &framebuffer->tail_pages)) {
        ESP_LOGE(TAG, "No physical memory pages for frame buffer");
        framebuffer_vaddr_deallocate(vaddr_start);
        free(framebuffer);
        return NULL;
    }

    ESP_LOGI(
        TAG,
        "Attempting to map our vaddr %p head_range %p tail_range %p",
        (void *)framebuffer->head_pages->vaddr_start,
        framebuffer->head_pages,
        tail_range
    );

    framebuffer_map_pages(framebuffer->head_pages, framebuffer->tail_pages);
    framebuffer->framebuffer.pixels = (uint16_t *)vaddr_start;

    framebuffer->framebuffer.w      = w;
    framebuffer->framebuffer.h      = h;
    framebuffer->framebuffer.format = format;
    // Keep a copy that the user can't easily change
    framebuffer->w                  = w;
    framebuffer->h                  = h;
    framebuffer->format             = format;
    framebuffer->num_pages          = num_pages;
    atomic_flag_test_and_set(&framebuffer->clean);

    memset(framebuffer->framebuffer.pixels, 0, (w * h * framebuffer_bpp));

    ESP_LOGW(
        TAG,
        "Allocated framebuffer at %p, pixels at %p, size %zi, dimensions %u x %u",
        framebuffer,
        framebuffer->framebuffer.pixels,
        num_pages,
        framebuffer->framebuffer.w,
        framebuffer->framebuffer.h
    );

    return (framebuffer_t *)framebuffer;
}

void framebuffer_free(managed_framebuffer_t *framebuffer) {
    if (framebuffer) {
        framebuffer_unmap_pages(framebuffer->head_pages);
        pages_deallocate(framebuffer->head_pages);
        framebuffer_vaddr_deallocate((uintptr_t)framebuffer->framebuffer.pixels);

        free(framebuffer);
    }
}

IRAM_ATTR static void on_refresh(void *ignored) {
    xTaskNotifyGiveIndexed(compositor_handle, 0);
}

// static bool IRAM_ATTR ppa_srm_callback(ppa_client_handle_t ppa_client, ppa_event_data_t *event_data, void *user_data)
// {
//     xTaskNotifyGiveIndexed(compositor_handle, 0);
//     return true;
// }

#if CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA && CONFIG_CJ_BADGEVMS_COMPOSITOR_PPA_SW_RGB_SWAP
// Swaps the R and B channels of a sub-rectangle of `src` (stride `src_stride_px`
// pixels) into `dst`, tightly packed at rect.w stride. Used to pre-swap into a
// scratch buffer so PPA's own rgb_swap bit can stay off -- see this option's
// Kconfig help text for why.
static void copy_swap_rb_rect(void *dst, void const *src, int src_stride_px, int bytes_per_pixel, window_rect_t rect) {
    if (bytes_per_pixel == 2) {
        uint16_t       *d = (uint16_t *)dst;
        uint16_t const *s = (uint16_t const *)src;
        for (int y = 0; y < rect.h; y++) {
            uint16_t const *srow = s + (size_t)(rect.y + y) * src_stride_px + rect.x;
            uint16_t       *drow = d + (size_t)y * rect.w;
            for (int x = 0; x < rect.w; x++) {
                uint16_t p = srow[x];
                drow[x]    = (uint16_t)((p & 0x07E0) | ((p & 0xF800) >> 11) | ((p & 0x001F) << 11));
            }
        }
    } else {
        uint32_t       *d = (uint32_t *)dst;
        uint32_t const *s = (uint32_t const *)src;
        for (int y = 0; y < rect.h; y++) {
            uint32_t const *srow = s + (size_t)(rect.y + y) * src_stride_px + rect.x;
            uint32_t       *drow = d + (size_t)y * rect.w;
            for (int x = 0; x < rect.w; x++) {
                uint32_t p = srow[x];
                drow[x]    = (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
            }
        }
    }
}
#endif

static void IRAM_ATTR NOINLINE_ATTR compositor(void *ignored) {
#if CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA
    static ppa_client_handle_t ppa_srm_handle = NULL;

    ppa_client_config_t ppa_srm_config = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };

    // ppa_event_callbacks_t srm_callbacks = {
    //     .on_trans_done = ppa_srm_callback,
    // };

    ppa_register_client(&ppa_srm_config, &ppa_srm_handle);
    // ppa_client_register_event_callbacks(ppa_srm_handle, &srm_callbacks);
#if CONFIG_CJ_BADGEVMS_COMPOSITOR_PPA_SW_RGB_SWAP
    // Max size any single block could need: a full 720x720 frame at 4 bytes/pixel.
    void *rgb_swap_scratch = heap_caps_malloc(FRAMEBUFFER_MAX_W * FRAMEBUFFER_MAX_H * 4, MALLOC_CAP_DEFAULT);
    if (!rgb_swap_scratch) {
        ESP_LOGE(TAG, "No memory for PPA rgb_swap scratch buffer -- falling back to hardware rgb_swap");
    }
#endif
#endif

    bool fn_down     = false;
    bool frame_ready = false;

    while (1) {
        bool changes   = false;
        int  processed = 0;
        ulTaskNotifyTakeIndexed(0, pdTRUE, portMAX_DELAY);

        if (frame_ready) {
            lcd_device->_draw(lcd_device, 0, 0, FRAMEBUFFER_MAX_W, FRAMEBUFFER_MAX_H, framebuffers[cur_fb]);
            cur_fb      = (cur_fb + 1) % DISPLAY_FRAMEBUFFERS;
            frame_ready = false;
        }

        compositor_message_t message;

        while (xQueueReceive(compositor_queue, &message, 0) == pdTRUE) {
            ++processed;
            switch (message.command) {
                case WINDOW_CREATE:
                    push_window(message.window);
                    mark_scene_damaged();
                    break;
                case WINDOW_DESTROY:
                    remove_window(message.window);
                    vQueueDelete(message.window->event_queue);

                    for (int i = 0; i < 2; ++i) {
                        ESP_LOGW(TAG, "Destroying framebuffer %u for window %p", i, message.window);
                        framebuffer_free(message.window->framebuffers[i]);
                    }

                    free(message.window->title);
                    free(message.window);
                    mark_scene_damaged();
                    break;
                case WINDOW_FLAGS:
                    // Preserve the double buffered flag
                    bool double_buffered  = message.window->flags & WINDOW_FLAG_DOUBLE_BUFFERED;
                    message.flags        &= ~WINDOW_FLAG_DOUBLE_BUFFERED;
                    if (double_buffered) {
                        message.flags |= WINDOW_FLAG_DOUBLE_BUFFERED;
                    }

                    if (message.window->flags & WINDOW_FLAG_FULLSCREEN) {
                        if (!(message.flags & WINDOW_FLAG_FULLSCREEN)) {
                            message.window->rect = message.window->rect_orig;
                        }
                    } else {
                        if (message.flags & WINDOW_FLAG_FULLSCREEN) {
                            message.window->rect_orig = message.window->rect;
                            message.window->rect.x    = 0;
                            message.window->rect.y    = 0;
                            message.window->rect.w    = FRAMEBUFFER_MAX_W;
                            message.window->rect.h    = FRAMEBUFFER_MAX_H;
                        }
                    }
                    message.window->flags = message.flags;
                    mark_scene_damaged();
                    break;
                case WINDOW_MOVE:
                    window_coords_t coords = window_clamp_position(message.window, message.coords);
                    message.window->rect.x = coords.x;
                    message.window->rect.y = coords.y;
                    mark_scene_damaged();
                    break;
                case WINDOW_RESIZE:
                    window_size_t size     = window_clamp_size(message.window, message.size);
                    message.window->rect.w = size.w;
                    message.window->rect.h = size.h;
                    mark_scene_damaged();
                    break;
                case FRAMEBUFFER_SWAP: framebuffer_swap(message.fb_a, message.fb_b); break;
                default: ESP_LOGE(TAG, "Unknown command %u", message.command);
            }

            if (message.caller) {
                if (eTaskGetState(message.caller) != eDeleted) {
                    xTaskNotifyGiveIndexed(message.caller, 0);
                }
            }

            if (processed > WINDOW_COMMANDS_PER_FRAME) {
                break;
            }
        }

        event_t events[KEYBOARD_EVENTS_PER_FRAME];
        ssize_t res = keyboard_device->_read(keyboard_device, 0, events, sizeof(event_t) * KEYBOARD_EVENTS_PER_FRAME);
        for (int i = 0; i < res / sizeof(event_t); ++i) {
            event_t *c = &events[i];
            if (c->keyboard.scancode == KEY_SCANCODE_FN) {
                if (c->keyboard.down) {
                    fn_down = true;
                } else {
                    fn_down = false;
                }
                // Hide the FN key
                c->type = EVENT_NONE;
            }

            if (window_stack) {
                ESP_LOGV(TAG, "Got scancode %02X mods %02X", c->keyboard.scancode, c->keyboard.mod);
                if (c->keyboard.scancode == KEY_SCANCODE_TAB && c->keyboard.mod & BADGEVMS_KMOD_LALT &&
                    c->keyboard.down) {
                    if (window_stack->next->title) {
                        ESP_LOGW(
                            TAG,
                            "ALT-TAB switching to window %p (%s)",
                            window_stack->next,
                            window_stack->next->title
                        );
                    } else {
                        ESP_LOGW(TAG, "ALT-TAB switching to window %p (no title)", window_stack->next);
                    }
                    window_stack          = window_stack->next;
                    c->type               = EVENT_NONE;
                    // No need to redraw the background
                    visible_regions_valid = false;
                    decoration_damaged    = 7;
                }

                if (fn_down) {
                    if (c->keyboard.down) {
                        window_coords_t cur_pos = {
                            .x = window_stack->rect.x,
                            .y = window_stack->rect.y,
                        };
                        switch (c->keyboard.scancode) {
                            case KEY_SCANCODE_UP:
                                cur_pos.y -= WINDOW_MOVE_STEP;
                                mark_scene_damaged();
                                break;
                            case KEY_SCANCODE_DOWN:
                                cur_pos.y += WINDOW_MOVE_STEP;
                                mark_scene_damaged();
                                break;
                            case KEY_SCANCODE_LEFT:
                                cur_pos.x -= WINDOW_MOVE_STEP;
                                mark_scene_damaged();
                                break;
                            case KEY_SCANCODE_RIGHT:
                                cur_pos.x += WINDOW_MOVE_STEP;
                                mark_scene_damaged();
                                break;
                            case KEY_SCANCODE_CROSS:
                                window_t    *window    = window_stack;
                                task_info_t *task_info = (task_info_t *)atomic_load(&window->task_info);
                                remove_window(window);
                                // So we don't end up deleting this window twice
                                window->next = NULL;
                                window->prev = NULL;

                                if (task_info) {
                                    if (eTaskGetState(task_info->handle) != eDeleted) {
                                        vTaskDelete(task_info->handle);
                                    }
                                }
                                mark_scene_damaged();
                                continue;
                            default:
                        }
                        cur_pos              = window_clamp_position(window_stack, cur_pos);
                        window_stack->rect.x = cur_pos.x;
                        window_stack->rect.y = cur_pos.y;
                    }
                }

                if (!fn_down && c->type != EVENT_NONE) {
                    if (xQueueSend(window_stack->event_queue, c, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "Unable to send event to task");
                    }
                }
            }
        }

        bool framebuffer_cleared = false;
        if (background_damaged & (1 << cur_fb)) {
            memset(framebuffers[cur_fb], 0xaa, FRAMEBUFFER_BYTES);
            // Make sure the ppa will see our new background
            esp_cache_msync(
                framebuffers[cur_fb],
                FRAMEBUFFER_BYTES,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE
            );
            background_damaged  &= ~(1 << cur_fb);
            changes              = true;
            framebuffer_cleared  = true;
        }

        if (window_stack) {
            window_t *window = window_stack->prev; // Start with back window

            do {
                task_info_t *task_info = (task_info_t *)atomic_load(&window->task_info);
                if (!task_info) {
                    remove_window(window);
                    break;
                }

                if (eTaskGetState(task_info->handle) != eDeleted) {
                    if ((window == window_stack) && (window_stack->flags & WINDOW_FLAG_FULLSCREEN) &&
                        (!(window_stack->flags & WINDOW_FLAG_LOW_PRIORITY))) {
                        // A foreground full-screen app gets as much CPU time as it can handle
                        vTaskPrioritySet(task_info->handle, TASK_PRIORITY_FOREGROUND);
                    } else {
                        vTaskPrioritySet(task_info->handle, TASK_PRIORITY);
                    }
                }

                managed_framebuffer_t *framebuffer = window->framebuffers[window->front_fb];

                if (!framebuffer) {
                    // Not yet allocated, or in the process of being destroyed
                    if (!visible_regions_valid) {
                        window_calculate_visible_regions(window, window_stack, 1.0);
                    }
                    window = window->prev;
                    continue;
                }

                float scale_x = ((float)window->rect.w / (float)framebuffer->w);
                float scale_y = ((float)window->rect.h / (float)framebuffer->h);
                float scale   = fminf(scale_x, scale_y);

                if (!visible_regions_valid) {
                    window_calculate_visible_regions(window, window_stack, scale);
                }

                bool is_clean             = atomic_flag_test_and_set(&framebuffer->clean);
                bool need_decoration_draw = decoration_damaged & (1 << cur_fb);
                bool need_content_draw    = false;

                if (is_clean) {
                    if (window->fb_dirty & (1 << cur_fb)) {
                        need_content_draw = true;
                    }
                } else {
                    window->fb_dirty  = 7;
                    need_content_draw = true;
                }

                if (framebuffer_cleared || window == window_stack) {
                    need_decoration_draw = true;
                    need_content_draw    = true;
                }

                if (need_content_draw) {
#if CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA
                    ppa_srm_rotation_angle_t ppa_rotation = rotation_to_srm(rotation);
                    bool                     rgb_swap     = false;
                    bool                     byte_swap    = false;
                    ppa_srm_color_mode_t     mode         = PPA_SRM_COLOR_MODE_RGB565;

                    if (window->flags & WINDOW_FLAG_FLIP_HORIZONTAL) {
                        ppa_rotation = PPA_SRM_ROTATION_ANGLE_270;
                    }

                    switch (framebuffer->format) {
                        case BADGEVMS_PIXELFORMAT_RGB565: rgb_swap = true; // Fallthrough
                        case BADGEVMS_PIXELFORMAT_BGR565: break;
                        case BADGEVMS_PIXELFORMAT_BGRA8888: rgb_swap = true; // Fallthrough
                        case BADGEVMS_PIXELFORMAT_RGBA8888: mode = PPA_SRM_COLOR_MODE_ARGB8888; break;
                        case BADGEVMS_PIXELFORMAT_ARGB8888: rgb_swap = true; // Fallthrough
                        case BADGEVMS_PIXELFORMAT_ABGR8888: mode = PPA_SRM_COLOR_MODE_ARGB8888; break;
                        default:
                    }

#if CONFIG_CJ_BADGEVMS_COMPOSITOR_PPA_SW_RGB_SWAP
                    bool do_sw_rgb_swap = rgb_swap && rgb_swap_scratch;
                    if (do_sw_rgb_swap) {
                        rgb_swap = false; // done on the CPU below instead -- see Kconfig help text
                    }
#endif

                    // PPA DMA-reads this buffer below; flush the CPU cache first so it sees the
                    // app's latest writes. Hardware-tested 2026-08-12/13: this alone doesn't fix
                    // the known PPA color bug (see CJ_BADGEVMS_COMPOSITOR_USE_PPA's Kconfig help
                    // text), but it's a real correctness fix on its own merits regardless, so it
                    // stays unconditional rather than behind a diagnostic flag.
                    esp_cache_msync(
                        framebuffer->framebuffer.pixels,
                        (size_t)framebuffer->w * framebuffer->h * BADGEVMS_BYTESPERPIXEL(framebuffer->format),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M
                    );

                    for (int i = 0; i < window->visible.count; i++) {
                        window_rect_t visible_content = window->visible.rects[i];
                        window_rect_t fb_rect         = content_to_framebuffer_rect(visible_content, window, scale);

                        if (fb_rect.w <= 0 || fb_rect.h <= 0) {
                            continue;
                        }

                        window_rect_t rotated_output = rotate_rect(visible_content, rotation);

                        void *ppa_in_buffer      = framebuffer->framebuffer.pixels;
                        int   ppa_in_pic_w       = framebuffer->w;
                        int   ppa_in_pic_h       = framebuffer->h;
                        int   ppa_in_block_off_x = fb_rect.x;
                        int   ppa_in_block_off_y = fb_rect.y;

#if CONFIG_CJ_BADGEVMS_COMPOSITOR_PPA_SW_RGB_SWAP
                        if (do_sw_rgb_swap) {
                            int bpp = BADGEVMS_BYTESPERPIXEL(framebuffer->format);
                            copy_swap_rb_rect(
                                rgb_swap_scratch,
                                framebuffer->framebuffer.pixels,
                                framebuffer->w,
                                bpp,
                                fb_rect
                            );
                            esp_cache_msync(
                                rgb_swap_scratch,
                                (size_t)fb_rect.w * fb_rect.h * bpp,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M
                            );
                            ppa_in_buffer      = rgb_swap_scratch;
                            ppa_in_pic_w       = fb_rect.w;
                            ppa_in_pic_h       = fb_rect.h;
                            ppa_in_block_off_x = 0;
                            ppa_in_block_off_y = 0;
                        }
#endif

                        ppa_srm_oper_config_t oper_config = {
                            .in.buffer         = ppa_in_buffer,
                            .in.pic_w          = ppa_in_pic_w,
                            .in.pic_h          = ppa_in_pic_h,
                            .in.block_w        = fb_rect.w,
                            .in.block_h        = fb_rect.h,
                            .in.block_offset_x = ppa_in_block_off_x,
                            .in.block_offset_y = ppa_in_block_off_y,
                            .in.srm_cm         = mode,

                            .out.buffer         = framebuffers[cur_fb],
                            .out.buffer_size    = FRAMEBUFFER_BYTES,
                            .out.pic_w          = FRAMEBUFFER_MAX_W,
                            .out.pic_h          = FRAMEBUFFER_MAX_H,
                            .out.block_offset_x = rotated_output.x,
                            .out.block_offset_y = rotated_output.y,
                            .out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565,

                            .rotation_angle = ppa_rotation,
                            .scale_x        = scale,
                            .scale_y        = scale,
                            .rgb_swap       = rgb_swap,
                            .byte_swap      = byte_swap,
                            .mode           = PPA_TRANS_MODE_BLOCKING,
                        };

                        esp_err_t ppa_result = ppa_do_scale_rotate_mirror(ppa_srm_handle, &oper_config);
                        if (ppa_result != ESP_OK) {
                            printf("PPA operation failed: %s\n", esp_err_to_name(ppa_result));
                        } else {
                            changes           = true;
                            window->fb_dirty &= ~(1 << cur_fb);
                        }
                    }
#else  // !CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA
       // CPU replacement for the PPA path above -- see
       // pixel_functions.c's blit_rect_rotated() and
       // docs/tanmatsu-launcher-port-analysis.md for why.
                    if (window->flags & WINDOW_FLAG_FLIP_HORIZONTAL) {
                        // Not implemented on this path: the old PPA path got
                        // this "for free" by abusing its own rotation_angle
                        // field, which has no equivalent here. No shipped app
                        // sets this flag today -- see the port-analysis doc.
                        ESP_LOGW(TAG, "WINDOW_FLAG_FLIP_HORIZONTAL is not supported by the CPU compositor path");
                    }

                    // Same cache-sync bracketing draw_window_box() already
                    // does below for the same DMA-backed framebuffers[cur_fb]
                    // -- the CPU blit writes with plain stores same as
                    // decoration drawing does, not a DMA engine that handles
                    // coherency itself the way the PPA did.
                    esp_cache_msync(framebuffers[cur_fb], FRAMEBUFFER_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

                    for (int i = 0; i < window->visible.count; i++) {
                        window_rect_t visible_content = window->visible.rects[i];
                        window_rect_t fb_rect         = content_to_framebuffer_rect(visible_content, window, scale);

                        if (fb_rect.w <= 0 || fb_rect.h <= 0) {
                            continue;
                        }

                        blit_rect_rotated(
                            framebuffers[cur_fb],
                            framebuffer->framebuffer.pixels,
                            framebuffer->w,
                            framebuffer->format,
                            fb_rect,
                            visible_content,
                            scale,
                            rotation
                        );

                        changes           = true;
                        window->fb_dirty &= ~(1 << cur_fb);
                    }

                    esp_cache_msync(
                        framebuffers[cur_fb],
                        FRAMEBUFFER_BYTES,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE
                    );
#endif // CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA

                    // Notify app that content was processed
                    if (!is_clean) {
                        if (eTaskGetState(task_info->handle) != eDeleted) {
                            xTaskNotifyGiveIndexed(task_info->handle, 1);
                        }
                    }
                }

                if (need_decoration_draw && !(window->flags & WINDOW_FLAG_FULLSCREEN)) {
                    // Cache sync before drawing decorations
                    esp_cache_msync(framebuffers[cur_fb], FRAMEBUFFER_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

                    draw_window_box(framebuffers[cur_fb], window, window == window_stack);

                    // Cache sync after drawing decorations
                    esp_cache_msync(
                        framebuffers[cur_fb],
                        FRAMEBUFFER_BYTES,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE
                    );
                    changes = true;
                }

                window = window->prev;
            } while (window != window_stack->prev);

            // Mark decorations as clean for this framebuffer
            decoration_damaged    &= ~(1 << cur_fb);
            visible_regions_valid  = true;
        }

        if (changes) {
            frame_ready = true;
        }
    }
}

size_t compositor_window_count(void) {
    return (size_t)atomic_load(&cur_num_windows);
}

void get_screen_info(int *width, int *height, pixel_format_t *format, float *refresh_rate) {
    *width        = FRAMEBUFFER_MAX_W;
    *height       = FRAMEBUFFER_MAX_H;
    *format       = BADGEVMS_PIXELFORMAT_RGB565;
    *refresh_rate = FRAMEBUFFER_MAX_REFRESH;
}

static managed_framebuffer_t *
    window_framebuffer_allocate(window_t *window, window_size_t size, pixel_format_t pixel_format) {
    size.w = size.w > FRAMEBUFFER_MAX_W ? FRAMEBUFFER_MAX_W : size.w;
    size.h = size.h > FRAMEBUFFER_MAX_H ? FRAMEBUFFER_MAX_H : size.h;

    size              = window_clamp_size(window, size);
    framebuffer_t *fb = framebuffer_allocate(size.w, size.h, pixel_format);
    if (!fb) {
        ESP_LOGW(TAG, "Unable to allocate framebuffer");
        return NULL;
    }

    return (managed_framebuffer_t *)fb;
}

framebuffer_t *window_framebuffer_create(window_t *window, window_size_t size, pixel_format_t pixel_format) {
    if (!window) {
        return NULL;
    }

    if (window->framebuffers[0] || window->framebuffers[1]) {
        return NULL;
    }
    window->front_fb = 0;
    window->back_fb  = 0;

    window->framebuffers[0] = window_framebuffer_allocate(window, size, pixel_format);
    if (!window->framebuffers[0]) {
        ESP_LOGW(
            TAG,
            "Unable to allocate framebuffer 0 for window %p, task %u",
            window,
            ((task_info_t *)window->task_info)->pid
        );
        return NULL;
    }

    if (window->flags & WINDOW_FLAG_DOUBLE_BUFFERED) {
        window->framebuffers[1] = window_framebuffer_allocate(window, size, pixel_format);
        if (!window->framebuffers[1]) {
            ESP_LOGW(TAG, "Unable to allocate framebuffer 1 for window %p", window);
            framebuffer_free(window->framebuffers[0]);
            window->framebuffers[0] = NULL;
            return NULL;
        }
        window->back_fb = 1;
    }

    window->fb_dirty = 7;

    return (framebuffer_t *)window->framebuffers[window->back_fb];
}

window_t *window_create(char const *title, window_size_t size, window_flag_t flags) {
    if (atomic_load(&cur_num_windows) >= MAX_WINDOWS) {
        return NULL;
    }

    task_info_t *task_info = get_task_info();

    window_t *window = calloc(1, sizeof(window_t));
    if (!window) {
        ESP_LOGW(TAG, "Unable to allocate window");
        goto error;
    }

    if (title) {
        window_title_set(window, title);
        if (!window->title) {
            ESP_LOGW(TAG, "Unable to allocate window title");
            goto error;
        }
    }

    window->event_queue = xQueueCreate(WINDOW_MAX_EVENTS, sizeof(event_t));
    if (!window->event_queue) {
        ESP_LOGW(TAG, "Out of memory trying to allocate window event queue");
        goto error;
    }

    window->flags  = flags;
    window->rect.x = 0;
    window->rect.y = 0;
    size           = window_clamp_size(window, size);
    window->rect.w = size.w;
    window->rect.h = size.h;

    atomic_store(&window->task_info, (uintptr_t)task_info);

    task_record_resource_alloc(RES_WINDOW, window);

    compositor_message_t message = {
        .command = WINDOW_CREATE,
        .window  = window,
        .caller  = xTaskGetCurrentTaskHandle(),
    };

    xQueueSend(compositor_queue, &message, portMAX_DELAY);
    ulTaskNotifyTakeIndexed(0, pdTRUE, portMAX_DELAY);

    return window;
error:
    free(window->title);
    free(window);
    return NULL;
}

void window_destroy_task(window_t *window) {
    if (!window) {
        return;
    }

    ESP_LOGI(TAG, "Destroying window %p\n", window);
    atomic_store(&window->task_info, (uintptr_t)NULL);

    compositor_message_t message = {
        .command = WINDOW_DESTROY,
        .window  = window,
    };

    xQueueSend(compositor_queue, &message, portMAX_DELAY);
}

void window_destroy(window_t *window) {
    if (!window) {
        return;
    }

    ESP_LOGI(TAG, "Destroying window %p\n", window);
    atomic_store(&window->task_info, (uintptr_t)NULL);

    compositor_message_t message = {
        .command = WINDOW_DESTROY,
        .window  = window,
        .caller  = xTaskGetCurrentTaskHandle(),
    };

    xQueueSend(compositor_queue, &message, portMAX_DELAY);
    ulTaskNotifyTakeIndexed(0, pdTRUE, portMAX_DELAY);

    task_record_resource_free(RES_WINDOW, window);
}

char const *window_title_get(window_t *window) {
    return window->title;
}

void window_title_set(window_t *window, char const *title) {
    // window->title is either NULL or a string from strndup()
    free(window->title);
    if (title) {
        window->title = strndup(title, 20);
        if (!window->title) {
            ESP_LOGW(TAG, "Unable to allocate window title");
        }
    }
}

window_coords_t window_position_get(window_t *window) {
    window_coords_t ret = {
        .x = window->rect.x,
        .y = window->rect.y,
    };
    return ret;
}

window_coords_t window_position_set(window_t *window, window_coords_t coords) {
    if (!window) {
        return (window_coords_t){0, 0};
    }

    compositor_message_t message = {
        .command = WINDOW_MOVE,
        .coords  = coords,
        .window  = window,
        .caller  = xTaskGetCurrentTaskHandle(),
    };

    xQueueSend(compositor_queue, &message, portMAX_DELAY);
    ulTaskNotifyTakeIndexed(0, pdTRUE, portMAX_DELAY);

    return window_position_get(window);
}

window_size_t window_size_get(window_t *window) {
    window_size_t ret = {
        .w = window->rect.w,
        .h = window->rect.h,
    };
    return ret;
}

window_size_t window_size_set(window_t *window, window_size_t size) {
    if (!window) {
        return (window_size_t){0, 0};
    }

    compositor_message_t message = {
        .command = WINDOW_RESIZE,
        .size    = size,
        .window  = window,
        .caller  = xTaskGetCurrentTaskHandle(),
    };

    xQueueSend(compositor_queue, &message, portMAX_DELAY);
    ulTaskNotifyTakeIndexed(0, pdTRUE, portMAX_DELAY);

    return window_size_get(window);
}

window_flag_t window_flags_get(window_t *window) {
    return window->flags;
}

window_flag_t window_flags_set(window_t *window, window_flag_t flags) {
    compositor_message_t message = {
        .command = WINDOW_FLAGS,
        .window  = window,
        .flags   = flags,
        .caller  = xTaskGetCurrentTaskHandle(),
    };

    xQueueSend(compositor_queue, &message, portMAX_DELAY);
    ulTaskNotifyTakeIndexed(0, pdTRUE, portMAX_DELAY);
    return flags;
}

framebuffer_t *window_framebuffer_get(window_handle_t window) {
    if (!window) {
        return NULL;
    }

    return (framebuffer_t *)window->framebuffers[window->back_fb];
}

window_size_t window_framebuffer_size_get(window_handle_t window) {
    if (!(window && window->framebuffers[0])) {
        return (window_size_t){0, 0};
    }

    return (window_size_t){window->framebuffers[0]->w, window->framebuffers[0]->h};
}

window_size_t window_framebuffer_size_set(window_handle_t window, window_size_t size) {
    return window_framebuffer_size_get(window);
}

pixel_format_t window_framebuffer_format_get(window_handle_t window) {
    if (!(window && window->framebuffers[0])) {
        return 0;
    }

    return window->framebuffers[0]->format;
}

#if 0
static inline void copy_framebuffer_rect(managed_framebuffer_t *dst, managed_framebuffer_t *src, window_rect_t rect) {
    if (rect.x == 0 && rect.y == 0 && rect.w == dst->w && rect.h == dst->h) {
        // Full rect damage
        memcpy(dst->framebuffer.pixels, src->framebuffer.pixels, dst->num_pages * SOC_MMU_PAGE_SIZE);
        return;
    }

    int max_x = MIN(dst->w, src->w);
    int max_y = MIN(dst->h, src->h);

    rect.x = MAX(0, MIN(rect.x, max_x));
    rect.y = MAX(0, MIN(rect.y, max_y));
    rect.w = MIN(rect.w, max_x - rect.x);
    rect.h = MIN(rect.h, max_y - rect.y);

    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    int      bytes_per_pixel = BADGEVMS_BYTESPERPIXEL(dst->format);
    uint8_t *dst_pixels      = (uint8_t *)dst->framebuffer.pixels;
    uint8_t *src_pixels      = (uint8_t *)src->framebuffer.pixels;

    int dst_stride = dst->w * bytes_per_pixel;
    int src_stride = src->w * bytes_per_pixel;
    int copy_width = rect.w * bytes_per_pixel;

    for (int y = 0; y < rect.h; y++) {
        uint8_t *dst_row = dst_pixels + ((rect.y + y) * dst_stride) + (rect.x * bytes_per_pixel);
        uint8_t *src_row = src_pixels + ((rect.y + y) * src_stride) + (rect.x * bytes_per_pixel);
        memcpy(dst_row, src_row, copy_width);
    }
}
#endif

void window_present(window_t *window, bool block, window_rect_t *rects, int num_rects) {
    if (!window || !window->framebuffers[0]) {
        return;
    }

    managed_framebuffer_t *front_buffer = NULL;
    managed_framebuffer_t *back_buffer  = NULL;

    if (window->flags & WINDOW_FLAG_DOUBLE_BUFFERED && window->framebuffers[1]) {
        front_buffer = window->framebuffers[window->front_fb];
        back_buffer  = window->framebuffers[window->back_fb];

        if (!front_buffer || !back_buffer) {
            why_die("window_present: front/back framebuffer missing on a double-buffered window");
        }

        compositor_message_t message = {
            .command = FRAMEBUFFER_SWAP,
            .fb_a    = front_buffer,
            .fb_b    = back_buffer,
            .caller  = xTaskGetCurrentTaskHandle(),
        };

        xQueueSend(compositor_queue, &message, portMAX_DELAY);
        ulTaskNotifyTakeIndexed(0, pdTRUE, portMAX_DELAY);

        // We've waited long enough
        block = false;
    } else {
        front_buffer = window->framebuffers[window->front_fb];
    }

    atomic_flag_clear(&front_buffer->clean);

    if (block) {
        ulTaskNotifyTakeIndexed(1, pdTRUE, portMAX_DELAY);
    }
}

event_t window_event_poll(window_t *window, bool block, uint32_t timeout_msec) {
    event_t    e;
    TickType_t wait = block ? portMAX_DELAY : timeout_msec / portTICK_PERIOD_MS;

    if (xQueueReceive(window->event_queue, &e, wait) != pdTRUE) {
        e.type = EVENT_NONE;
    }

    return e;
}

bool compositor_init(char const *lcd_device_name, char const *keyboard_device_name) {
    ESP_LOGI(TAG, "Initializing");

    lcd_device = (lcd_device_t *)device_get(lcd_device_name);
    if (!lcd_device) {
        ESP_LOGE(TAG, "Unable to access the LCD device '%s'", lcd_device_name);
        return false;
    }

    keyboard_device = (device_t *)device_get(keyboard_device_name);
    if (!keyboard_device) {
        ESP_LOGE(TAG, "Unable to access the keyboard device '%s'", keyboard_device_name);
        return false;
    }

    for (int i = 0; i < DISPLAY_FRAMEBUFFERS; ++i) {
        lcd_device->_getfb(lcd_device, i, (void *)&framebuffers[i]);
        memset(framebuffers[i], 0xaa, FRAMEBUFFER_BYTES);
        esp_cache_msync(framebuffers[i], FRAMEBUFFER_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        ESP_LOGW(TAG, "Got framebuffer[%i]: %p", i, framebuffers[i]);
    }

    cur_fb = (cur_fb + 1) % DISPLAY_FRAMEBUFFERS;

    lcd_device->_set_refresh_cb(lcd_device, NULL, on_refresh);

    compositor_queue = xQueueCreate(10, sizeof(compositor_message_t));
    /* Core 1, not 0: this used to be safe pinned to core 0 because the PPA
     * hardware did the actual pixel work off-CPU, so this task mostly just
     * queued PPA commands and waited. Now that CONFIG_CJ_BADGEVMS_COMPOSITOR_
     * USE_PPA=n makes it do real CPU-side blit_rect_rotated() work every
     * frame, keeping it on core 0 starves badgevms/drivers/led_matrix_pca9698.c's
     * mtx_refresh_task_hw -- that task has no vTaskDelay in its per-row I2C
     * loop and depends on tight, undisturbed core-0 scheduling for its
     * ~460 Hz refresh (see the "no flicker" comment in led_matrix_pca9698.c);
     * losing that turns its multiplexed row-scan into visible flicker.
     *
     * Priority also has to drop on the CPU path, not just the core. App
     * processes run at priority 5 (task.c's zeus()); this task was priority
     * 20 from day one, which was fine when it barely touched the CPU (PPA
     * hardware did the work). A strictly higher priority than every app
     * means FreeRTOS never preempts it for a lower-priority app no matter
     * how long it runs -- taskYIELD() inside this task wouldn't have helped,
     * since yielding only matters when something of equal-or-higher priority
     * is ready. With real per-frame CPU work now sharing core 1 with app
     * processes, that turned into outright starvation, not "ordinary jank"
     * as an earlier version of this comment assumed: a busy screen (several
     * windows/rects queued at once) could keep this task runnable long
     * enough to freeze every app on core 1 -- including input handling,
     * which is why a stuck app looked unresponsive to the keyboard too, not
     * just visually static. Matching the app priority (5) lets FreeRTOS's
     * equal-priority round-robin time-slicing interleave rendering with app
     * work instead of one strictly dominating the other. Only the CPU path
     * needs this: the PPA path's original priority-20/core-0 pairing is left
     * alone since it never had this problem. */
#if CONFIG_CJ_BADGEVMS_COMPOSITOR_USE_PPA
    create_kernel_task(compositor, "Compositor", 8192, NULL, 20, &compositor_handle, 1);
#else
    /* Priority 5, matching app-process priority -- NOT a tunable knob for
     * "smoother but still safe" rendering. Tried priority 8 as an
     * experiment (hardware-tested 2026-08-12): still above app priority,
     * so FreeRTOS still never preempts this task for an app no matter how
     * small the gap -- confirmed on hardware to reproduce the same
     * starvation as priority 20, just manifesting as a black screen
     * (stuck even before the first frame) rather than a static spinner.
     * Equal priority is the only value confirmed safe: it's what makes
     * FreeRTOS's round-robin time-slicing actually interleave this task
     * with app work, instead of one strictly dominating the other. Do not
     * raise this without also giving the CPU render path real internal
     * yield points (breaking a multi-window redraw into smaller chunks) --
     * priority alone cannot buy back throughput here. */
    create_kernel_task(compositor, "Compositor", 8192, NULL, 5, &compositor_handle, 1);
#endif

    return true;
}
