/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2015-2016, 2019 CodeWeavers, Inc

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"

#include "spice-client.h"
#include "spice-common.h"
#include "spice-channel-priv.h"
#include "common/recorder.h"

#include "channel-display-priv.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/gstvideometa.h>


typedef struct SpiceGstFrame SpiceGstFrame;

/* GStreamer decoder implementation */

#if GST_CHECK_VERSION(1,14,0)
static GstStaticCaps stream_reference = GST_STATIC_CAPS("timestamp/spice-stream");
#endif

typedef struct SpiceGstDecoder {
    VideoDecoder base;

    /* ---------- GStreamer pipeline ---------- */

    GstAppSrc *appsrc;
    GstAppSink *appsink;
    GstElement *pipeline;
    GstClock *clock;

    /* ---------- Decoding and display queues ---------- */

    uint32_t last_mm_time;

    GMutex queues_mutex;
    GQueue *decoding_queue;
    SpiceGstFrame *display_frame;
    guint timer_id;
    guint pending_samples;
} SpiceGstDecoder;

#define VALID_VIDEO_CODEC_TYPE(codec) \
    (codec > 0 && codec < G_N_ELEMENTS(gst_opts))

/* Decoded frames are big so limit how many are queued by GStreamer */
#define MAX_DECODED_FRAMES 2

/* GstPlayFlags enum is in plugin's header which should not be exported.
 * https://bugzilla.gnome.org/show_bug.cgi?id=784279
 */
typedef enum {
  GST_PLAY_FLAG_VIDEO             = (1 << 0),
  GST_PLAY_FLAG_AUDIO             = (1 << 1),
  GST_PLAY_FLAG_TEXT              = (1 << 2),
  GST_PLAY_FLAG_VIS               = (1 << 3),
  GST_PLAY_FLAG_SOFT_VOLUME       = (1 << 4),
  GST_PLAY_FLAG_NATIVE_AUDIO      = (1 << 5),
  GST_PLAY_FLAG_NATIVE_VIDEO      = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD          = (1 << 7),
  GST_PLAY_FLAG_BUFFERING         = (1 << 8),
  GST_PLAY_FLAG_DEINTERLACE       = (1 << 9),
  GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10),
  GST_PLAY_FLAG_FORCE_FILTERS     = (1 << 11),
} SpiceGstPlayFlags;

/* ---------- SpiceGstFrame ---------- */

struct SpiceGstFrame {
    GstClockTime timestamp;
    GstBuffer *encoded_buffer;
    SpiceFrame *encoded_frame;
    GstSample *decoded_sample;
    guint queue_len;
};

static SpiceGstFrame *create_gst_frame(GstBuffer *buffer, SpiceFrame *frame)
{
    SpiceGstFrame *gstframe = g_new(SpiceGstFrame, 1);

    gstframe->timestamp = GST_BUFFER_PTS(buffer);
#if GST_CHECK_VERSION(1,14,0)
    GstReferenceTimestampMeta *time_meta;

    time_meta = gst_buffer_get_reference_timestamp_meta(buffer, gst_static_caps_get(&stream_reference));
    if (time_meta) {
        gstframe->timestamp = time_meta->timestamp;
    }
#endif
    gstframe->encoded_buffer = gst_buffer_ref(buffer);
    gstframe->encoded_frame = frame;
    gstframe->decoded_sample = NULL;
    return gstframe;
}

static void free_gst_frame(SpiceGstFrame *gstframe)
{
    gst_buffer_unref(gstframe->encoded_buffer);
    // encoded_frame was owned by encoded_buffer, don't release it
    g_clear_pointer(&gstframe->decoded_sample, gst_sample_unref);
    g_free(gstframe);
}


/* ---------- GStreamer pipeline ---------- */

static void schedule_frame(SpiceGstDecoder *decoder);

RECORDER(frames_stats, 64, "Frames statistics");

static int spice_gst_buffer_get_stride(GstBuffer *buffer)
{
    GstVideoMeta *video = gst_buffer_get_video_meta(buffer);
    return video && video->n_planes > 0 ? video->stride[0] : SPICE_UNKNOWN_STRIDE;
}

/* main context */
static gboolean display_frame(gpointer video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;
    SpiceGstFrame *gstframe;
    GstCaps *caps;
    gint width, height;
    GstStructure *s;
    GstBuffer *buffer;
    GstMapInfo mapinfo;

    g_mutex_lock(&decoder->queues_mutex);
    decoder->timer_id = 0;
    gstframe = decoder->display_frame;
    decoder->display_frame = NULL;
    g_mutex_unlock(&decoder->queues_mutex);
    /* If the queue is empty we don't even need to reschedule */
    g_return_val_if_fail(gstframe, G_SOURCE_REMOVE);

    if (!gstframe->decoded_sample) {
        spice_warning("got a frame without a sample!");
        goto error;
    }

    caps = gst_sample_get_caps(gstframe->decoded_sample);
    if (!caps) {
        spice_warning("GStreamer error: could not get the caps of the sample");
        goto error;
    }

    s = gst_caps_get_structure(caps, 0);
    if (!gst_structure_get_int(s, "width", &width) ||
        !gst_structure_get_int(s, "height", &height)) {
        spice_warning("GStreamer error: could not get the size of the frame");
        goto error;
    }

    buffer = gst_sample_get_buffer(gstframe->decoded_sample);
    if (!gst_buffer_map(buffer, &mapinfo, GST_MAP_READ)) {
        spice_warning("GStreamer error: could not map the buffer");
        goto error;
    }

    stream_display_frame(decoder->base.stream, gstframe->encoded_frame,
                         width, height, spice_gst_buffer_get_stride(buffer), mapinfo.data);
    gst_buffer_unmap(buffer, &mapinfo);

 error:
    free_gst_frame(gstframe);
    schedule_frame(decoder);
    return G_SOURCE_REMOVE;
}

/* Returns the decoding queue entry that matches the specified GStreamer buffer.
 *
 * The entry is identified based on the buffer timestamp. However sometimes
 * GStreamer returns the same buffer twice (and the second time the entry may
 * have been removed already) or buffers that have a modified, and thus
 * unrecognizable, timestamp. In such cases NULL is returned.
 *
 * queues_mutex must be held.
 */
static GList *find_frame_entry(SpiceGstDecoder *decoder, GstBuffer *buffer)
{
    GstClockTime buffer_ts = GST_BUFFER_PTS(buffer);
#if GST_CHECK_VERSION(1,14,0)
    GstReferenceTimestampMeta *time_meta;

    time_meta = gst_buffer_get_reference_timestamp_meta(buffer, gst_static_caps_get(&stream_reference));
    if (time_meta) {
        buffer_ts = time_meta->timestamp;
    }
#endif

    GList *l = g_queue_peek_head_link(decoder->decoding_queue);
    while (l) {
        const SpiceGstFrame *gstframe = l->data;
        if (gstframe->timestamp == buffer_ts) {
            return l;
        }
        l = l->next;
    }

    return NULL;
}

/* Pops the queued frames up to and including the specified frame.
 * All frames are freed except that last frame which belongs to the caller.
 * Returns the number of freed frames.
 *
 * queues_mutex must be held.
 */
static guint32 pop_up_to_frame(SpiceGstDecoder *decoder, const SpiceGstFrame *popframe)
{
    SpiceGstFrame *gstframe;
    guint32 freed = 0;

    while ((gstframe = g_queue_pop_head(decoder->decoding_queue)) != popframe) {
        free_gst_frame(gstframe);
        freed++;
    }
    return freed;
}

/* Helper for schedule_frame().
 *
 * queues_mutex must be held.
 */
static void fetch_pending_sample(SpiceGstDecoder *decoder)
{
    GstSample *sample = gst_app_sink_pull_sample(decoder->appsink);
    if (sample) {
        // account for the fetched sample
        decoder->pending_samples--;

        GstBuffer *buffer = gst_sample_get_buffer(sample);

        /* gst_app_sink_pull_sample() sometimes returns the same buffer twice
         * or buffers that have a modified, and thus unrecognizable, PTS.
         * Blindly removing frames from the decoding_queue until we find a
         * match would only empty the queue, resulting in later buffers not
         * finding a match either, etc. So check the buffer has a matching
         * frame first.
         */
        GList *l = find_frame_entry(decoder, buffer);
        if (l) {
            SpiceGstFrame *gstframe = l->data;

            /* Dequeue this and any dropped frames */
            guint32 dropped = pop_up_to_frame(decoder, gstframe);
            if (dropped) {
                SPICE_DEBUG("the GStreamer pipeline dropped %u frames", dropped);
            }

            /* The frame is now ready for display */
            gstframe->decoded_sample = sample;
            decoder->display_frame = gstframe;
        } else {
            spice_warning("got an unexpected decoded buffer!");
            gst_sample_unref(sample);
        }
    } else {
        // no more samples to get, possibly some sample was dropped
        decoder->pending_samples = 0;
        spice_warning("GStreamer error: could not pull sample");
    }
}

/* main loop or GStreamer streaming thread */
static void schedule_frame(SpiceGstDecoder *decoder)
{
    guint32 now = stream_get_time(decoder->base.stream);
    g_mutex_lock(&decoder->queues_mutex);

    while (!decoder->timer_id) {
        while (decoder->display_frame == NULL && decoder->pending_samples) {
            fetch_pending_sample(decoder);
        }

        SpiceGstFrame *gstframe = decoder->display_frame;
        if (!gstframe) {
            break;
        }

        if (spice_mmtime_diff(gstframe->encoded_frame->mm_time, now) >= 0) {
            decoder->timer_id = g_timeout_add(gstframe->encoded_frame->mm_time - now,
                                              display_frame, decoder);
        } else if (decoder->display_frame && !decoder->pending_samples) {
            /* Still attempt to display the least out of date frame so the
             * video is not completely frozen for an extended period of time.
             */
            decoder->timer_id = g_timeout_add(0, display_frame, decoder);
        } else {
            SPICE_DEBUG("%s: rendering too late by %u ms (ts: %u, mmtime: %u), dropping",
                        __FUNCTION__, now - gstframe->encoded_frame->mm_time,
                        gstframe->encoded_frame->mm_time, now);
            stream_dropped_frame_on_playback(decoder->base.stream);
            decoder->display_frame = NULL;
            free_gst_frame(gstframe);
        }
    }

    g_mutex_unlock(&decoder->queues_mutex);
}

/* GStreamer thread
 *
 * Decoded frames are big so we rely on GStreamer to limit how many are
 * buffered (see MAX_DECODED_FRAMES). This means we must not pull the samples
 * as soon as they become available. Instead just increment pending_samples so
 * schedule_frame() knows whether it can pull a new sample when it needs one.
 *
 * Note that GStreamer's signals are not always run in the main context, hence
 * the schedule_frame() + display_frame() mechanism. So we might as well use
 * a callback here (lower overhead).
 */
static GstFlowReturn new_sample(GstAppSink *gstappsink, gpointer video_decoder)
{
    SpiceGstDecoder *decoder = video_decoder;

    g_mutex_lock(&decoder->queues_mutex);
    decoder->pending_samples++;
    if (decoder->timer_id && decoder->display_frame) {
        g_mutex_unlock(&decoder->queues_mutex);
        return GST_FLOW_OK;
    }
    g_mutex_unlock(&decoder->queues_mutex);

    schedule_frame(decoder);

    return GST_FLOW_OK;
}

static void free_pipeline(SpiceGstDecoder *decoder)
{
    if (!decoder->pipeline) {
        return;
    }

    gst_element_set_state(decoder->pipeline, GST_STATE_NULL);
    gst_object_unref(decoder->appsrc);
    if (decoder->appsink) {
        gst_object_unref(decoder->appsink);
    }
    gst_object_unref(decoder->pipeline);
    gst_object_unref(decoder->clock);
    decoder->pipeline = NULL;
}

static gboolean handle_pipeline_message(GstBus *bus, GstMessage *msg, gpointer video_decoder)
{
    SpiceGstDecoder *decoder = video_decoder;

    switch(GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug_info = NULL;
        gst_message_parse_error(msg, &err, &debug_info);
        spice_warning("GStreamer error from element %s: %s",
                      GST_OBJECT_NAME(msg->src), err->message);
        if (debug_info) {
            SPICE_DEBUG("debug information: %s", debug_info);
            g_free(debug_info);
        }
        g_clear_error(&err);

        /* We won't be able to process any more frame anyway */
        free_pipeline(decoder);
        break;
    }
    case GST_MESSAGE_STREAM_START: {
        gchar *filename = g_strdup_printf("spice-gtk-gst-pipeline-debug-%" G_GUINT32_FORMAT "-%s",
                                          decoder->base.stream->id,
                                          gst_opts[decoder->base.codec_type].name);
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(decoder->pipeline),
                                  GST_DEBUG_GRAPH_SHOW_ALL
                                    | GST_DEBUG_GRAPH_SHOW_FULL_PARAMS
                                    | GST_DEBUG_GRAPH_SHOW_STATES,
                                    filename);
        g_free(filename);
        break;
    }
    default:
        /* not being handled */
        break;
    }
    return TRUE;
}

static void app_source_setup(GstElement *pipeline G_GNUC_UNUSED,
                             GstElement *source,
                             SpiceGstDecoder *decoder)
{
    GstCaps *caps;

    /* - We schedule the frame display ourselves so set sync=false on appsink
     *   so the pipeline decodes them as fast as possible. This will also
     *   minimize the risk of frames getting lost when we rebuild the
     *   pipeline.
     * - Set max-bytes=0 on appsrc so it does not drop frames that may be
     *   needed by those that follow.
     */
    caps = gst_caps_from_string(gst_opts[decoder->base.codec_type].dec_caps);
    g_object_set(source,
                 "caps", caps,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "max-bytes", G_GINT64_CONSTANT(0),
                 "block", TRUE,
                 NULL);
    gst_caps_unref(caps);
    decoder->appsrc = GST_APP_SRC(gst_object_ref(source));
}

static GstPadProbeReturn
sink_event_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)data;

    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) { // Buffer arrived
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        g_mutex_lock(&decoder->queues_mutex);

        GList *l = find_frame_entry(decoder, buffer);
        if (l) {
            SpiceGstFrame *gstframe = l->data;
            const SpiceFrame *frame = gstframe->encoded_frame;
            int64_t duration = g_get_monotonic_time() - frame->creation_time;
            /* Note that queue_len (the length of the queue prior to adding
             * this frame) is crucial to correctly interpret the decoding time:
             * - Less than MAX_DECODED_FRAMES means nothing blocked the
             *   decoding of that frame.
             * - More than MAX_DECODED_FRAMES means decoding was delayed by one
             *   or more frame intervals.
             */
            record(frames_stats,
                   "frame mm_time %u size %u creation time %" PRId64
                   " decoded time %" PRId64 " queue %u before %u",
                   frame->mm_time, frame->size, frame->creation_time, duration,
                   decoder->decoding_queue->length, gstframe->queue_len);

            if (!decoder->appsink) {
                /* The sink will display the frame directly so this
                 * SpiceGstFrame and those of any dropped frame are no longer
                 * needed.
                 */
                pop_up_to_frame(decoder, gstframe);
                free_gst_frame(gstframe);
            }
        }

        g_mutex_unlock(&decoder->queues_mutex);
    }
    return GST_PAD_PROBE_OK;
}

static inline const char *gst_element_name(GstElement *element)
{
   GstElementFactory *f = gst_element_get_factory(element);
   return f ? GST_OBJECT_NAME(f) : GST_OBJECT_NAME(element);
}

/* This function is used to set a probe on the sink */
static void
deep_element_added_cb(GstBin *pipeline, GstBin *bin, GstElement *element,
                      SpiceGstDecoder *decoder)
{
     SPICE_DEBUG("A new element was added to Gstreamer's pipeline (%s)",
                 gst_element_name(element));
    /* Attach a probe to the sink to update the statistics */
    if (GST_IS_BASE_SINK(element)) {
        GstPad *pad = gst_element_get_static_pad(element, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, sink_event_probe, decoder, NULL);
        gst_object_unref(pad);
    }
}

static gboolean create_pipeline(SpiceGstDecoder *decoder)
{
    GstBus *bus;
    GstElement *playbin, *sink;
    SpiceGstPlayFlags flags;
    GstCaps *caps;

    playbin = gst_element_factory_make("playbin", "playbin");
    if (playbin == NULL) {
        spice_warning("error upon creation of 'playbin' element");
        return FALSE;
    }

    /* Passing the pipeline to widget, try to get window handle and
     * set the GstVideoOverlay interface, setting overlay to the window
     * will happen only when prepare-window-handle message is received
     */
    if (!hand_pipeline_to_widget(decoder->base.stream, GST_PIPELINE(playbin))) {
        sink = gst_element_factory_make("appsink", "sink");
        if (sink == NULL) {
            spice_warning("error upon creation of 'appsink' element");
            gst_object_unref(playbin);
            return FALSE;
        }
        caps = gst_caps_from_string("video/x-raw,format=BGRx");
        g_object_set(sink,
                 "caps", caps,
                 "sync", FALSE,
                 "drop", FALSE,
                 NULL);
        gst_caps_unref(caps);
        g_object_set(playbin,
                 "video-sink", gst_object_ref(sink),
                 NULL);

        decoder->appsink = GST_APP_SINK(sink);
    } else {
        /* handle has received, it means playbin will render directly into
         * widget using the gstvideooverlay interface instead of app-sink.
         */
        SPICE_DEBUG("Video is presented using gstreamer's GstVideoOverlay interface");

#if !GST_CHECK_VERSION(1,14,0)
        /* Avoid using vaapisink if exist since vaapisink could be
         * buggy when it is combined with playbin. changing its rank to
         * none will make playbin to avoid of using it.
         */
        GstRegistry *registry = NULL;
        GstPluginFeature *vaapisink = NULL;

        registry = gst_registry_get();
        if (registry) {
            vaapisink = gst_registry_lookup_feature(registry, "vaapisink");
        }
        if (vaapisink) {
            gst_plugin_feature_set_rank(vaapisink, GST_RANK_NONE);
            gst_object_unref(vaapisink);
        }
#endif
    }

    g_signal_connect(playbin, "deep-element-added", G_CALLBACK(deep_element_added_cb), decoder);
    g_signal_connect(playbin, "source-setup", G_CALLBACK(app_source_setup), decoder);

    g_object_set(playbin,
                 "uri", "appsrc://",
                 NULL);

    /* Disable audio in playbin */
    g_object_get(playbin, "flags", &flags, NULL);
    flags &= ~(GST_PLAY_FLAG_AUDIO | GST_PLAY_FLAG_TEXT);
    g_object_set(playbin, "flags", flags, NULL);

    g_warn_if_fail(decoder->appsrc == NULL);
    decoder->pipeline = playbin;

    if (decoder->appsink) {
        GstAppSinkCallbacks appsink_cbs = { NULL };
        appsink_cbs.new_sample = new_sample;
        gst_app_sink_set_callbacks(decoder->appsink, &appsink_cbs, decoder, NULL);
        gst_app_sink_set_max_buffers(decoder->appsink, MAX_DECODED_FRAMES);
        gst_app_sink_set_drop(decoder->appsink, FALSE);
    }
    bus = gst_pipeline_get_bus(GST_PIPELINE(decoder->pipeline));
    gst_bus_add_watch(bus, handle_pipeline_message, decoder);
    gst_object_unref(bus);

    decoder->clock = gst_pipeline_get_clock(GST_PIPELINE(decoder->pipeline));

    if (gst_element_set_state(decoder->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        SPICE_DEBUG("GStreamer error: Unable to set the pipeline to the playing state.");
        free_pipeline(decoder);
        return FALSE;
    }

    return TRUE;
}


/* ---------- VideoDecoder's public API ---------- */

static void spice_gst_decoder_reschedule(VideoDecoder *video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    if (!decoder->appsink) {
        return;
    }
    guint timer_id;

    g_mutex_lock(&decoder->queues_mutex);
    timer_id = decoder->timer_id;
    decoder->timer_id = 0;
    g_mutex_unlock(&decoder->queues_mutex);

    if (timer_id != 0) {
        g_source_remove(timer_id);
    }
    schedule_frame(decoder);
}

/* main context */
static void spice_gst_decoder_destroy(VideoDecoder *video_decoder)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    /* Stop and free the pipeline to ensure there will not be any further
     * new_sample() call (clearing thread-safety concerns).
     */
    free_pipeline(decoder);

    /* Even if we kept the decoder around, once we return the stream will be
     * destroyed making it impossible to display frames. So cancel any
     * scheduled display_frame() call and drop the queued frames.
     */
    if (decoder->timer_id) {
        g_source_remove(decoder->timer_id);
    }
    g_mutex_clear(&decoder->queues_mutex);
    g_queue_free_full(decoder->decoding_queue, (GDestroyNotify)free_gst_frame);
    if (decoder->display_frame) {
        free_gst_frame(decoder->display_frame);
    }

    g_free(decoder);

    /* Don't call gst_deinit() as other parts of the client
     * may still be using GStreamer.
     */
}


/* spice_gst_decoder_queue_frame() queues the SpiceFrame for decoding and
 * displaying. The steps it goes through are as follows:
 *
 * 1) frame->data, which contains the compressed frame data, is wrapped in a GstBuffer
 *    (encoded_buffer) which owns the SpiceFrame.
 * 2) A SpiceGstFrame is created to keep track of SpiceFrame (encoded_frame),
 *    and additional metadata among which GStreamer's encoded_buffer the
 *    refcount of which is incremented. The SpiceGstFrame is then pushed into
 *    the decoding_queue.
 *
 * If GstVideoOverlay is used (window handle was obtained successfully at the widget):
 *   3) Decompressed frames will be rendered to widget directly from GStreamer's pipeline
 *      using some GStreamer sink plugin which implements the GstVideoOverlay interface
 *      (last step).
 *   4) As soon as GStreamer's pipeline no longer needs the compressed frame it will
 *      unref the encoded_buffer.
 *   5) Once a decoded buffer arrives to the sink sink_event_probe() will pop
 *      its matching SpiceGstFrame from the decoding_queue and free it using
 *      free_gst_frame(). This will also unref the encoded_buffer which will
 *      allow GStreamer to call spice_frame_free() and free its encoded_frame.
 *
 * Otherwise appsink is used:
 *   3) Once the decompressed frame is available the GStreamer pipeline calls
 *      new_sample() in the GStreamer thread.
 *   4) new_sample() then increments the pending_samples count and calls
 *      schedule_frame().
 *   5) schedule_frame() is called whenever a new frame might need to be
 *      displayed. If that is the case and pending_samples is non-zero it calls
 *      fetch_pending_sample().
 *   6) fetch_pending_sample() grabs GStreamer's latest sample and then calls
 *      get_decoded_frame() which compares the GStreamer's buffer timestamp to
 *      gstframe->encoded_frame->mm_time to match it with a decoding_queue
 *      entry.
 *   7) fetch_pending_sample() then attaches the sample to the SpiceGstFrame,
 *      and sets display_frame.
 *   8) schedule_frame() then uses display_frame->encoded_frame->mm_time to
 *      arrange for display_frame() to be called, in the main thread, at the
 *      right time.
 *   9) display_frame() uses SpiceGstFrame from display_frame and calls
 *      stream_display_frame().
 *  10) display_frame() then calls free_gst_frame() to free the SpiceGstFrame
 *      and unref the encoded_buffer which allows GStreamer to call
 *      spice_frame_free() and free its encoded_frame.
 */
static gboolean spice_gst_decoder_queue_frame(VideoDecoder *video_decoder,
                                              SpiceFrame *frame, int margin)
{
    SpiceGstDecoder *decoder = (SpiceGstDecoder*)video_decoder;

    if (frame->size == 0) {
        SPICE_DEBUG("got an empty frame buffer!");
        spice_frame_free(frame);
        return TRUE;
    }

    if (spice_mmtime_diff(frame->mm_time, decoder->last_mm_time) < 0) {
        SPICE_DEBUG("new-frame-time < last-frame-time (%u < %u):"
                    " resetting stream",
                    frame->mm_time, decoder->last_mm_time);
        /* Let GStreamer deal with the frame anyway */
    }
    decoder->last_mm_time = frame->mm_time;

    if (margin < 0 &&
        decoder->base.codec_type == SPICE_VIDEO_CODEC_TYPE_MJPEG) {
        /* Dropping MJPEG frames has no impact on those that follow and
         * saves CPU so do it.
         */
        SPICE_DEBUG("dropping a late MJPEG frame");
        spice_frame_free(frame);
        return TRUE;
    }

    if (decoder->pipeline == NULL) {
        /* An error occurred, causing the GStreamer pipeline to be freed */
        spice_warning("An error occurred, stopping the video stream");
        spice_frame_free(frame);
        return FALSE;
    }

    if (decoder->appsrc == NULL) {
        spice_warning("Error: Playbin has not yet initialized the Appsrc element");
        stream_dropped_frame_on_playback(decoder->base.stream);
        spice_frame_free(frame);
        return TRUE;
    }

    /* frame ownership is moved to the buffer */
    GstBuffer *buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_PHYSICALLY_CONTIGUOUS,
                                                    frame->data, frame->size, 0, frame->size,
                                                    frame, (GDestroyNotify) spice_frame_free);

    GstClockTime pts = gst_clock_get_time(decoder->clock) - gst_element_get_base_time(decoder->pipeline) + ((uint64_t)MAX(0, margin)) * 1000 * 1000;
    GST_BUFFER_DURATION(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_PTS(buffer) = pts;
#if GST_CHECK_VERSION(1,14,0)
    gst_buffer_add_reference_timestamp_meta(buffer, gst_static_caps_get(&stream_reference),
                                            pts, GST_CLOCK_TIME_NONE);
#endif

    SpiceGstFrame *gst_frame = create_gst_frame(buffer, frame);
    g_mutex_lock(&decoder->queues_mutex);
    gst_frame->queue_len = decoder->decoding_queue->length;
    g_queue_push_tail(decoder->decoding_queue, gst_frame);
    g_mutex_unlock(&decoder->queues_mutex);

    if (gst_app_src_push_buffer(decoder->appsrc, buffer) != GST_FLOW_OK) {
        SPICE_DEBUG("GStreamer error: unable to push frame");
        stream_dropped_frame_on_playback(decoder->base.stream);
    }
    return TRUE;
}

static gboolean gstvideo_init(void)
{
    static int success = 0;
    if (!success) {
        GError *err = NULL;
        if (gst_init_check(NULL, NULL, &err)) {
            success = 1;
        } else {
            spice_warning("Disabling GStreamer video support: %s", err->message);
            g_clear_error(&err);
            success = -1;
        }
    }
    return success > 0;
}

G_GNUC_INTERNAL
VideoDecoder* create_gstreamer_decoder(int codec_type, display_stream *stream)
{
    SpiceGstDecoder *decoder = NULL;

    g_return_val_if_fail(VALID_VIDEO_CODEC_TYPE(codec_type), NULL);

    if (gstvideo_init()) {
        decoder = g_new0(SpiceGstDecoder, 1);
        decoder->base.destroy = spice_gst_decoder_destroy;
        decoder->base.reschedule = spice_gst_decoder_reschedule;
        decoder->base.queue_frame = spice_gst_decoder_queue_frame;
        decoder->base.codec_type = codec_type;
        decoder->base.stream = stream;
        decoder->last_mm_time = stream_get_time(stream);
        g_mutex_init(&decoder->queues_mutex);
        decoder->decoding_queue = g_queue_new();

        if (!create_pipeline(decoder)) {
            decoder->base.destroy((VideoDecoder*)decoder);
            decoder = NULL;
        }
    }

    return (VideoDecoder*)decoder;
}

static void gstvideo_debug_available_decoders(int codec_type,
                                              GList *all_decoders,
                                              GList *codec_decoders)
{
    GList *l;
    GString *msg = g_string_new(NULL);
    /* Print list of available decoders to make debugging easier */
    g_string_printf(msg, "From %3u video decoder elements, %2u can handle caps %12s: ",
                    g_list_length(all_decoders), g_list_length(codec_decoders),
                    gst_opts[codec_type].dec_caps);

    for (l = codec_decoders; l != NULL; l = l->next) {
        GstPluginFeature *pfeat = GST_PLUGIN_FEATURE(l->data);
        g_string_append_printf(msg, "%s, ", gst_plugin_feature_get_name(pfeat));
    }

    /* Drop trailing ", " */
    g_string_truncate(msg, msg->len - 2);
    spice_debug("%s", msg->str);
    g_string_free(msg, TRUE);
}

G_GNUC_INTERNAL
gboolean gstvideo_has_codec(int codec_type)
{
    GList *all_decoders, *codec_decoders;
    GstCaps *caps;
    GstElementFactoryListType type;

    g_return_val_if_fail(gstvideo_init(), FALSE);
    g_return_val_if_fail(VALID_VIDEO_CODEC_TYPE(codec_type), FALSE);

    type = GST_ELEMENT_FACTORY_TYPE_DECODER |
           GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO |
           GST_ELEMENT_FACTORY_TYPE_MEDIA_IMAGE;
    all_decoders = gst_element_factory_list_get_elements(type, GST_RANK_NONE);
    if (all_decoders == NULL) {
        spice_debug("No video decoders from GStreamer for %s were found",
                    gst_opts[codec_type].name);
        return FALSE;
    }

    caps = gst_caps_from_string(gst_opts[codec_type].dec_caps);
    codec_decoders = gst_element_factory_list_filter(all_decoders, caps, GST_PAD_SINK, FALSE);
    gst_caps_unref(caps);

    if (codec_decoders == NULL) {
        spice_debug("From %u decoders, none can handle '%s'",
                    g_list_length(all_decoders), gst_opts[codec_type].dec_caps);
        gst_plugin_feature_list_free(all_decoders);
        return FALSE;
    }

    if (spice_util_get_debug())
        gstvideo_debug_available_decoders(codec_type, all_decoders, codec_decoders);

    gst_plugin_feature_list_free(codec_decoders);
    gst_plugin_feature_list_free(all_decoders);
    return TRUE;
}
