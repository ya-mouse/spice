/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2015 Jeremy White
   Copyright (C) 2015 Francois Gouget

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <<A HREF="http://www.gnu.org/licenses/">http://www.gnu.org/licenses/</A>>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include "red_common.h"
#include "video_encoder.h"


#define GSTE_DEFAULT_FPS 30

#define DO_ZERO_COPY

#define NANO_SECOND (1000000000LL)
#define MILLI_SECOND (1000LL)
#define NANO_MS (NANO_SECOND / MILLI_SECOND)

typedef struct {
    SpiceBitmapFmt spice_format;
    const char *format;
    uint32_t bpp;
} SpiceFormatForGStreamer;

typedef struct SpiceGstVideoBuffer {
    VideoBuffer base;
    GstBuffer *gst_buffer;
    GstMapInfo map;
} SpiceGstVideoBuffer;

typedef struct {
    uint32_t mm_time;
    uint32_t size;
} GstFrameInformation;

typedef struct SpiceGstEncoder {
    VideoEncoder base;

    /* Rate control callbacks */
    VideoEncoderRateControlCbs cbs;
    void *cbs_opaque;

    /* Spice's initial bit rate estimation in bits per second. */
    uint64_t starting_bit_rate;

    /* ---------- Video characteristics ---------- */

    int width;
    int height;
    SpiceFormatForGStreamer *format;
    SpiceBitmapFmt spice_format;

    /* ---------- GStreamer pipeline ---------- */

    /* Pointers to the GStreamer pipeline elements. If pipeline is NULL the
     * other pointers are invalid.
     */
    GstElement *pipeline;
    GstCaps *src_caps;
    GstAppSrc *appsrc;
    GstElement *gstenc;
    GstAppSink *appsink;

#ifdef DO_ZERO_COPY
    /* Set to TRUE until GStreamer no longer needs the raw bitmap buffer. */
    gboolean needs_bitmap;
#endif

    /* The frame counter for GStreamer buffers */
    uint32_t frame;


    /* ---------- Encoded frame statistics ---------- */

    /* Should be >= than FRAME_STATISTICS_COUNT. This is also used to annotate
     * the client report debug traces with bit rate information.
     */
#   define GSTE_HISTORY_SIZE 60

    /* A circular buffer containing the past encoded frames information. */
    GstFrameInformation history[GSTE_HISTORY_SIZE];

    /* The indices of the oldest and newest frames in the history buffer. */
    uint32_t history_first;
    uint32_t history_last;

    /* How many frames to take into account when computing the effective
     * bit rate, average frame size, etc. This should be large enough so the
     * I and P frames average out, and short enough for it to reflect the
     * current situation.
     */
#   define GSTE_FRAME_STATISTICS_COUNT 21

    /* The index of the oldest frame taken into account for the statistics. */
    uint32_t stat_first;

    /* Used to compute the average frame size. */
    uint64_t stat_sum;

    /* Tracks the maximum frame size. */
    uint32_t stat_maximum;


    /* ---------- Encoder bit rate control ----------
     *
     * GStreamer encoders don't follow the specified bit rate very
     * closely. These fields are used to ensure we don't exceed the desired
     * stream bit rate, regardless of the GStreamer encoder's output.
     */

    /* The bit rate target for the outgoing network stream. (bits per second) */
    uint64_t bit_rate;

    /* The minimum bit rate */
#   define GSTE_MIN_BITRATE (128 * 1024)

    /* The default bit rate */
#   define GSTE_DEFAULT_BITRATE (8 * 1024 * 1024)

    /* The bit rate control is performed using a virtual buffer to allow short
     * term variations: bursts are allowed until the virtual buffer is full.
     * Then frames are dropped to limit the bit rate. VBUFFER_SIZE defines the
     * size of the virtual buffer in milliseconds worth of data.
     */
#   define GSTE_VBUFFER_SIZE 300

    int32_t vbuffer_size;
    int32_t vbuffer_free;

    /* When dropping frames, this is set to the minimum mm_time of the next
     * frame to encode. Otherwise set to zero.
     */
    uint32_t next_frame;

    /* Defines the minimum allowed fps. */
#   define GSTE_MAX_PERIOD (NANO_SECOND / 3)

    /* How big of a margin to take to cover for latency jitter. */
#   define GSTE_LATENCY_MARGIN 0.1
} SpiceGstEncoder;


/* ---------- The SpiceGstVideoBuffer implementation ---------- */

static void gst_video_buffer_free(VideoBuffer *video_buffer)
{
    SpiceGstVideoBuffer *buffer = (SpiceGstVideoBuffer*)video_buffer;
    gst_buffer_unref(buffer->gst_buffer);
    free(buffer);
}

static SpiceGstVideoBuffer* create_gst_video_buffer(void)
{
    SpiceGstVideoBuffer *buffer = spice_new0(SpiceGstVideoBuffer, 1);
    buffer->base.free = &gst_video_buffer_free;
    return buffer;
}


/* ---------- Miscellaneous SpiceGstEncoder helpers ---------- */

static inline double get_mbps(uint64_t bit_rate)
{
    return (double)bit_rate / 1024 / 1024;
}

/* Returns the source frame rate which may change at any time so don't store
 * the result.
 */
static uint32_t get_source_fps(SpiceGstEncoder *encoder)
{
    return encoder->cbs.get_source_fps ?
        encoder->cbs.get_source_fps(encoder->cbs_opaque) : GSTE_DEFAULT_FPS;
}

static uint32_t get_network_latency(SpiceGstEncoder *encoder)
{
    /* Assume that the network latency is symmetric */
    return encoder->cbs.get_roundtrip_ms ?
        encoder->cbs.get_roundtrip_ms(encoder->cbs_opaque) / 2 : 0;
}

static inline int rate_control_is_active(SpiceGstEncoder* encoder)
{
    return encoder->cbs.get_roundtrip_ms != NULL;
}

static void reset_pipeline(SpiceGstEncoder *encoder)
{
    if (!encoder->pipeline) {
        return;
    }

    gst_element_set_state(encoder->pipeline, GST_STATE_NULL);
    gst_caps_unref(encoder->src_caps);
    encoder->src_caps = NULL;

    gst_object_unref(encoder->appsrc);
    gst_object_unref(encoder->gstenc);
    gst_object_unref(encoder->appsink);
    gst_object_unref(encoder->pipeline);
    encoder->pipeline = NULL;
}


/* ---------- Encoded frame statistics ---------- */

static inline uint32_t get_last_frame_mm_time(SpiceGstEncoder *encoder)
{
    return encoder->history[encoder->history_last].mm_time;
}

/* Returns the current bit rate based on the last GSTE_FRAME_STATISTICS_COUNT
 * frames.
 */
static uint64_t get_effective_bit_rate(SpiceGstEncoder *encoder)
{
    uint32_t elapsed = encoder->history[encoder->history_last].mm_time -
        encoder->history[encoder->stat_first].mm_time;
    if (encoder->next_frame) {
        elapsed += encoder->next_frame - get_last_frame_mm_time(encoder);
    } else {
        elapsed += MILLI_SECOND / get_source_fps(encoder);
    }
    return elapsed ? encoder->stat_sum * 8 * MILLI_SECOND / elapsed : 0;
}

static uint64_t get_average_frame_size(SpiceGstEncoder *encoder)
{
    uint32_t count = encoder->history_last +
        (encoder->history_last < encoder->stat_first ? GSTE_HISTORY_SIZE : 0) -
        encoder->stat_first + 1;
    return encoder->stat_sum / count;
}

static uint32_t get_maximum_frame_size(SpiceGstEncoder *encoder)
{
    if (encoder->stat_maximum == 0) {
        uint32_t index = encoder->history_last;
        while (1) {
            encoder->stat_maximum = MAX(encoder->stat_maximum,
                                        encoder->history[index].size);
            if (index == encoder->stat_first) {
                break;
            }
            index = (index ? index : GSTE_HISTORY_SIZE) - 1;
        }
    }
    return encoder->stat_maximum;
}

/* Returns the bit rate of the specified period. from and to must be the
 * mm time of the first and last frame to consider.
 */
static uint64_t get_period_bit_rate(SpiceGstEncoder *encoder, uint32_t from,
                                    uint32_t to)
{
    uint32_t sum = 0;
    uint32_t last_mm_time = 0;
    uint32_t index = encoder->history_last;
    while (1) {
        if (encoder->history[index].mm_time == to) {
            if (last_mm_time == 0) {
                /* We don't know how much time elapsed between the period's
                 * last frame and the next so we cannot include it.
                 */
                sum = 1;
                last_mm_time = to;
            } else {
                sum = encoder->history[index].size + 1;
            }

        } else if (encoder->history[index].mm_time == from) {
            sum += encoder->history[index].size;
            return (sum - 1) * 8 * MILLI_SECOND / (last_mm_time - from);

        } else if (index == encoder->history_first) {
            /* This period is outside the recorded history */
            spice_debug("period (%u-%u) outside known history (%u-%u)",
                        from, to,
                        encoder->history[encoder->history_first].mm_time,
                        encoder->history[encoder->history_last].mm_time);
           return 0;

        } else if (sum > 0) {
            sum += encoder->history[index].size;

        } else {
            last_mm_time = encoder->history[index].mm_time;
        }
        index = (index ? index : GSTE_HISTORY_SIZE) - 1;
    }

}

static void add_frame(SpiceGstEncoder *encoder, uint32_t frame_mm_time,
                      uint32_t size)
{
    /* Update the statistics */
    uint32_t count = encoder->history_last +
        (encoder->history_last < encoder->stat_first ? GSTE_HISTORY_SIZE : 0) -
        encoder->stat_first + 1;
    if (count == GSTE_FRAME_STATISTICS_COUNT) {
        encoder->stat_sum -= encoder->history[encoder->stat_first].size;
        if (encoder->stat_maximum == encoder->history[encoder->stat_first].size) {
            encoder->stat_maximum = 0;
        }
        encoder->stat_first = (encoder->stat_first + 1) % GSTE_HISTORY_SIZE;
    }
    encoder->stat_sum += size;
    if (encoder->stat_maximum > 0 && size > encoder->stat_maximum) {
        encoder->stat_maximum = size;
    }

    /* Update the frame history */
    encoder->history_last = (encoder->history_last + 1) % GSTE_HISTORY_SIZE;
    if (encoder->history_last == encoder->history_first) {
        encoder->history_first = (encoder->history_first + 1) % GSTE_HISTORY_SIZE;
    }
    encoder->history[encoder->history_last].mm_time = frame_mm_time;
    encoder->history[encoder->history_last].size = size;
}


/* ---------- Encoder bit rate control ---------- */

static uint32_t get_min_playback_delay(SpiceGstEncoder *encoder)
{
    /* Make sure the delay is large enough to send a large frame (typically an
     * I frame) and an average frame. This also takes into account the frames
     * dropped by the encoder bit rate control.
     */
    uint32_t size = get_maximum_frame_size(encoder) + get_average_frame_size(encoder);
    uint32_t send_time = MILLI_SECOND * size * 8 / encoder->bit_rate;

    /* Also factor in the network latency with a margin for jitter. */
    uint32_t net_latency = get_network_latency(encoder) * (1.0 + GSTE_LATENCY_MARGIN);

    return send_time + net_latency;
}

static void update_client_playback_delay(SpiceGstEncoder *encoder)
{
    if (encoder->cbs.update_client_playback_delay) {
        uint32_t min_delay = get_min_playback_delay(encoder);
        encoder->cbs.update_client_playback_delay(encoder->cbs_opaque, min_delay);
    }
}

static void update_next_frame(SpiceGstEncoder *encoder)
{
    if (encoder->vbuffer_free >= 0) {
        encoder->next_frame = 0;
        return;
    }

    /* Figure out how many frames to drop to not exceed the current bit rate.
     * Use nanoseconds to avoid precision loss.
     */
    uint64_t delay_ns = -encoder->vbuffer_free * 8 * NANO_SECOND / encoder->bit_rate;
    uint64_t period_ns = NANO_SECOND / get_source_fps(encoder);
    uint32_t drops = (delay_ns + period_ns - 1) / period_ns; /* round up */
    spice_debug("drops=%u vbuffer %d/%d", drops, encoder->vbuffer_free,
                encoder->vbuffer_size);

    delay_ns = drops * period_ns + period_ns / 2;
    if (delay_ns > GSTE_MAX_PERIOD) {
        delay_ns = GSTE_MAX_PERIOD;
    }
    encoder->next_frame = get_last_frame_mm_time(encoder) + delay_ns / NANO_MS;

    /* Drops mean a higher delay between encoded frames so update the playback
     * delay.
     */
    update_client_playback_delay(encoder);
}


/* ---------- Network bit rate control ---------- */

/* The maximum bit rate we will use for the current video.
 *
 * This is based on a 10x compression ratio which should be more than enough
 * for even MJPEG to provide good quality.
 */
static uint64_t get_bit_rate_cap(SpiceGstEncoder *encoder)
{
    uint32_t raw_frame_bits = encoder->width * encoder->height * encoder->format->bpp;
    return raw_frame_bits * get_source_fps(encoder) / 10;
}

static void adjust_bit_rate(SpiceGstEncoder *encoder)
{
    if (encoder->bit_rate == 0) {
        /* Use the default value, */
        encoder->bit_rate = GSTE_DEFAULT_BITRATE;
    } else if (encoder->bit_rate < GSTE_MIN_BITRATE) {
        /* don't let the bit rate go too low */
        encoder->bit_rate = GSTE_MIN_BITRATE;
    } else {
        /* or too high */
        encoder->bit_rate = MIN(encoder->bit_rate, get_bit_rate_cap(encoder));
    }
    spice_debug("adjust_bit_rate(%.3fMbps)", get_mbps(encoder->bit_rate));
}


/* ---------- GStreamer pipeline ---------- */

/* A helper for gst_encoder_encode_frame(). */
static SpiceFormatForGStreamer *map_format(SpiceBitmapFmt format)
{
    /* See GStreamer's part-mediatype-video-raw.txt and
     * section-types-definitions.html documents.
     */
    static SpiceFormatForGStreamer format_map[] =  {
        {SPICE_BITMAP_FMT_RGBA, "BGRA", 32},
        /* TODO: Test the other formats */
        {SPICE_BITMAP_FMT_32BIT, "BGRx", 32},
        {SPICE_BITMAP_FMT_24BIT, "BGR", 24},
        {SPICE_BITMAP_FMT_16BIT, "BGR15", 16},
    };

    int i;
    for (i = 0; i < sizeof(format_map) / sizeof(*format_map); i++) {
        if (format_map[i].spice_format == format) {
            return &format_map[i];
        }
    }

    return NULL;
}

static void set_appsrc_caps(SpiceGstEncoder *encoder)
{
    if (encoder->src_caps) {
        gst_caps_unref(encoder->src_caps);
    }
    encoder->src_caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, encoder->format->format,
        "width", G_TYPE_INT, encoder->width,
        "height", G_TYPE_INT, encoder->height,
        "framerate", GST_TYPE_FRACTION, get_source_fps(encoder), 1,
        NULL);
    g_object_set(G_OBJECT(encoder->appsrc), "caps", encoder->src_caps, NULL);
}

/* A helper for gst_encoder_encode_frame(). */
static gboolean construct_pipeline(SpiceGstEncoder *encoder,
                                   const SpiceBitmap *bitmap)
{
    const gchar* gstenc_name;
    switch (encoder->base.codec_type)
    {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        gstenc_name = "avenc_mjpeg";
        break;
    case SPICE_VIDEO_CODEC_TYPE_VP8:
        gstenc_name = "vp8enc";
        break;
    case SPICE_VIDEO_CODEC_TYPE_H264:
        gstenc_name = "x264enc";
        break;
    default:
        /* gstreamer_encoder_new() should have rejected this codec type */
        spice_warning("unsupported codec type %d", encoder->base.codec_type);
        return FALSE;
    }

    GError *err = NULL;
    gchar *desc = g_strdup_printf("appsrc name=src format=2 do-timestamp=true ! videoconvert ! %s name=encoder ! appsink name=sink", gstenc_name);
    spice_debug("GStreamer pipeline: %s", desc);
    encoder->pipeline = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    g_free(desc);
    if (!encoder->pipeline || err) {
        spice_warning("GStreamer error: %s", err->message);
        g_clear_error(&err);
        if (encoder->pipeline) {
            gst_object_unref(encoder->pipeline);
            encoder->pipeline = NULL;
        }
        return FALSE;
    }
    encoder->appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(encoder->pipeline), "src"));
    encoder->gstenc = gst_bin_get_by_name(GST_BIN(encoder->pipeline), "encoder");
    encoder->appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(encoder->pipeline), "sink"));

    /* Configure the encoder bitrate, frame latency, etc. */
    adjust_bit_rate(encoder);
    switch (encoder->base.codec_type) {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        g_object_set(G_OBJECT(encoder->gstenc),
                     "bitrate", encoder->bit_rate,
                     "max-threads", 1, /* zero-frame latency */
                     NULL);
        break;
    case SPICE_VIDEO_CODEC_TYPE_VP8: {
        /* See http://www.webmproject.org/docs/encoder-parameters/ */
#ifdef HAVE_G_GET_NUMPROCESSORS
        int core_count = g_get_num_processors();
#else
        int core_count = 1;
#endif
        g_object_set(G_OBJECT(encoder->gstenc),
                     "resize-allowed", TRUE, /* for very low bit rates */
                     "target-bitrate", encoder->bit_rate,
                     "end-usage", 1, /* CBR */
                     "lag-in-frames", 0, /* zero-frame latency */
                     "error-resilient", 1, /* for client frame drops */
                     "deadline", 1000000 / get_source_fps(encoder) / 2, /* usec */
                     "threads", core_count - 1,
                     NULL);
        break;
        }
    case SPICE_VIDEO_CODEC_TYPE_H264:
        g_object_set(G_OBJECT(encoder->gstenc),
                     "bitrate", encoder->bit_rate / 1024,
                     "byte-stream", TRUE,
                     "aud", FALSE,
                     "tune", 4, /* zero-frame latency */
                     "sliced-threads", TRUE, /* zero-frame latency */
                     "speed-preset", 1, /* ultrafast */
                     "intra-refresh", TRUE, /* uniform compressed frame sizes */
                     NULL);
        break;
    default:
        /* gstreamer_encoder_new() should have rejected this codec type */
        spice_warning("unknown encoder type %d", encoder->base.codec_type);
        reset_pipeline(encoder);
        return FALSE;
    }

    /* Set the source caps */
    set_appsrc_caps(encoder);

    if (encoder->base.codec_type == SPICE_VIDEO_CODEC_TYPE_MJPEG) {
        /* See https://bugzilla.gnome.org/show_bug.cgi?id=753257 */
        spice_debug("removing the pipeline clock");
        gst_pipeline_use_clock(GST_PIPELINE(encoder->pipeline), NULL);
    }

    /* Start playing */
    spice_debug("setting state to PLAYING");
    if (gst_element_set_state(encoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        spice_warning("GStreamer error: unable to set the pipeline to the playing state");
        reset_pipeline(encoder);
        return FALSE;
    }

    return TRUE;
}

/* A helper for gst_encoder_encode_frame(). */
static void reconfigure_pipeline(SpiceGstEncoder *encoder)
{
    if (encoder->base.codec_type == SPICE_VIDEO_CODEC_TYPE_VP8) {
        /* vp8enc gets confused if we try to reconfigure the pipeline */
        reset_pipeline(encoder);
        return;
    }

    if (gst_element_set_state(encoder->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        spice_debug("GStreamer error: could not pause the pipeline, rebuilding it instead");
        reset_pipeline(encoder);
    }
    set_appsrc_caps(encoder);
    if (gst_element_set_state(encoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        spice_debug("GStreamer error: could not restart the pipeline, rebuilding it instead");
        reset_pipeline(encoder);
    }
}

/* A helper for push_raw_frame(). */
static inline int line_copy(SpiceGstEncoder *encoder, const SpiceBitmap *bitmap,
                            uint32_t chunk_offset, uint32_t stream_stride,
                            uint32_t height, uint8_t *buffer)
{
     uint8_t *dst = buffer;
     SpiceChunks *chunks = bitmap->data;
     uint32_t chunk_index = 0;
     for (int l = 0; l < height; l++) {
         /* We may have to move forward by more than one chunk the first
          * time around.
          */
         while (chunk_offset >= chunks->chunk[chunk_index].len) {
             /* Make sure that the chunk is not padded */
             if (chunks->chunk[chunk_index].len % bitmap->stride != 0) {
                 spice_warning("chunk %d/%d is padded, cannot copy line %d/%d",
                               chunk_index, chunks->num_chunks, l, height);
                 return FALSE;
             }
             chunk_offset -= chunks->chunk[chunk_index].len;
             chunk_index++;
         }

         /* Copy the line */
         uint8_t *src = chunks->chunk[chunk_index].data + chunk_offset;
         memcpy(dst, src, stream_stride);
         dst += stream_stride;
         chunk_offset += bitmap->stride;
     }
     spice_assert(dst - buffer == stream_stride * height);
     return TRUE;
}

#ifdef DO_ZERO_COPY
/* A helper for zero_copy() */
static void unref_bitmap(gpointer mem)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)mem;
    encoder->needs_bitmap = FALSE;
}

/* A helper for push_raw_frame(). */
static inline int zero_copy(SpiceGstEncoder *encoder, const SpiceBitmap *bitmap,
                            GstBuffer *buffer, uint32_t *chunk_index,
                            uint32_t *chunk_offset, uint32_t *len)
{
    /* We cannot control the lifetime of the bitmap but we can wrap it in
     * the buffer anyway because:
     * - Before returning from gst_encoder_encode_frame() we wait for the
     *   pipeline to have converted this frame into a compressed buffer.
     *   So it has to have gone through the frame at least once.
     * - For all encoders but MJPEG, the first element of the pipeline will
     *   convert the bitmap to another image format which entails copying
     *   it. So by the time the encoder starts its work, this buffer will
     *   not be needed anymore.
     * - The MJPEG encoder does not perform inter-frame compression and thus
     *   does not need to keep hold of this buffer once it has processed it.
     * encoder->needs_bitmap lets us verify that these conditions still hold
     * true through an assert.
     */
    SpiceChunks *chunks = bitmap->data;
    while (*chunk_offset >= chunks->chunk[*chunk_index].len) {
        /* Make sure that the chunk is not padded */
        if (chunks->chunk[*chunk_index].len % bitmap->stride != 0) {
            return FALSE;
        }
        *chunk_offset -= chunks->chunk[*chunk_index].len;
        (*chunk_index)++;
    }

    int max_mem = gst_buffer_get_max_memory();
    if (chunks->num_chunks - *chunk_index > max_mem) {
        /* There are more chunks than we can fit memory objects in a
         * buffer. This will cause the buffer to merge memory objects to
         * fit the extra chunks, which means doing wasteful memory copies.
         * So use the zero-copy approach for the first max_mem-1 chunks, and
         * let push_raw_frame() add another memory object to copy the rest.
         */
        max_mem = *chunk_index + max_mem - 1;
    } else {
        max_mem = chunks->num_chunks;
    }

    while (*len && *chunk_index < max_mem) {
        /* Make sure that the chunk is not padded */
        if (chunks->chunk[*chunk_index].len % bitmap->stride != 0) {
            spice_warning("chunk %d/%d is padded, cannot zero-copy", *chunk_index,
                          chunks->num_chunks);
            return FALSE;
        }
        uint32_t thislen = MIN(chunks->chunk[*chunk_index].len - *chunk_offset, *len);
        GstMemory *mem = gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY,
                                                chunks->chunk[*chunk_index].data,
                                                chunks->chunk[*chunk_index].len,
                                                *chunk_offset, thislen,
                                                encoder, &unref_bitmap);
        gst_buffer_append_memory(buffer, mem);
        *len -= thislen;
        *chunk_offset = 0;
        (*chunk_index)++;
    }
    encoder->needs_bitmap = TRUE;
    return TRUE;
}
#endif

/* A helper for gst_encoder_encode_frame(). */
static int push_raw_frame(SpiceGstEncoder *encoder, const SpiceBitmap *bitmap,
                          const SpiceRect *src, int top_down)
{
    const uint32_t height = src->bottom - src->top;
    const uint32_t stream_stride = (src->right - src->left) * encoder->format->bpp / 8;
    uint32_t len = stream_stride * height;
    GstBuffer *buffer = gst_buffer_new();
    /* TODO Use GST_MAP_INFO_INIT once GStreamer 1.4.5 is no longer relevant */
    GstMapInfo map = { .memory = NULL };

    /* Note that we should not reorder the lines, even if top_down is false.
     * It just changes the number of lines to skip at the start of the bitmap.
     */
    const uint32_t skip_lines = top_down ? src->top : bitmap->y - (src->bottom - 0);
    uint32_t chunk_offset = bitmap->stride * skip_lines;

    if (stream_stride != bitmap->stride) {
        /* We have to do a line-by-line copy because for each we have to leave
         * out pixels on the left or right.
         */
        GstMemory *mem = gst_allocator_alloc(NULL, len, NULL);
        if (!mem) {
            gst_buffer_unref(buffer);
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }
        spice_assert(gst_memory_map(mem, &map, GST_MAP_WRITE));
        uint8_t *dst = map.data;

        chunk_offset += src->left * encoder->format->bpp / 8;
        if (!line_copy(encoder, bitmap, chunk_offset, stream_stride, height, dst)) {
            gst_memory_unmap(map.memory, &map);
            gst_memory_unref(map.memory);
            gst_buffer_unref(buffer);
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }

    } else {
        SpiceChunks *chunks = bitmap->data;
        uint32_t chunk_index = 0;
        /* We can copy the bitmap chunk by chunk */
#ifdef DO_ZERO_COPY
        if (!zero_copy(encoder, bitmap, buffer, &chunk_index, &chunk_offset, &len)) {
            gst_buffer_unref(buffer);
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }
        /* Now we must avoid any write to the GstBuffer object as it would
         * cause a copy of the read-only memory objects we just added.
         * Fortunately we can append extra writable memory objects instead.
         */
#endif

        if (len) {
            GstMemory *mem = gst_allocator_alloc(NULL, len, NULL);
            if (!mem) {
                gst_buffer_unref(buffer);
                return VIDEO_ENCODER_FRAME_UNSUPPORTED;
            }
            spice_assert(gst_memory_map(mem, &map, GST_MAP_WRITE));
        }
        uint8_t *dst = map.data;

        while (len && chunk_index < chunks->num_chunks) {
            /* Make sure that the chunk is not padded */
            if (chunks->chunk[chunk_index].len % bitmap->stride != 0) {
                gst_memory_unmap(map.memory, &map);
                gst_memory_unref(map.memory);
                gst_buffer_unref(buffer);
                spice_warning("chunk %d/%d is padded, cannot copy it",
                              chunk_index, chunks->num_chunks);
                return VIDEO_ENCODER_FRAME_UNSUPPORTED;
            }
            if (chunk_offset >= chunks->chunk[chunk_index].len) {
                chunk_offset -= chunks->chunk[chunk_index].len;
                chunk_index++;
                continue;
            }

            uint8_t *src = chunks->chunk[chunk_index].data + chunk_offset;
            uint32_t thislen = MIN(chunks->chunk[chunk_index].len - chunk_offset, len);
            memcpy(dst, src, thislen);
            dst += thislen;
            len -= thislen;
            chunk_offset = 0;
            chunk_index++;
        }
        spice_assert(len == 0);
    }
    if (map.memory) {
        gst_memory_unmap(map.memory, &map);
        gst_buffer_append_memory(buffer, map.memory);
    }
    GST_BUFFER_OFFSET(buffer) = encoder->frame++;

    GstFlowReturn ret = gst_app_src_push_buffer(encoder->appsrc, buffer);
    if (ret != GST_FLOW_OK) {
        spice_debug("GStreamer error: unable to push source buffer (%d)", ret);
        return VIDEO_ENCODER_FRAME_UNSUPPORTED;
    }

    return VIDEO_ENCODER_FRAME_ENCODE_DONE;
}

/* A helper for gst_encoder_encode_frame(). */
static int pull_compressed_buffer(SpiceGstEncoder *encoder,
                                  VideoBuffer **video_buffer)
{
    GstSample *sample = gst_app_sink_pull_sample(encoder->appsink);
    if (sample) {
        SpiceGstVideoBuffer *buffer = create_gst_video_buffer();
        buffer->gst_buffer = gst_sample_get_buffer(sample);
        if (buffer->gst_buffer &&
            gst_buffer_map(buffer->gst_buffer, &buffer->map, GST_MAP_READ)) {
            buffer->base.data = buffer->map.data;
            buffer->base.size = gst_buffer_get_size(buffer->gst_buffer);
            *video_buffer = (VideoBuffer*)buffer;
            gst_buffer_ref(buffer->gst_buffer);
            gst_sample_unref(sample);
            return VIDEO_ENCODER_FRAME_ENCODE_DONE;
        }
        buffer->base.free((VideoBuffer*)buffer);
        gst_sample_unref(sample);
    }
    spice_debug("failed to pull the compressed buffer");
    return VIDEO_ENCODER_FRAME_UNSUPPORTED;
}


/* ---------- VideoEncoder's public API ---------- */

static void gst_encoder_destroy(VideoEncoder *video_encoder)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    reset_pipeline(encoder);
    free(encoder);
}

static int gst_encoder_encode_frame(VideoEncoder *video_encoder,
                                    const SpiceBitmap *bitmap,
                                    int width, int height,
                                    const SpiceRect *src, int top_down,
                                    uint32_t frame_mm_time,
                                    VideoBuffer **video_buffer)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    if (width != encoder->width || height != encoder->height ||
        encoder->spice_format != bitmap->format) {
        spice_debug("video format change: width %d -> %d, height %d -> %d, format %d -> %d",
                    encoder->width, width, encoder->height, height,
                    encoder->spice_format, bitmap->format);
        encoder->format = map_format(bitmap->format);
        if (!encoder->format) {
            spice_debug("unable to map format type %d", bitmap->format);
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }
        encoder->spice_format = bitmap->format;
        encoder->width = width;
        encoder->height = height;
        if (encoder->bit_rate == 0) {
            encoder->history[0].mm_time = frame_mm_time;
            encoder->bit_rate = encoder->starting_bit_rate;
            adjust_bit_rate(encoder);
            encoder->vbuffer_free = 0; /* Slow start */
        } else if (encoder->pipeline) {
            reconfigure_pipeline(encoder);
        }
    }

    if (rate_control_is_active(encoder) &&
        frame_mm_time < encoder->next_frame) {
        /* Drop the frame to limit the outgoing bit rate. */
        return VIDEO_ENCODER_FRAME_DROP;
    }

    if (!encoder->pipeline && !construct_pipeline(encoder, bitmap)) {
        return VIDEO_ENCODER_FRAME_UNSUPPORTED;
    }

    int rc = push_raw_frame(encoder, bitmap, src, top_down);
    if (rc == VIDEO_ENCODER_FRAME_ENCODE_DONE) {
        rc = pull_compressed_buffer(encoder, video_buffer);
#ifdef DO_ZERO_COPY
        /* GStreamer should have released the source frame buffer by now */
        spice_assert(!encoder->needs_bitmap);
#endif
    }
    if (rc != VIDEO_ENCODER_FRAME_ENCODE_DONE) {
        return rc;
    }
    add_frame(encoder, frame_mm_time, (*video_buffer)->size);

    update_next_frame(encoder);

    return rc;
}

static void gst_encoder_client_stream_report(VideoEncoder *video_encoder,
                                             uint32_t num_frames,
                                             uint32_t num_drops,
                                             uint32_t start_frame_mm_time,
                                             uint32_t end_frame_mm_time,
                                             int32_t end_frame_delay,
                                             uint32_t audio_delay)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    uint64_t period_bit_rate = get_period_bit_rate(encoder, start_frame_mm_time, end_frame_mm_time);
    spice_debug("client report: %u/%u drops in %ums margins video %3d audio %3u bw %.3f/%.3fMbps",
                num_drops, num_frames, end_frame_mm_time - start_frame_mm_time,
                end_frame_delay, audio_delay,
                get_mbps(period_bit_rate),
                get_mbps(get_effective_bit_rate(encoder)));
}

static void gst_encoder_notify_server_frame_drop(VideoEncoder *video_encoder)
{
    spice_debug("server report: getting frame drops...");
}

static uint64_t gst_encoder_get_bit_rate(VideoEncoder *video_encoder)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    return get_effective_bit_rate(encoder);
}

static void gst_encoder_get_stats(VideoEncoder *video_encoder,
                                  VideoEncoderStats *stats)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    uint64_t raw_bit_rate = encoder->width * encoder->height * (encoder->format ? encoder->format->bpp : 0) * get_source_fps(encoder);

    spice_assert(encoder != NULL && stats != NULL);
    stats->starting_bit_rate = encoder->starting_bit_rate;
    stats->cur_bit_rate = get_effective_bit_rate(encoder);

    /* Use the compression level as a proxy for the quality */
    stats->avg_quality = stats->cur_bit_rate ? 100.0 - raw_bit_rate / stats->cur_bit_rate : 0;
    if (stats->avg_quality < 0) {
        stats->avg_quality = 0;
    }
}

VideoEncoder *gstreamer_encoder_new(SpiceVideoCodecType codec_type,
                                    uint64_t starting_bit_rate,
                                    VideoEncoderRateControlCbs *cbs,
                                    void *cbs_opaque)
{
    SpiceGstEncoder *encoder;

    spice_assert(GSTE_FRAME_STATISTICS_COUNT <= GSTE_HISTORY_SIZE);

    spice_assert(!cbs || (cbs && cbs->get_roundtrip_ms && cbs->get_source_fps));
    if (codec_type != SPICE_VIDEO_CODEC_TYPE_MJPEG &&
        codec_type != SPICE_VIDEO_CODEC_TYPE_VP8 &&
        codec_type != SPICE_VIDEO_CODEC_TYPE_H264) {
        spice_warning("unsupported codec type %d", codec_type);
        return NULL;
    }

    GError *err = NULL;
    if (!gst_init_check(NULL, NULL, &err)) {
        spice_warning("GStreamer error: %s", err->message);
        g_clear_error(&err);
        return NULL;
    }

    encoder = spice_new0(SpiceGstEncoder, 1);
    encoder->base.destroy = &gst_encoder_destroy;
    encoder->base.encode_frame = &gst_encoder_encode_frame;
    encoder->base.client_stream_report = &gst_encoder_client_stream_report;
    encoder->base.notify_server_frame_drop = &gst_encoder_notify_server_frame_drop;
    encoder->base.get_bit_rate = &gst_encoder_get_bit_rate;
    encoder->base.get_stats = &gst_encoder_get_stats;
    encoder->base.codec_type = codec_type;

    if (cbs) {
        encoder->cbs = *cbs;
    }
    encoder->cbs_opaque = cbs_opaque;
    encoder->starting_bit_rate = starting_bit_rate;

    /* All the other fields are initialized to zero by spice_new0(). */

    return (VideoEncoder*)encoder;
}
