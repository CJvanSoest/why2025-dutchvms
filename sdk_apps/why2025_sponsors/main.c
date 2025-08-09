#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <badgevms/compositor.h>
#include <badgevms/event.h>
#include <badgevms/framebuffer.h>
#include <badgevms/process.h>
#include <dirent.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RGB565(r, g, b)           ((((r) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((b) & 0x1F))
#define RGB888_TO_RGB565(r, g, b) RGB565(((r) * 31 + 127) / 255, ((g) * 63 + 127) / 255, ((b) * 31 + 127) / 255)

#define SCROLLER_HEIGHT         30
#define MAX_SPONSORS            64
#define SPONSOR_DISPLAY_TIME_MS 3000
#define SPONSOR_DIR             "APPS:[WHY2025_SPONSORS.IMAGES]"

#define SPONSOR_WIDTH_ORIGINAL  256
#define SPONSOR_HEIGHT_ORIGINAL 128
#define SPONSOR_SCALE_FACTOR    2
#define SPONSOR_WIDTH_SCALED    (SPONSOR_WIDTH_ORIGINAL * SPONSOR_SCALE_FACTOR)
#define SPONSOR_HEIGHT_SCALED   (SPONSOR_HEIGHT_ORIGINAL * SPONSOR_SCALE_FACTOR)

#define SPONSOR_REGION_Y_START ((720 - SPONSOR_HEIGHT_SCALED) / 2)
#define SPONSOR_REGION_Y_END   (SPONSOR_REGION_Y_START + SPONSOR_HEIGHT_SCALED)

#define LIGHTEST_R 0xaa
#define LIGHTEST_G 0xb9
#define LIGHTEST_B 0xc8

#define DARKEST_R 0x4e
#define DARKEST_G 0x7f
#define DARKEST_B 0xbb

int render_png_to_framebuffer(
    uint16_t *framebuffer, int fb_width, int fb_height, char const *filename, int dest_x, int dest_y
);
int render_jpg_to_framebuffer(
    uint16_t *framebuffer, int fb_width, int fb_height, char const *filename, int dest_x, int dest_y
);
int stbi_info(char const *filename, int *x, int *y, int *comp);
int render_png_with_alpha_scaled(
    uint16_t *framebuffer, int fb_width, int fb_height, char const *filename, int dest_x, int dest_y, int scale_factor
);
int render_sponsor_centered(uint16_t *framebuffer, int fb_width, int fb_height, char const *filename);

typedef struct {
    char        sponsor_files[MAX_SPONSORS][256];
    int         sponsor_count;
    int         current_sponsor_index;
    atomic_bool new_image_ready;
    atomic_bool thread_should_exit;
    uint16_t   *temp_framebuffer;
    uint16_t   *clean_background;
    int         fb_width;
    int         fb_height;
    bool        all_seen;
} sponsor_state_t;

static sponsor_state_t g_sponsor_state = {0};

uint8_t interpolate_color(uint8_t start, uint8_t end, int distance, int max_distance) {
    if (distance >= max_distance)
        return end;
    return start + ((end - start) * distance) / max_distance;
}

int wrapped_distance(int x, int bright_spot, int width) {
    int direct_dist = abs(x - bright_spot);
    int wrap_dist   = width - direct_dist;
    return (direct_dist < wrap_dist) ? direct_dist : wrap_dist;
}

void render_win95_scroller_smooth(uint16_t *framebuffer, int width, int height, int gradient_offset) {
    int start_y = height - SCROLLER_HEIGHT;
    if (start_y < 0)
        start_y = 0;

    int max_gradient_distance = width / 2;

    for (int y = start_y; y < height; y++) {
        uint16_t *row = ((uint16_t *)framebuffer) + (y * width);

        for (int x = 0; x < width; x++) {
            int distance = wrapped_distance(x, gradient_offset, width);

            // Clamp distance to maximum gradient range
            if (distance > max_gradient_distance) {
                distance = max_gradient_distance;
            }

            uint8_t r = interpolate_color(LIGHTEST_R, DARKEST_R, distance, max_gradient_distance);
            uint8_t g = interpolate_color(LIGHTEST_G, DARKEST_G, distance, max_gradient_distance);
            uint8_t b = interpolate_color(LIGHTEST_B, DARKEST_B, distance, max_gradient_distance);

            row[x] = RGB888_TO_RGB565(r, g, b);
        }
    }
}

void shuffle_sponsors(sponsor_state_t *state) {
    srand(time(NULL));
    for (int i = state->sponsor_count - 1; i > 0; i--) {
        int  j = rand() % (i + 1);
        char temp[256];
        strcpy(temp, state->sponsor_files[i]);
        strcpy(state->sponsor_files[i], state->sponsor_files[j]);
        strcpy(state->sponsor_files[j], temp);
    }
}

int load_sponsor_list(sponsor_state_t *state) {
    DIR *dir = opendir(SPONSOR_DIR);
    if (!dir) {
        return -1;
    }

    struct dirent *entry;
    state->sponsor_count = 0;

    while ((entry = readdir(dir)) != NULL && state->sponsor_count < MAX_SPONSORS) {
        size_t len = strlen(entry->d_name);
        if (len > 4) {
            char *ext = entry->d_name + len - 4;
            if (strcasecmp(ext, ".png") == 0) {
                char const *dir     = SPONSOR_DIR;
                int         dir_len = strlen(dir);
                snprintf(
                    state->sponsor_files[state->sponsor_count],
                    sizeof(state->sponsor_files[0]),
                    "%s%s",
                    dir,
                    entry->d_name
                );
                printf("Found sponsor file %s\n", state->sponsor_files[state->sponsor_count]);
                state->sponsor_count++;
            }
        }
    }

    closedir(dir);

    if (state->sponsor_count > 0) {
        shuffle_sponsors(state);
    }

    return state->sponsor_count;
}

void sponsor_loader_thread(void *user_data) {
    sponsor_state_t *state = (sponsor_state_t *)user_data;

    uint32_t last_change_time = 0;

    while (!atomic_load(&state->thread_should_exit)) {
        uint32_t current_time = time(NULL) * 1000;

        if (current_time - last_change_time >= SPONSOR_DISPLAY_TIME_MS) {
            if (state->sponsor_count > 0) {
                char *current_file = state->sponsor_files[state->current_sponsor_index];
                printf("Loading %s\n", current_file);

                uint16_t *temp_fb = malloc(state->fb_width * state->fb_height * sizeof(uint16_t));
                if (temp_fb) {
                    memcpy(temp_fb, state->clean_background, state->fb_width * state->fb_height * sizeof(uint16_t));
                    if (render_sponsor_centered(temp_fb, state->fb_width, state->fb_height, current_file) == 0) {
                        free(state->temp_framebuffer);
                        state->temp_framebuffer = temp_fb;
                        atomic_store(&state->new_image_ready, true);
                        printf("Done loading %s\n", current_file);

                        state->current_sponsor_index = (state->current_sponsor_index + 1) % state->sponsor_count;
                        if (state->current_sponsor_index == 0) {
                            printf("Seen all sponsors!\n");
                            state->all_seen = true;
                        }
                        last_change_time = current_time;
                    } else {
                        free(temp_fb);
                    }
                }
            }
        }

        usleep(100000);
    }
}

int render_sponsor_centered(uint16_t *framebuffer, int fb_width, int fb_height, char const *filename) {
    int img_width, img_height, channels;

    if (!stbi_info(filename, &img_width, &img_height, &channels)) {
        return -1;
    }

    int scale_factor = 2;

    int scaled_width  = img_width * scale_factor;
    int scaled_height = img_height * scale_factor;
    int dest_x        = (fb_width - scaled_width) / 2;
    int dest_y        = (fb_height - scaled_height) / 2;

    return render_png_with_alpha_scaled(framebuffer, fb_width, fb_height, filename, dest_x, dest_y, scale_factor);
}

int main(int argc, char *argv[]) {
    printf("Sponsor app\n");

    bool at_boot = false;

    for (int i = 1; i < argc; ++i) {
        printf("Argv[%i] %s\n", i, argv[i]);
        if (strcmp(argv[i], "--boot") == 0) {
            at_boot = true;
        }
    }

    // If our flag exists, and we are at boot, exit
    FILE *f = fopen("APPS:[WHY2025_SPONSORS]bootflag", "r");
    if (f) {
        fclose(f);
        if (at_boot) {
            printf("At boot and all logos already seen, exiting\n");
            return 0;
        }
    }

    window_size_t size;

    size.w = 720;
    size.h = 720;

    window_handle_t window      = window_create("FB test", size, WINDOW_FLAG_FULLSCREEN);
    framebuffer_t  *framebuffer = window_framebuffer_create(window, size, BADGEVMS_PIXELFORMAT_RGB565);

    render_jpg_to_framebuffer(
        framebuffer->pixels,
        framebuffer->w,
        framebuffer->h,
        "APPS:[WHY2025_SPONSORS]BACKGROUND.JPG",
        0,
        0
    );

    g_sponsor_state.fb_width              = framebuffer->w;
    g_sponsor_state.fb_height             = framebuffer->h;
    g_sponsor_state.current_sponsor_index = 0;
    atomic_init(&g_sponsor_state.new_image_ready, false);
    atomic_init(&g_sponsor_state.thread_should_exit, false);

    g_sponsor_state.clean_background = malloc(framebuffer->w * framebuffer->h * sizeof(uint16_t));
    memcpy(g_sponsor_state.clean_background, framebuffer->pixels, framebuffer->w * framebuffer->h * sizeof(uint16_t));

    if (load_sponsor_list(&g_sponsor_state) <= 0) {
        printf("No sponsors found?\n");
        // No sponsors found, continue without them
    }

    pid_t sponsor_thread = 0;
    if (g_sponsor_state.sponsor_count > 0) {
        sponsor_thread = thread_create(sponsor_loader_thread, &g_sponsor_state, 16384);
    }

    static int gradient_offset    = 0;
    uint32_t   last_sponsor_check = 0;

    bool can_exit      = !at_boot;
    int  rendered_exit = 0;
    while (true) {
        if (can_exit && rendered_exit < 2) {
            render_png_with_alpha_scaled(
                framebuffer->pixels,
                framebuffer->w,
                framebuffer->h,
                "APPS:[WHY2025_SPONSORS]esc.png",
                620,
                30,
                1
            );
            ++rendered_exit;
        }

        event_t e = window_event_poll(window, false, 0);
        if (e.type == EVENT_KEY_DOWN) {
            if (e.keyboard.scancode == KEY_SCANCODE_ESCAPE) {
                if (can_exit) {
                    break;
                }
            }
        }

        if (atomic_load(&g_sponsor_state.new_image_ready)) {
            printf("New sponsor image ready\n");

            size_t bytes_per_row     = framebuffer->w * sizeof(uint16_t);
            size_t copy_start_offset = SPONSOR_REGION_Y_START * framebuffer->w;
            size_t copy_size         = SPONSOR_HEIGHT_SCALED * bytes_per_row;

            memcpy(
                (uint8_t *)framebuffer->pixels + (copy_start_offset * sizeof(uint16_t)),
                (uint8_t *)g_sponsor_state.temp_framebuffer + (copy_start_offset * sizeof(uint16_t)),
                copy_size
            );

            atomic_store(&g_sponsor_state.new_image_ready, false);
            if (g_sponsor_state.all_seen) {
                can_exit = true;
            }
        }

        render_win95_scroller_smooth(framebuffer->pixels, framebuffer->w, framebuffer->h, gradient_offset);
        window_present(window, true, NULL, 0);
        gradient_offset = (gradient_offset - 8 + framebuffer->w) % framebuffer->w;
    }

    if (g_sponsor_state.all_seen) {
        f = fopen("APPS:[WHY2025_SPONSORS]bootflag", "w");
        if (f) {
            fclose(f);
        }
    }
    return 0;
}
