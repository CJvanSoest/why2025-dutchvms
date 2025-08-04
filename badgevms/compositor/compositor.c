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
#include "badgevms_config.h"
#include "compositor_private.h"
#include "driver/ppa.h"
#include "esp_cache.h"
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

static TaskHandle_t  compositor_handle;
static lcd_device_t *lcd_device;
static device_t     *keyboard_device;

static window_t         *window_stack      = NULL;
static SemaphoreHandle_t window_stack_lock = NULL;
static SemaphoreHandle_t vsync             = NULL;
static SemaphoreHandle_t ppa               = NULL;
static QueueHandle_t     compositor_queue;

static int        cur_fb = 1;
static atomic_int cur_num_windows;
static uint16_t  *framebuffers[3];

static int  background_damaged    = 7;
static bool visible_regions_valid = false;

typedef enum { WINDOW_CREATE, WINDOW_DESTROY, WINDOW_FLAGS, FRAMEBUFFER_FREE } compositor_command_t;

typedef struct {
    compositor_command_t command;
    window_t            *window;
    window_flag_t        flags;
    int                  fb_num;
    TaskHandle_t         caller;
} compositor_message_t;

rotation_angle_t rotation = ROTATION_ANGLE_270;

#define WINDOW_MAX_W (FRAMEBUFFER_MAX_W - (2 * BORDER_PX) - SIDE_BAR_PX)
#define WINDOW_MAX_H (FRAMEBUFFER_MAX_H - BORDER_TOP_PX - TOP_BAR_PX)

__attribute__((always_inline)) static inline ppa_srm_rotation_angle_t rotation_to_srm(rotation_angle_t rotation) {
    switch (rotation) {
        case ROTATION_ANGLE_270: return PPA_SRM_ROTATION_ANGLE_90;
        case ROTATION_ANGLE_180: return PPA_SRM_ROTATION_ANGLE_180;
        case ROTATION_ANGLE_90: return PPA_SRM_ROTATION_ANGLE_270;
        default:
    }
    return PPA_SRM_ROTATION_ANGLE_0;
}

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
        int max_x = FRAMEBUFFER_MAX_W - (window->rect.w + (2 * BORDER_PX)) - 1;
        int max_y = FRAMEBUFFER_MAX_H - (window->rect.h + (BORDER_TOP_PX + BORDER_PX)) - 1;
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
    atomic_fetch_sub(&cur_num_windows, 1);
}

// Workaround for the PPA hardware. It really does not like 65 pixel high strips.
__attribute__((always_inline)) static inline bool is_problematic_block_height(int content_height, float scale) {
    // Check if height is "N × 32 + 1"
    int fb_height = (int)(content_height / scale);
    if (fb_height > 32 && (fb_height % 32) == 1) {
        return true;
    }
    return false;
}

__attribute__((always_inline)) static inline bool ppa_workaround_split_rects(rect_array_t *visible, float scale) {
    rect_array_t new_visible = {0};
    bool         split       = false;

    for (int i = 0; i < visible->count; i++) {
        window_rect_t rect = visible->rects[i];

        if (is_problematic_block_height(rect.h, scale)) {
            split           = true;
            int first_half  = (rect.h / 2) - 1;
            int second_half = rect.h - first_half;
            ESP_LOGW(TAG, "Splitting block of problematic height %u in %u and %u", rect.h, first_half, second_half);

            new_visible.rects[new_visible.count++] =
                (window_rect_t){.x = rect.x, .y = rect.y, .w = rect.w, .h = first_half};

            if (new_visible.count < MAX_VISIBLE_RECTS) {
                new_visible.rects[new_visible.count++] =
                    (window_rect_t){.x = rect.x, .y = rect.y + first_half, .w = rect.w, .h = second_half};
            }
        } else {
            if (new_visible.count < MAX_VISIBLE_RECTS) {
                new_visible.rects[new_visible.count++] = rect;
            }
        }
    }

    *visible = new_visible;
    return split;
}

void window_calculate_visible_regions(window_t *window, window_t *window_list_head, float scale) {
    window->visible.count    = 1;
    window->visible.rects[0] = window->rect;

    if (!(window->flags & WINDOW_FLAG_FULLSCREEN)) {
        window->visible.rects[0].x += BORDER_PX;
        window->visible.rects[0].y += BORDER_TOP_PX;
    }

    window_t *occluder = window_list_head;

    while (occluder != NULL && occluder != window) {
        rect_array_t new_visible = {0};

        for (int j = 0; j < window->visible.count; j++) {
            window_rect_t occluder_rect = occluder->rect;
            if (!(occluder->flags & WINDOW_FLAG_FULLSCREEN)) {
                occluder_rect.x += BORDER_PX;
                occluder_rect.y += BORDER_TOP_PX;
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
    while (ppa_workaround_split_rects(&window->visible, scale)) {
    }
}

window_rect_t content_to_framebuffer_rect(window_rect_t content_rect, window_t *window, float scale) {
    if (!(window->flags & WINDOW_FLAG_FULLSCREEN)) {
        content_rect.x -= (window->rect.x + BORDER_PX);
        content_rect.y -= (window->rect.y + BORDER_TOP_PX);
    }

    managed_framebuffer_t *framebuffer = window->framebuffers[window->cur_fb];

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

framebuffer_t *framebuffer_allocate(uint32_t w, uint32_t h, pixel_format_t format) {
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
    if (!pages_allocate(vaddr_start, num_pages - 1, &framebuffer->pages, &tail_range)) {
        ESP_LOGE(TAG, "No physical memory pages for frame buffer");
        framebuffer_vaddr_deallocate(vaddr_start);
        free(framebuffer);
        return NULL;
    }

    ESP_LOGI(
        TAG,
        "Attempting to map our vaddr %p head_range %p tail_range %p",
        (void *)framebuffer->pages->vaddr_start,
        framebuffer->pages,
        tail_range
    );

    framebuffer_map_pages(framebuffer->pages, tail_range);
    framebuffer->framebuffer.pixels = (uint16_t *)vaddr_start;

    framebuffer->framebuffer.w      = w;
    framebuffer->framebuffer.h      = h;
    framebuffer->framebuffer.format = format;
    // Keep a copy that the user can't easily change
    framebuffer->w                  = w;
    framebuffer->h                  = h;
    framebuffer->format             = format;
    atomic_flag_test_and_set(&framebuffer->clean);
    atomic_store_explicit(&framebuffer->active, true, memory_order_release);

    // vTaskPrioritySet(framebuffer->task_info->handle, TASK_PRIORITY_FOREGROUND);
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
        atomic_store_explicit(&framebuffer->active, false, memory_order_release);

        framebuffer_unmap_pages(framebuffer->pages);
        pages_deallocate(framebuffer->pages);
        framebuffer_vaddr_deallocate((uintptr_t)framebuffer->framebuffer.pixels);

        free(framebuffer);
    }
}

IRAM_ATTR static void on_refresh(void *ignored) {
    xSemaphoreGive(vsync);

    // if (compositor_initialized) {
    // }
}

// static bool IRAM_ATTR ppa_srm_callback(ppa_client_handle_t ppa_client, ppa_event_data_t *event_data, void *user_data)
// {
//     xTaskNotifyGiveIndexed(compositor_handle, 0);
//     return true;
// }

static void IRAM_ATTR NOINLINE_ATTR compositor(void *ignored) {
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

    bool fn_down = false;

    while (1) {
        bool changes   = false;
        int  processed = 0;
        xSemaphoreTake(vsync, portMAX_DELAY);

        if (xSemaphoreTake(window_stack_lock, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to get window list mutex");
            abort();
        }

        compositor_message_t message;

        while (xQueueReceive(compositor_queue, &message, 0) == pdTRUE) {
            ++processed;
            switch (message.command) {
                case WINDOW_CREATE: push_window(message.window); break;
                case WINDOW_DESTROY:
                    remove_window(message.window);
                    vQueueDelete(message.window->event_queue);

                    for (int i = 0; i < WINDOW_MAX_FRAMEBUFFER; ++i) {
                        ESP_LOGW(TAG, "Destroying framebuffer %u for window %p", i, message.window);
                        framebuffer_free(message.window->framebuffers[i]);
                    }

                    free(message.window->title);
                    free(message.window);
                    break;
                case WINDOW_FLAGS:
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
                    break;
                case FRAMEBUFFER_FREE:
                    managed_framebuffer_t *fb =
                        (managed_framebuffer_t *)window_framebuffer_get(message.window, message.fb_num);
                    if (fb) {
                        message.window->cur_fb = message.fb_num - 1 > 0 ? message.fb_num - 1 : 0;
                        message.window->num_fb--;
                        message.window->framebuffers[message.fb_num] = NULL;
                        bool is_clean                                = atomic_flag_test_and_set(&fb->clean);
                        if (!is_clean) {
                            xTaskNotifyGiveIndexed(message.window->task_info->handle, 1);
                        }
                        framebuffer_free(fb);
                    }
                    break;
                default: ESP_LOGE(TAG, "Unknown command %u", message.command);
            }
            visible_regions_valid = false;
            background_damaged    = 7;
            xTaskNotifyGiveIndexed(message.caller, 0);
            if (processed > 5)
                break;
        }

        event_t events[10];
        ssize_t res = keyboard_device->_read(keyboard_device, 0, events, sizeof(event_t) * 10);
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
                    visible_regions_valid = false;
                }

                if (fn_down) {
                    if (c->keyboard.down) {
                        window_coords_t cur_pos = {
                            .x = window_stack->rect.x,
                            .y = window_stack->rect.y,
                        };
                        switch (c->keyboard.scancode) {
                            case KEY_SCANCODE_UP:
                                cur_pos.y             -= 10;
                                background_damaged     = 7;
                                visible_regions_valid  = false;
                                break;
                            case KEY_SCANCODE_DOWN:
                                cur_pos.y             += 10;
                                background_damaged     = 7;
                                visible_regions_valid  = false;
                                break;
                            case KEY_SCANCODE_LEFT:
                                cur_pos.x             -= 10;
                                background_damaged     = 7;
                                visible_regions_valid  = false;
                                break;
                            case KEY_SCANCODE_RIGHT:
                                cur_pos.x             += 10;
                                background_damaged     = 7;
                                visible_regions_valid  = false;
                                break;
                            case KEY_SCANCODE_CROSS:
                                task_info_t *task_info = window_stack->task_info;
                                vTaskDelete(task_info->handle);
                                xSemaphoreGive(window_stack_lock);
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

        if (background_damaged & (1 << cur_fb)) {
            memset(framebuffers[cur_fb], 0xaa, FRAMEBUFFER_BYTES);
            // Make sure the ppa will see our new background
            esp_cache_msync(
                framebuffers[cur_fb],
                FRAMEBUFFER_BYTES,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE
            );
            background_damaged &= ~(1 << cur_fb);
            changes             = true;
        }

        if (window_stack) {
            window_t *window = window_stack->prev; // Start with back window

            do {
                if ((window == window_stack) && (window_stack->flags & WINDOW_FLAG_FULLSCREEN)) {
                    // A foreground full-screen app gets as much CPU time as it can handle
                    vTaskPrioritySet(window->task_info->handle, TASK_PRIORITY_FOREGROUND);
                } else {
                    vTaskPrioritySet(window->task_info->handle, TASK_PRIORITY);
                }

                managed_framebuffer_t *framebuffer = window->framebuffers[window->cur_fb];

                if (!framebuffer || (atomic_load_explicit(&framebuffer->active, memory_order_acquire) == false)) {
                    // Not yet allocated, or in the process of being destroyed
                    if (!visible_regions_valid) {
                        // We expect every window to be up-to-date after this loop
                        window_calculate_visible_regions(window, window_stack, 1.0);
                    }

                    window = window->prev;
                    continue;
                }

                // Fill the window entirely, preserving aspect ratio and centering
                float scale_x = ((float)window->rect.w / (float)framebuffer->w);
                float scale_y = ((float)window->rect.h / (float)framebuffer->h);

                float scale = fminf(scale_x, scale_y);

                if (!visible_regions_valid) {
                    window_calculate_visible_regions(window, window_stack, scale);
                }

                ppa_srm_rotation_angle_t ppa_rotation = rotation_to_srm(rotation);

                bool                 rgb_swap  = false;
                bool                 byte_swap = false;
                ppa_srm_color_mode_t mode      = PPA_SRM_COLOR_MODE_RGB565;

                switch (framebuffer->format) {
                    case BADGEVMS_PIXELFORMAT_RGB565: rgb_swap = true; // Fallthrough
                    case BADGEVMS_PIXELFORMAT_BGR565: break;
                    case BADGEVMS_PIXELFORMAT_BGRA8888: rgb_swap = true; // Fallthrough
                    case BADGEVMS_PIXELFORMAT_RGBA8888:
                        // byte_swap = true;
                        mode = PPA_SRM_COLOR_MODE_ARGB8888;
                        break;
                    case BADGEVMS_PIXELFORMAT_ARGB8888: rgb_swap = true; // Fallthrough
                    case BADGEVMS_PIXELFORMAT_ABGR8888: mode = PPA_SRM_COLOR_MODE_ARGB8888; break;
                    default:
                }

                bool is_clean    = atomic_flag_test_and_set(&framebuffer->clean);
                bool drew_pixels = false;

                for (int i = 0; i < window->visible.count; i++) {
                    window_rect_t visible_content = window->visible.rects[i];
                    window_rect_t fb_rect         = content_to_framebuffer_rect(visible_content, window, scale);

                    if (fb_rect.w <= 0 || fb_rect.h <= 0) {
                        continue;
                    }

                    window_rect_t rotated_output = rotate_rect(visible_content, rotation);

                    ppa_srm_oper_config_t oper_config = {
                        .in.buffer         = framebuffer->framebuffer.pixels,
                        .in.pic_w          = framebuffer->w,
                        .in.pic_h          = framebuffer->h,
                        .in.block_w        = fb_rect.w,
                        .in.block_h        = fb_rect.h,
                        .in.block_offset_x = fb_rect.x,
                        .in.block_offset_y = fb_rect.y,
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
                        // .mode           = PPA_TRANS_MODE_NON_BLOCKING,
                        .mode           = PPA_TRANS_MODE_BLOCKING,
                    };

                    esp_err_t ppa_result = ppa_do_scale_rotate_mirror(ppa_srm_handle, &oper_config);
                    if (ppa_result != ESP_OK) {
                        printf("PPA operation failed: %s\n", esp_err_to_name(ppa_result));
                        printf(
                            "Trying: at %ux%u size %ux%u -> %ux%u size %ux%u scale %f\n",
                            window->rect.x,
                            window->rect.y,
                            window->rect.w,
                            window->rect.h,
                            fb_rect.x,
                            fb_rect.y,
                            fb_rect.w,
                            fb_rect.h,
                            scale
                        );
                    } else {
                        drew_pixels = true;
                        changes     = true;
                    }

                    // Currently commented out, we don't need the overhead
#if 0
                    // Wait for SRM to complete
                    if (!ulTaskNotifyTakeIndexed(0, pdTRUE, 64 / portTICK_PERIOD_MS)) {
                        ESP_LOGE(
                            TAG,
                            "PPA Config:"
                            "  Input: buf=%p pic=%dx%d block=%dx%d offset=(%d,%d) cm=%d "
                            "  Output: buf=%p size=%zu pic=%dx%d offset=(%d,%d) cm=%d "
                            "  Transform: rot=%d scale=(%.3f,%.3f) rgb_swap=%d byte_swap=%d mode=%d "
                            "  Context: win=(%d,%d)%dx%d vis=(%d,%d)%dx%d fb=(%d,%d)%dx%d rot_out=(%d,%d)%dx%d "
                            "fb_dim=%dx%d fmt=%d",
                            oper_config.in.buffer,
                            oper_config.in.pic_w,
                            oper_config.in.pic_h,
                            oper_config.in.block_w,
                            oper_config.in.block_h,
                            oper_config.in.block_offset_x,
                            oper_config.in.block_offset_y,
                            oper_config.in.srm_cm,
                            oper_config.out.buffer,
                            oper_config.out.buffer_size,
                            oper_config.out.pic_w,
                            oper_config.out.pic_h,
                            oper_config.out.block_offset_x,
                            oper_config.out.block_offset_y,
                            oper_config.out.srm_cm,
                            oper_config.rotation_angle,
                            oper_config.scale_x,
                            oper_config.scale_y,
                            oper_config.rgb_swap,
                            oper_config.byte_swap,
                            oper_config.mode,
                            window->rect.x,
                            window->rect.y,
                            window->rect.w,
                            window->rect.h,
                            visible_content.x,
                            visible_content.y,
                            visible_content.w,
                            visible_content.h,
                            fb_rect.x,
                            fb_rect.y,
                            fb_rect.w,
                            fb_rect.h,
                            rotated_output.x,
                            rotated_output.y,
                            rotated_output.w,
                            rotated_output.h,
                            framebuffer->w,
                            framebuffer->h,
                            framebuffer->format
                        );
                        while (1) {
                        };
                    }
#endif
                }

                if (!is_clean) {
                    xTaskNotifyGiveIndexed(window->task_info->handle, 1);
                }

                if (drew_pixels) {
                    // No need to resync the framebuffer if nothing changed
                    // Invalidate caches so windowbox drawing doesn't mess up the screen
                    esp_cache_msync(framebuffers[cur_fb], FRAMEBUFFER_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

                    if ((!(window->flags & WINDOW_FLAG_FULLSCREEN))) {
                        draw_window_box(framebuffers[cur_fb], window, window == window_stack);
                        // Make sure that the PPA will see the current frame including our decorations
                        esp_cache_msync(
                            framebuffers[cur_fb],
                            FRAMEBUFFER_BYTES,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE
                        );
                    }
                }

                window = window->prev;
            } while (window != window_stack->prev);

            visible_regions_valid = true;
        }

        xSemaphoreGive(window_stack_lock);

        if (changes) {
            lcd_device->_draw(lcd_device, 0, 0, FRAMEBUFFER_MAX_W, FRAMEBUFFER_MAX_H, framebuffers[cur_fb]);
            cur_fb = (cur_fb + 1) % 3;
        }
    }
}

void get_screen_info(int *width, int *height, pixel_format_t *format, float *refresh_rate) {
    *width        = FRAMEBUFFER_MAX_W;
    *height       = FRAMEBUFFER_MAX_H;
    *format       = BADGEVMS_PIXELFORMAT_RGB565;
    *refresh_rate = FRAMEBUFFER_MAX_REFRESH;
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

    window->flags     = flags;
    window->task_info = task_info;
    window->rect.x    = 0;
    window->rect.y    = 0;
    size              = window_clamp_size(window, size);
    window->rect.w    = size.w;
    window->rect.h    = size.h;

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

void window_destroy(window_t *window) {
    if (!window) {
        return;
    }

    ESP_LOGI(TAG, "Destroying window %p\n", window);

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
    coords         = window_clamp_position(window, coords);
    window->rect.x = coords.x;
    window->rect.y = coords.y;
    return coords;
}

window_size_t window_size_get(window_t *window) {
    window_size_t ret = {
        .w = window->rect.w,
        .h = window->rect.h,
    };
    return ret;
}

window_size_t window_size_set(window_t *window, window_size_t size) {
    size           = window_clamp_size(window, size);
    window->rect.w = size.w;
    window->rect.h = size.h;
    return size;
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

framebuffer_t *
    window_framebuffer_allocate(window_handle_t window, pixel_format_t format, window_size_t size, int *num) {
    if (window->num_fb == WINDOW_MAX_FRAMEBUFFER) {
        ESP_LOGW(TAG, "Window already has the max number of framebuffers");
        return NULL;
    }

    size.w = size.w > FRAMEBUFFER_MAX_W ? FRAMEBUFFER_MAX_W : size.w;
    size.h = size.h > FRAMEBUFFER_MAX_H ? FRAMEBUFFER_MAX_H : size.h;

    size              = window_clamp_size(window, size);
    framebuffer_t *fb = framebuffer_allocate(size.w, size.h, format);
    if (!fb) {
        ESP_LOGW(TAG, "Unable to allocate framebuffer");
        return NULL;
    }

    window->framebuffers[window->num_fb] = (managed_framebuffer_t *)fb;

    if (num) {
        *num = window->num_fb;
    }

    window->num_fb++;

    // To catch the edge-case of the window's framebuffer somehow creating PPA "illegal" rect sizes now.
    visible_regions_valid = false;
    return fb;
}

void window_framebuffer_free(window_handle_t window, int num) {
    framebuffer_t *fb = window_framebuffer_get(window, num);

    if (fb) {
        compositor_message_t message = {
            .command = FRAMEBUFFER_FREE,
            .window  = window,
            .fb_num  = num,
            .caller  = xTaskGetCurrentTaskHandle(),
        };

        xQueueSend(compositor_queue, &message, portMAX_DELAY);
        ulTaskNotifyTakeIndexed(0, pdTRUE, portMAX_DELAY);
    }
}

__attribute__((always_inline)) inline framebuffer_t *window_framebuffer_get(window_handle_t window, int num) {
    if (!window->num_fb) {
        ESP_LOGW(TAG, "No framebuffers to get");
        return NULL;
    }

    if (num > window->num_fb - 1) {
        ESP_LOGW(TAG, "Framebuffer num out of range");
        return NULL;
    }

    return (framebuffer_t *)window->framebuffers[num];
}

void window_framebuffer_update(window_t *window, int num, bool block, window_rect_t *rects, int num_rects) {
    framebuffer_t *fb = window_framebuffer_get(window, num);
    if (!fb) {
        ESP_LOGW(TAG, "Posting non-existant framebuffer");
        return;
    }

    managed_framebuffer_t *managed_framebuffer = (managed_framebuffer_t *)fb;

    if (atomic_load_explicit(&managed_framebuffer->active, memory_order_acquire) == false) {
        ESP_LOGW(TAG, "Cannot update framebuffer that is being destroyed");
        return;
    }

    window->cur_fb = num;

    atomic_flag_clear(&managed_framebuffer->clean);

    if (block) {
        ESP_LOGV(TAG, "Suspending myself");
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

void compositor_init(char const *lcd_device_name, char const *keyboard_device_name) {
    ESP_LOGI(TAG, "Initializing");

    lcd_device = (lcd_device_t *)device_get(lcd_device_name);
    if (!lcd_device) {
        ESP_LOGE(TAG, "Unable to access the LCD device '%s'", lcd_device_name);
        return;
    }

    keyboard_device = (device_t *)device_get(keyboard_device_name);
    if (!keyboard_device) {
        ESP_LOGE(TAG, "Unable to access the keyboard device '%s'", keyboard_device_name);
        // return;
    }

    lcd_device->_getfb(lcd_device, 0, (void *)&framebuffers[0]);
    lcd_device->_getfb(lcd_device, 1, (void *)&framebuffers[1]);
    lcd_device->_getfb(lcd_device, 2, (void *)&framebuffers[2]);

    memset(framebuffers[0], 0xaa, FRAMEBUFFER_BYTES);
    memset(framebuffers[1], 0xaa, FRAMEBUFFER_BYTES);
    memset(framebuffers[2], 0xaa, FRAMEBUFFER_BYTES);

    esp_cache_msync(framebuffers[0], FRAMEBUFFER_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(framebuffers[1], FRAMEBUFFER_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(framebuffers[2], FRAMEBUFFER_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    cur_fb = 1;

    ESP_LOGW(TAG, "Got framebuffers 0: %p, 1: %p, 2: %p", framebuffers[0], framebuffers[1], framebuffers[2]);
    lcd_device->_set_refresh_cb(lcd_device, NULL, on_refresh);

    compositor_queue  = xQueueCreate(10, sizeof(compositor_message_t));
    window_stack_lock = xSemaphoreCreateMutex();
    vsync             = xSemaphoreCreateBinary();
    ppa               = xSemaphoreCreateBinary();
    create_kernel_task(compositor, "Compositor", 8192, NULL, 20, &compositor_handle, 0);
}
