/**
 * @file lv_gif.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "src/widgets/gif/lv_gif.h"
#ifdef ESP_PLATFORM
#include <inttypes.h>
#include "esp_timer.h"
#include "esp_log.h"
#endif

#if LV_USE_GIF
#include "src/misc/lv_timer_private.h"
#include "src/misc/cache/lv_cache.h"
#include "src/core/lv_obj_class_private.h"
#include "src/widgets/image/lv_image_private.h"
#include "src/libs/gif/AnimatedGIF.h"

/*********************
 *      DEFINES
 *********************/
#define MY_CLASS (&lv_gif_class)

/**********************
 *      TYPEDEFS
 **********************/

/* the type of the AnimatedGIF pallete type passed to `GIF_begin` */
typedef unsigned char animatedgif_color_format_t;

typedef struct {
    lv_image_t img;
    GIFIMAGE gif;
    const void * src;
    lv_color_format_t color_format;
    lv_timer_t * timer;
    lv_draw_buf_t * draw_buf;
    int32_t loop_count;
    uint32_t is_open : 1;
    uint32_t is_auto_pause : 1;
    uint32_t cur_frame_index;
#if defined(ESP_PLATFORM) && WATCH_DEBUG_ENABLED
    int64_t perf_started_us;
    uint64_t perf_decode_us;
    uint32_t perf_decode_max_us, perf_frames, perf_source_ms, perf_catchup_skipped;
#endif
    uint32_t next_frame_due; /* Absolute animation clock, including decode/render time. */
} lv_gif_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lv_gif_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj);
static void lv_gif_destructor(const lv_obj_class_t * class_p, lv_obj_t * obj);
static void gif_draw_raw_cb(GIFDRAW * pDraw);
static void gif_previous_close(lv_gif_t * gifobj);
static void gif_initialize(lv_gif_t * gifobj);
static void gif_disposal_last_frame(GIFIMAGE * gif, lv_draw_buf_t * draw_buf);
static void gif_next_frame_task_cb(lv_timer_t * t);

/**********************
 *  STATIC VARIABLES
 **********************/

const lv_obj_class_t lv_gif_class = {
    .constructor_cb = lv_gif_constructor,
    .destructor_cb = lv_gif_destructor,
    .instance_size = sizeof(lv_gif_t),
    .base_class = &lv_image_class,
    .name = "lv_gif",
};

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * lv_gif_create(lv_obj_t * parent)
{
    LV_LOG_INFO("begin");
    lv_obj_t * obj = lv_obj_class_create_obj(MY_CLASS, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

void lv_gif_set_color_format(lv_obj_t * obj, lv_color_format_t color_format)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(gifobj->color_format == color_format) {
        return;
    }

    switch(color_format) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB565_SWAPPED:
        case LV_COLOR_FORMAT_RGB888:
        case LV_COLOR_FORMAT_XRGB8888:
        case LV_COLOR_FORMAT_ARGB8888:
            break;
        default:
            LV_LOG_WARN("gif widget does not support this color format");
            return;
    }

    gifobj->color_format = color_format;

    if(gifobj->src != NULL && gifobj->is_open) {
        gif_previous_close(gifobj);
        gif_initialize(gifobj);
    }
}

void lv_gif_set_src(lv_obj_t * obj, const void * src)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(gifobj->is_open) {
        gif_previous_close(gifobj);
    }

    gifobj->src = src;

    gif_initialize(gifobj);
}

void lv_gif_restart(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(!gifobj->is_open) {
        LV_LOG_WARN("Gif resource not loaded correctly");
        return;
    }

    GIF_reset(&gifobj->gif);
    gifobj->loop_count = -1; /* match the behavior of the old library */
    gifobj->cur_frame_index = 0;
    gifobj->next_frame_due = lv_tick_get();
    lv_timer_resume(gifobj->timer);
    lv_timer_ready(gifobj->timer);
}

void lv_gif_pause(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    lv_timer_pause(gifobj->timer);
}

void lv_gif_resume(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(!gifobj->is_open) {
        LV_LOG_WARN("Gif resource not loaded correctly");
        return;
    }

    gifobj->next_frame_due = lv_tick_get() + gifobj->timer->period;
#if defined(ESP_PLATFORM) && WATCH_DEBUG_ENABLED
    /* Exclude screen-off time from playback performance. The prepared frame's
     * visible hold starts now; its decode happened before wake. */
    gifobj->perf_started_us = esp_timer_get_time();
    gifobj->perf_decode_us = 0;
    gifobj->perf_decode_max_us = gifobj->perf_frames = gifobj->perf_catchup_skipped = 0;
    gifobj->perf_source_ms = gifobj->timer->period;
#endif
    lv_timer_reset(gifobj->timer);
    lv_timer_resume(gifobj->timer);
}

bool lv_gif_is_loaded(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    return gifobj->is_open;
}

int32_t lv_gif_get_loop_count(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(!gifobj->is_open) {
        return -1;
    }

    return gifobj->loop_count;
}

void lv_gif_set_loop_count(lv_obj_t * obj, int32_t count)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(!gifobj->is_open) {
        LV_LOG_WARN("Gif resource not loaded correctly");
        return;
    }

    gifobj->loop_count = count;
}

void lv_gif_set_auto_pause_invisible(lv_obj_t * obj, bool auto_pause)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    gifobj->is_auto_pause = auto_pause;
}

bool lv_gif_get_size(const char * src, uint16_t * w, uint16_t * h)
{
    LV_ASSERT_NULL(src);
    LV_ASSERT_NULL(w);
    LV_ASSERT_NULL(h);

    LV_PROFILER_DECODER_BEGIN;
    GIFIMAGE gif;
    if(!GIF_openFile(&gif, src, NULL)) {
        *w = 0;
        *h = 0;
        LV_PROFILER_DECODER_END;
        return false;
    }

    *w = GIF_getCanvasWidth(&gif);
    *h = GIF_getCanvasHeight(&gif);
    GIF_close(&gif);

    LV_PROFILER_DECODER_END;
    return true;
}

int32_t lv_gif_get_frame_count(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(!gifobj->is_open) {
        return -1;
    }

    LV_PROFILER_DECODER_BEGIN;

    GIFINFO info;
    if(!GIF_getInfo(&gifobj->gif, &info)) {
        LV_LOG_WARN("Get the frame count failed");
        LV_PROFILER_DECODER_END;
        return -1;
    }

    LV_PROFILER_DECODER_END;
    return info.iFrameCount;
}

int32_t lv_gif_get_current_frame_index(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(!gifobj->is_open) {
        return -1;
    }

    return (int32_t)gifobj->cur_frame_index;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void lv_gif_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);

    lv_gif_t * gifobj = (lv_gif_t *) obj;

    gifobj->color_format = LV_COLOR_FORMAT_ARGB8888;
    gifobj->is_open = 0;
    gifobj->is_auto_pause = 0;
    gifobj->cur_frame_index = 0;
    gifobj->timer = lv_timer_create(gif_next_frame_task_cb, 10, obj);
    lv_timer_pause(gifobj->timer);
}

static void lv_gif_destructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    const void * src = lv_image_get_src(obj);
    if(src != NULL) {
        lv_image_cache_drop(src);
    }

    if(gifobj->is_open) {
        GIF_close(&gifobj->gif);
        lv_draw_buf_destroy(gifobj->draw_buf);
        gifobj->draw_buf = NULL;
        gifobj->is_open = 0;
    }

    lv_timer_delete(gifobj->timer);
    gifobj->timer = NULL;
}

static inline void gif_blend_to_rgb565(GIFDRAW * pDraw, lv_draw_buf_t * draw_buf)
{
    LV_PROFILER_DECODER_BEGIN;

    uint8_t pixel;
    uint8_t * src = pDraw->pPixels;
    uint8_t * end = src + pDraw->iWidth;
    uint16_t * pal = pDraw->pPalette;
    uint16_t * dst = (uint16_t *)((uint8_t *)draw_buf->data + ((pDraw->iY + pDraw->y) * draw_buf->header.stride + pDraw->iX
                                                               * 2));

    if(pDraw->ucHasTransparency) {
        if(pDraw->ucDisposalMethod == 2) {
            /* Disposal 2: Replace transparent pixels with background color */
            while(src < end) {
                pixel = *src++;
                if(pixel == pDraw->ucTransparent) {
                    pixel = pDraw->ucBackground;
                }
                *dst++ = pal[pixel];
            }
        }
        else {
            /* Disposal 0,1,3: Replace transparent pixels with background color to maintain position
             * The gif_disposal_last_frame function handles the actual background clearing
            */
            uint16_t bg_color = pal[pDraw->ucBackground];
            while(src < end) {
                pixel = *src++;
                if(pixel == pDraw->ucTransparent) {
                    *dst++ = bg_color;
                }
                else {
                    *dst++ = pal[pixel];
                }
            }
        }
    }
    else {
        while(src < end) {
            pixel = *src++;
            *dst++ = pal[pixel];
        }
    }

    LV_PROFILER_DECODER_END;
}

static inline void gif_blend_to_rgb888(GIFDRAW * pDraw, lv_draw_buf_t * draw_buf)
{
    LV_PROFILER_DECODER_BEGIN;

    uint8_t pixel;
    uint8_t * src = pDraw->pPixels;
    uint8_t * end = src + pDraw->iWidth;
    uint8_t * pal = pDraw->pPalette24;
    uint8_t * dst = (uint8_t *)draw_buf->data + ((pDraw->iY + pDraw->y) * draw_buf->header.stride + pDraw->iX * 3);

    if(pDraw->ucHasTransparency) {
        if(pDraw->ucDisposalMethod == 2) {
            /* Disposal 2: Replace transparent pixels with background color */
            while(src < end) {
                pixel = *src++;
                if(pixel == pDraw->ucTransparent) {
                    pixel = pDraw->ucBackground;
                }
                dst[0] = pal[(pixel * 3) + 2];
                dst[1] = pal[(pixel * 3) + 1];
                dst[2] = pal[(pixel * 3) + 0];
                dst += 3;
            }
        }
        else {
            /* Disposal 0,1,3: Replace transparent pixels with background color to maintain position */
            uint8_t bg_r = pal[(pDraw->ucBackground * 3) + 2];
            uint8_t bg_g = pal[(pDraw->ucBackground * 3) + 1];
            uint8_t bg_b = pal[(pDraw->ucBackground * 3) + 0];
            while(src < end) {
                pixel = *src++;
                if(pixel == pDraw->ucTransparent) {
                    dst[0] = bg_r;
                    dst[1] = bg_g;
                    dst[2] = bg_b;
                }
                else {
                    dst[0] = pal[(pixel * 3) + 2];
                    dst[1] = pal[(pixel * 3) + 1];
                    dst[2] = pal[(pixel * 3) + 0];
                }
                dst += 3;
            }
        }
    }
    else {
        while(src < end) {
            pixel = *src++;
            dst[0] = pal[(pixel * 3) + 2];
            dst[1] = pal[(pixel * 3) + 1];
            dst[2] = pal[(pixel * 3) + 0];
            dst += 3;
        }
    }

    LV_PROFILER_DECODER_END;
}

static inline void gif_blend_to_argb8888(GIFDRAW * pDraw, lv_draw_buf_t * draw_buf)
{
    LV_PROFILER_DECODER_BEGIN;

    uint8_t pixel;
    uint8_t * src = pDraw->pPixels;
    uint8_t * end = src + pDraw->iWidth;
    uint8_t * pal = pDraw->pPalette24;
    uint8_t * dst = (uint8_t *)draw_buf->data + ((pDraw->iY + pDraw->y) * draw_buf->header.stride + pDraw->iX * 4);

    /* GIF transparency means leave the composited canvas untouched. The
     * PREVIOUS frame's disposal was already applied by gif_disposal_last_frame.
     * Clearing transparent pixels here destroys preserved parts of disposal-1
     * frames and produces holes/stripes on delta-encoded stickers. */
    while(src < end) {
        pixel = *src++;
        if(!pDraw->ucHasTransparency || pixel != pDraw->ucTransparent) {
            dst[0] = pal[(pixel * 3) + 2];
            dst[1] = pal[(pixel * 3) + 1];
            dst[2] = pal[(pixel * 3) + 0];
            dst[3] = 0xFF;
        }
        dst += 4;
    }

    LV_PROFILER_DECODER_END;
}

static void gif_draw_raw_cb(GIFDRAW * pDraw)
{
    lv_gif_t * gifobj = (lv_gif_t *) pDraw->pUser;

    switch(pDraw->ucPaletteType) {
        case GIF_PALETTE_RGB565_LE:
        case GIF_PALETTE_RGB565_BE:
            gif_blend_to_rgb565(pDraw, gifobj->draw_buf);
            break;
        case GIF_PALETTE_RGB888:
            gif_blend_to_rgb888(pDraw, gifobj->draw_buf);
            break;
        case GIF_PALETTE_RGB8888:
            gif_blend_to_argb8888(pDraw, gifobj->draw_buf);
            break;
        default:
            LV_LOG_WARN("Unsupported palette type: %d", pDraw->ucPaletteType);
            break;
    }
}

static void gif_previous_close(lv_gif_t * gifobj)
{
    LV_PROFILER_DECODER_BEGIN;

    /* Close previous gif if any */
    const char * src = lv_image_get_src((lv_obj_t *) gifobj);
    if(src != NULL) {
        lv_image_cache_drop(src);
    }

    lv_image_set_src((lv_obj_t *) gifobj, NULL);

    lv_timer_resume(gifobj->timer);
    lv_timer_reset(gifobj->timer);

    GIF_close(&gifobj->gif);
    lv_draw_buf_destroy(gifobj->draw_buf);
    gifobj->draw_buf = NULL;
    gifobj->is_open = 0;
    gifobj->loop_count = -1;

    LV_PROFILER_DECODER_END;
}

static void gif_initialize(lv_gif_t * gifobj)
{
    LV_PROFILER_DECODER_BEGIN;

    animatedgif_color_format_t decoder_cf;
    switch(gifobj->color_format) {
        case LV_COLOR_FORMAT_RGB565:
            decoder_cf = GIF_PALETTE_RGB565_LE;
            break;
        case LV_COLOR_FORMAT_RGB565_SWAPPED:
            decoder_cf = GIF_PALETTE_RGB565_BE;
            break;
        case LV_COLOR_FORMAT_RGB888:
            decoder_cf = GIF_PALETTE_RGB888;
            break;
        case LV_COLOR_FORMAT_XRGB8888:
        case LV_COLOR_FORMAT_ARGB8888:
            decoder_cf = GIF_PALETTE_RGB8888;
            break;
        default:
            LV_LOG_WARN("Unsupported color format: %d", gifobj->color_format);
            LV_PROFILER_DECODER_END;
            return;
    }

    GIFIMAGE * gif = &gifobj->gif;
    GIF_begin(gif, decoder_cf);

    lv_image_src_t src_type = lv_image_src_get_type(gifobj->src);
    if(src_type == LV_IMAGE_SRC_VARIABLE) {
        const lv_image_dsc_t * img_dsc = gifobj->src;
        gifobj->is_open = GIF_openRAM(gif, (uint8_t *) img_dsc->data, img_dsc->data_size, gif_draw_raw_cb);
    }
    else if(src_type == LV_IMAGE_SRC_FILE) {
        gifobj->is_open = GIF_openFile(gif, gifobj->src, gif_draw_raw_cb);
    }
    if(gifobj->is_open == 0) {
        LV_LOG_WARN("Couldn't load the source");
        LV_PROFILER_DECODER_END;
        return;
    }

    uint32_t width = GIF_getCanvasWidth(gif);
    uint32_t height = GIF_getCanvasHeight(gif);
    gif->ucDrawType = GIF_DRAW_RAW;

    gifobj->draw_buf = lv_draw_buf_create(width, height, gifobj->color_format, LV_STRIDE_AUTO);

    if(gifobj->draw_buf == NULL) {
        LV_LOG_WARN("Couldn't allocate memory for the gif with width: %"LV_PRIu32" and height: %"LV_PRIu32, width, height);
        GIF_close(gif);
        gifobj->is_open = 0;
        LV_PROFILER_DECODER_END;
        return;
    }

    lv_draw_buf_clear(gifobj->draw_buf, NULL);
    gifobj->cur_frame_index = 0;
    gifobj->next_frame_due = lv_tick_get();
#if defined(ESP_PLATFORM) && WATCH_DEBUG_ENABLED
    gifobj->perf_started_us = esp_timer_get_time();
    gifobj->perf_decode_us = 0;
    gifobj->perf_decode_max_us = gifobj->perf_frames = gifobj->perf_source_ms = gifobj->perf_catchup_skipped = 0;
#endif
    lv_image_set_src((lv_obj_t *) gifobj, gifobj->draw_buf);

    gifobj->loop_count = GIF_getLoopCount(&gifobj->gif);

    lv_timer_resume(gifobj->timer);
    lv_timer_reset(gifobj->timer);
    gif_next_frame_task_cb(gifobj->timer);
    LV_PROFILER_DECODER_END;
}

/**
 * Dispose the previous frame area before rendering the next frame, according to the GIF disposal method.
 *
 * This function handles the disposal of the previous frame's area in the GIF image, as specified by the disposal method.
 * Disposal method values:
 *   0: No disposal specified (do nothing)
 *   1: Do not dispose (leave as is)
 *   2: Restore to background color (the affected area is filled with the background color)
 *   3: Restore to previous (not implemented here)
 * Only disposal method 2 ("restore to background") is handled in this function.
 *
 * @param gif      Pointer to the GIFIMAGE structure representing the current GIF frame.
 * @param drawbuf  Pointer to the draw buffer where the frame is rendered.
 *
 * Assumptions:
 *   - The coordinates and dimensions (iX, iY, iWidth, iHeight) are within the bounds of the draw buffer.
 *   - The palette type and background color are valid for the current GIF frame.
 */
static void gif_disposal_last_frame(GIFIMAGE * gif, lv_draw_buf_t * drawbuf)
{
    LV_PROFILER_DECODER_BEGIN;

    int x = gif->iX;
    int y = gif->iY;
    int w = gif->iWidth;
    int h = gif->iHeight;
    int disposal_method = (gif->ucGIFBits & 0x1c) >> 2;
    int i, j;

    /* Bounds validation to prevent out-of-bounds access */
    if(x < 0 || y < 0 || w <= 0 || h <= 0 ||
       x + w > drawbuf->header.w || y + h > drawbuf->header.h) {
        LV_PROFILER_DECODER_END;
        return;
    }

    /* Only disposal method 2 requires explicit clearing */
    if(disposal_method == 2) {
        /* Restore to background color */
        unsigned char bg = gif->ucBackground;
        unsigned char * palette = (unsigned char *)(gif->bUseLocalPalette ? gif->pLocalPalette : gif->pPalette);
        switch(gif->ucPaletteType) {
            case GIF_PALETTE_RGB565_LE:
            case GIF_PALETTE_RGB565_BE: {
                    unsigned short * palette16 = (unsigned short *)palette;
                    for(i = y; i < y + h; i++) {
                        uint8_t * dst = drawbuf->data + drawbuf->header.stride * i;
                        for(j = x; j < x + w; j++) {
                            *(uint16_t *)(dst + 2 * j) = palette16[bg];
                        }
                    }
                }
                break;
            case GIF_PALETTE_RGB888:
                for(i = y; i < y + h; i++) {
                    uint8_t * dst = drawbuf->data + drawbuf->header.stride * i;
                    for(j = x; j < x + w; j++) {
                        dst[3 * j] = palette[(bg * 3) + 2];
                        dst[3 * j + 1] = palette[(bg * 3) + 1];
                        dst[3 * j + 2] = palette[(bg * 3) + 0];
                    }
                }
                break;
            case GIF_PALETTE_RGB8888: {
                    lv_color32_t bg_color = lv_color32_make(palette[(bg * 3) + 2], palette[(bg * 3) + 1], palette[(bg * 3)], 0xff);
                    /* has transparent */
                    if(gif->ucGIFBits & 1) {
                        bg_color = lv_color32_make(0, 0, 0, 0);
                    }

                    for(i = y; i < y + h; i++) {
                        uint8_t * dst = drawbuf->data + drawbuf->header.stride * i;
                        for(j = x; j < x + w; j++) {
                            *(lv_color32_t *)(dst + 4 * j) = bg_color;
                        }
                    }
                }
                break;
            default:
                break;
        }
    }
    /* disposal_method 0 and 1: do nothing, leave existing content */
    /* disposal_method 3: not supported, do nothing */

    LV_PROFILER_DECODER_END;
}

static void gif_next_frame_task_cb(lv_timer_t * t)
{
    lv_obj_t * obj = t->user_data;
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(gifobj == NULL) {
        return;
    }

    LV_PROFILER_DECODER_BEGIN;

    bool is_visible = lv_obj_is_visible(obj);
    if(gifobj->is_auto_pause && !is_visible) {
        lv_timer_pause(t);
        LV_PROFILER_DECODER_END;
        return;
    }

    GIFIMAGE * gif = &gifobj->gif;
    unsigned decoded = 0;
    /* A slow refresh can span several source frames. Compose overdue frames
     * without rendering each intermediate one, otherwise the whole animation
     * plays in slow motion even though it uses absolute deadlines. Four bounds
     * decoder work per callback; a later call can continue catching up. */
    while(decoded < 4 && (int32_t)(lv_tick_get() - gifobj->next_frame_due) >= 0) {
#if defined(ESP_PLATFORM) && WATCH_DEBUG_ENABLED
        int64_t decode_started_us = esp_timer_get_time();
        if(gifobj->cur_frame_index == 0 && gifobj->perf_frames > 0 &&
           decode_started_us - gifobj->perf_started_us >= 5000000) {
            uint32_t wall_ms = (uint32_t)((decode_started_us - gifobj->perf_started_us) / 1000);
            ESP_LOGI("WatchGifPerf",
                     "source=%" PRIu32 "ms wall=%" PRIu32 "ms speed=%" PRIu32
                     "%% decoded=%" PRIu32 " catchup_skipped=%" PRIu32
                     " decode_avg=%" PRIu32 "us decode_max=%" PRIu32 "us",
                     gifobj->perf_source_ms, wall_ms, gifobj->perf_source_ms * 100 / wall_ms,
                     gifobj->perf_frames, gifobj->perf_catchup_skipped,
                     (uint32_t)(gifobj->perf_decode_us / gifobj->perf_frames),
                     gifobj->perf_decode_max_us);
            gifobj->perf_started_us = decode_started_us;
            gifobj->perf_decode_us = 0;
            gifobj->perf_decode_max_us = gifobj->perf_frames = gifobj->perf_source_ms = gifobj->perf_catchup_skipped = 0;
        }
#endif
        if(gifobj->cur_frame_index == 0) {
            lv_draw_buf_clear(gifobj->draw_buf, NULL);
        }
        else {
            gif_disposal_last_frame(gif, gifobj->draw_buf);
        }

        int ms_delay_next;
        LV_PROFILER_DECODER_BEGIN_TAG("GIF_playFrame");
        int has_next = GIF_playFrame(gif, &ms_delay_next, gifobj);
        LV_PROFILER_DECODER_END_TAG("GIF_playFrame");
        ++decoded;
#if defined(ESP_PLATFORM) && WATCH_DEBUG_ENABLED
        if(decoded > 1) gifobj->perf_catchup_skipped++;
#endif
        if(has_next <= 0) {
            gifobj->cur_frame_index = 0;
            lv_result_t res = lv_obj_send_event(obj, LV_EVENT_READY, NULL);
            /* A READY listener may delete this object. */
            if(res != LV_RESULT_OK) {
                LV_PROFILER_DECODER_END;
                return;
            }
            if(gifobj->loop_count > 0) {
                if(gifobj->loop_count == 1) lv_timer_pause(t);
                else gifobj->loop_count--;
            }
            else if(gifobj->loop_count < 0) lv_timer_pause(t);
        }
        else {
            gifobj->cur_frame_index++;
        }

        /* Every frame, including the final one, keeps its original duration. */
        if(ms_delay_next < 10) ms_delay_next = 10;
        gifobj->next_frame_due += (uint32_t)ms_delay_next;
#if defined(ESP_PLATFORM) && WATCH_DEBUG_ENABLED
        uint32_t decode_us = (uint32_t)(esp_timer_get_time() - decode_started_us);
        gifobj->perf_decode_us += decode_us;
        if(decode_us > gifobj->perf_decode_max_us) gifobj->perf_decode_max_us = decode_us;
        gifobj->perf_frames++;
        gifobj->perf_source_ms += (uint32_t)ms_delay_next;
#endif
        if(lv_timer_get_paused(t)) break;
    }
    /* last_run is set BEFORE this callback. Include time spent decoding when
     * calculating the next deadline, rather than adding that time again. */
    int32_t remaining = (int32_t)(gifobj->next_frame_due - t->last_run);
    lv_timer_set_period(t, remaining > 0 ? (uint32_t)remaining : 1);
    if(decoded == 0) {
        LV_PROFILER_DECODER_END;
        return;
    }

    lv_draw_buf_flush_cache(gifobj->draw_buf, NULL);
    lv_image_cache_drop(lv_image_get_src(obj));
    lv_obj_invalidate(obj);

    LV_PROFILER_DECODER_END;
}

#endif /*LV_USE_GIF*/

