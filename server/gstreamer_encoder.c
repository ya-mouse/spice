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


typedef struct {
    SpiceBitmapFmt spice_format;
    const char *format;
    uint32_t bpp;
} SpiceFormatForGStreamer;

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

    /* The frame counter for GStreamer buffers */
    uint32_t frame;

    /* The bit rate target for the outgoing network stream. (bits per second) */
    uint64_t bit_rate;

    /* The minimum bit rate */
#   define GSTE_MIN_BITRATE (128 * 1024)

    /* The default bit rate */
#   define GSTE_DEFAULT_BITRATE (8 * 1024 * 1024)
} SpiceGstEncoder;


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
    GError *err = NULL;
    const gchar *desc = "appsrc name=src format=2 do-timestamp=true ! videoconvert ! avenc_mjpeg name=encoder ! appsink name=sink";
    spice_debug("GStreamer pipeline: %s", desc);
    encoder->pipeline = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    if (!encoder->pipeline || err) {
        spice_warning("GStreamer error: %s", err->message);
        g_clear_error(&err);
        return FALSE;
    }
    encoder->appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(encoder->pipeline), "src"));
    encoder->gstenc = gst_bin_get_by_name(GST_BIN(encoder->pipeline), "encoder");
    encoder->appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(encoder->pipeline), "sink"));

    /* Configure the encoder bitrate, frame latency, etc. */
    adjust_bit_rate(encoder);
    g_object_set(G_OBJECT(encoder->gstenc),
                 "bitrate", encoder->bit_rate,
                 "max-threads", 1, /* zero-frame latency */
                 NULL);

    /* Set the source caps */
    set_appsrc_caps(encoder);

    /* See https://bugzilla.gnome.org/show_bug.cgi?id=753257 */
    spice_debug("removing the pipeline clock");
    gst_pipeline_use_clock(GST_PIPELINE(encoder->pipeline), NULL);

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

/* A helper for gst_encoder_encode_frame(). */
static int push_raw_frame(SpiceGstEncoder *encoder, const SpiceBitmap *bitmap,
                          const SpiceRect *src, int top_down)
{
    const uint32_t height = src->bottom - src->top;
    const uint32_t stream_stride = (src->right - src->left) * encoder->format->bpp / 8;
    uint32_t len = stream_stride * height;
    GstBuffer *buffer = gst_buffer_new_and_alloc(len);
    GstMapInfo map;
    gst_buffer_map(buffer, &map, GST_MAP_WRITE);
    uint8_t *dst = map.data;

    /* Note that we should not reorder the lines, even if top_down is false.
     * It just changes the number of lines to skip at the start of the bitmap.
     */
    const uint32_t skip_lines = top_down ? src->top : bitmap->y - (src->bottom - 0);
    uint32_t chunk_offset = bitmap->stride * skip_lines;

    if (stream_stride != bitmap->stride) {
        /* We have to do a line-by-line copy because for each we have to leave
         * out pixels on the left or right.
         */
        chunk_offset += src->left * encoder->format->bpp / 8;
        if (!line_copy(encoder, bitmap, chunk_offset, stream_stride, height, dst)) {
            gst_buffer_unmap(buffer, &map);
            gst_buffer_unref(buffer);
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }

    } else {
        SpiceChunks *chunks = bitmap->data;
        uint32_t chunk_index = 0;
        /* We can copy the bitmap chunk by chunk */
        while (len && chunk_index < chunks->num_chunks) {
            /* Make sure that the chunk is not padded */
            if (chunks->chunk[chunk_index].len % bitmap->stride != 0) {
                gst_buffer_unmap(buffer, &map);
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
    gst_buffer_unmap(buffer, &map);
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
                                  uint8_t **outbuf, size_t *outbuf_size,
                                  int *data_size)
{
    GstSample *sample = gst_app_sink_pull_sample(encoder->appsink);
    if (sample) {
        GstMapInfo map;
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            int size = gst_buffer_get_size(buffer);
            spice_assert(outbuf && outbuf_size);
            if (!*outbuf || *outbuf_size < size) {
                free(*outbuf);
                *outbuf = spice_malloc(size);
                *outbuf_size = size;
            }
            /* TODO Try to avoid this copy by changing the GstBuffer handling */
            memcpy(*outbuf, map.data, size);
            *data_size = size;
            gst_buffer_unmap(buffer, &map);
            gst_sample_unref(sample);
            return VIDEO_ENCODER_FRAME_ENCODE_DONE;
        }
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
                                    uint8_t **outbuf, size_t *outbuf_size,
                                    int *data_size)
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
        if (encoder->pipeline) {
            reconfigure_pipeline(encoder);
        }
    }
    if (!encoder->pipeline && !construct_pipeline(encoder, bitmap)) {
        return VIDEO_ENCODER_FRAME_UNSUPPORTED;
    }

    int rc = push_raw_frame(encoder, bitmap, src, top_down);
    if (rc == VIDEO_ENCODER_FRAME_ENCODE_DONE) {
        rc = pull_compressed_buffer(encoder, outbuf, outbuf_size, data_size);
    }
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
    spice_debug("client report: #frames %u, #drops %d, duration %u video-delay %d audio-delay %u",
                num_frames, num_drops,
                end_frame_mm_time - start_frame_mm_time,
                end_frame_delay, audio_delay);
}

static void gst_encoder_notify_server_frame_drop(VideoEncoder *video_encoder)
{
    spice_debug("server report: getting frame drops...");
}

static uint64_t gst_encoder_get_bit_rate(VideoEncoder *video_encoder)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    return encoder->bit_rate;
}

static void gst_encoder_get_stats(VideoEncoder *video_encoder,
                                  VideoEncoderStats *stats)
{
    SpiceGstEncoder *encoder = (SpiceGstEncoder*)video_encoder;
    uint64_t raw_bit_rate = encoder->width * encoder->height * (encoder->format ? encoder->format->bpp : 0) * get_source_fps(encoder);

    spice_assert(encoder != NULL && stats != NULL);
    stats->starting_bit_rate = encoder->starting_bit_rate;
    stats->cur_bit_rate = encoder->bit_rate;

    /* Use the compression level as a proxy for the quality */
    stats->avg_quality = stats->cur_bit_rate ? 100.0 - raw_bit_rate / stats->cur_bit_rate : 0;
    if (stats->avg_quality < 0) {
        stats->avg_quality = 0;
    }
}

VideoEncoder *gstreamer_encoder_new(uint64_t starting_bit_rate,
                                    VideoEncoderRateControlCbs *cbs,
                                    void *cbs_opaque)
{
    SpiceGstEncoder *encoder;

    spice_assert(!cbs || (cbs && cbs->get_roundtrip_ms && cbs->get_source_fps));

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

    if (cbs) {
        encoder->cbs = *cbs;
    }
    encoder->cbs_opaque = cbs_opaque;
    encoder->bit_rate = encoder->starting_bit_rate = starting_bit_rate;

    /* All the other fields are initialized to zero by spice_new0(). */

    return (VideoEncoder*)encoder;
}
