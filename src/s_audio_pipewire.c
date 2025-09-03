/* Copyright (c) 1997-2003 Guenter Geiger, Miller Puckette, Larry Troxler,
* Winfried Ritsch, Karl MacMillan, and others.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* this file inputs and outputs audio using the pipewire API available on linux. */

/* support for pipewire 0.3 by Charles K. Neimog <charlesneimog@outlook.com> */

#include <alsa/asoundlib.h>

#include "m_pd.h"
#include "s_stuff.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/param.h>

#include "m_private_utils.h"

/* ------------------------ Simple interleaved-float SPSC ringbuffer ------------------------ */
typedef struct {
    float *buf;           /* interleaved samples */
    size_t cap;           /* total samples (frames * channels) */
    _Atomic size_t r;     /* read position in frames */
    _Atomic size_t w;     /* write position in frames */
} rb_t;

static void rb_init(rb_t *rb, size_t frames_cap, int channels)
{
    rb->cap = frames_cap * (size_t)channels;
    rb->buf = (float *)calloc(rb->cap, sizeof(float));
    atomic_store(&rb->r, 0);
    atomic_store(&rb->w, 0);
}
static void rb_free(rb_t *rb)
{
    free(rb->buf);
    rb->buf = NULL;
    rb->cap = 0;
}
static size_t rb_read(rb_t *rb, float *dst, size_t frames, int channels)
{
    if (!rb->buf || rb->cap == 0) return 0;
    size_t cap_frames = rb->cap / (size_t)channels;
    size_t r = atomic_load_explicit(&rb->r, memory_order_relaxed);
    size_t w = atomic_load_explicit(&rb->w, memory_order_acquire);
    size_t avail = (w + cap_frames - r) % cap_frames;
    size_t todo = frames < avail ? frames : avail;
    if (todo == 0) return 0;

    size_t idx_frames = r % cap_frames;
    size_t end_frames = cap_frames - idx_frames;
    size_t c1 = todo < end_frames ? todo : end_frames;

    memcpy(dst, rb->buf + (idx_frames * channels), c1 * (size_t)channels * sizeof(float));
    if (todo > c1) {
        memcpy(dst + c1 * channels, rb->buf, (todo - c1) * (size_t)channels * sizeof(float));
    }
    atomic_store_explicit(&rb->r, (r + todo) % cap_frames, memory_order_release);
    return todo;
}
static size_t rb_write(rb_t *rb, const float *src, size_t frames, int channels)
{
    if (!rb->buf || rb->cap == 0) return 0;
    size_t cap_frames = rb->cap / (size_t)channels;
    size_t r = atomic_load_explicit(&rb->r, memory_order_acquire);
    size_t w = atomic_load_explicit(&rb->w, memory_order_relaxed);
    size_t freef = (r + cap_frames - w) % cap_frames;
    if (freef == 0) freef = cap_frames;
    if (freef == cap_frames) freef--; /* keep one slot free */
    size_t todo = frames < freef ? frames : freef;
    if (todo == 0) return 0;

    size_t idx_frames = w % cap_frames;
    size_t end_frames = cap_frames - idx_frames;
    size_t c1 = todo < end_frames ? todo : end_frames;

    memcpy(rb->buf + (idx_frames * channels), src, c1 * (size_t)channels * sizeof(float));
    if (todo > c1) {
        memcpy(rb->buf, src + c1 * channels, (todo - c1) * (size_t)channels * sizeof(float));
    }
    atomic_store_explicit(&rb->w, (w + todo) % cap_frames, memory_order_release);
    return todo;
}

/* ------------------------------- Backend state ------------------------------- */
#define MAX_BUFFERS 4

typedef struct _pw_state {
    int samplerate;
    int blocksize;
    int in_n_devices;
    int out_n_devices;

    unsigned in_channels;
    unsigned out_channels;

    struct pw_thread_loop *tloop;
    struct pw_stream *stream_in;
    struct pw_stream *stream_out;

    struct spa_hook stream_in_listener;
    struct spa_hook stream_out_listener;

    rb_t rb_in;                  /* PW -> Pd ring (capture)  [interleaved float] */
    rb_t rb_out;                 /* Pd -> PW ring (playback) [interleaved float] */

    float *tmp_in;               /* interleaved temp for deinterleave to Pd */
    size_t tmp_in_cap;           /* frames capacity for tmp_in */
    float *tmp_out;              /* interleaved temp for interleave from Pd */
    size_t tmp_out_cap;          /* frames capacity for tmp_out */

    unsigned desired_block;      /* as requested by Pd (blocksize) */
    int in_printed_match_once;
    int in_printed_mismatch_once;
    int out_printed_match_once;
    int out_printed_mismatch_once;

    int running;
} t_pw_state;

static t_pw_state pw_state;
static int pw_inited = 0;


/*
 * Pipewire input callbacks, for now we just use pw_in_process
*/
static void pw_in_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    // This is called when, for example, in apps like pwvucontrol we change the
    // input device.
    logpost(0, 3, "Program in changed");
    (void)data; (void)id; (void)param;
}


static void pw_in_process(void *data)
{
    t_pw_state *st = (t_pw_state *)data;
    if (!st || !st->stream_in) return;

    struct pw_buffer *b = pw_stream_dequeue_buffer(st->stream_in);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    if (!buf || buf->n_datas <= 0 || buf->datas[0].data == NULL) {
        pw_stream_queue_buffer(st->stream_in, b);
        return;
    }

    void *base = buf->datas[0].data;
    size_t maxsize = (size_t)buf->datas[0].maxsize;
    size_t chunk_offset = 0;
    size_t chunk_size = 0;
    if (buf->datas[0].chunk) {
        chunk_offset = (size_t)buf->datas[0].chunk->offset;
        chunk_size   = (size_t)buf->datas[0].chunk->size;
    }
    if (chunk_offset > maxsize) {
        pw_stream_queue_buffer(st->stream_in, b);
        return;
    }
    size_t nbytes = chunk_size ? chunk_size : (maxsize - chunk_offset);
    if (nbytes == 0) {
        pw_stream_queue_buffer(st->stream_in, b);
        return;
    }

    const float *src = (const float *)((const char *)base + chunk_offset);
    uint32_t stride = st->in_channels * sizeof(float);
    uint32_t nframes = stride ? (uint32_t)(nbytes / stride) : 0;

    if (nframes > 0 && st->in_channels > 0) {
        rb_write(&st->rb_in, src, nframes, (int)st->in_channels);
    }

    pw_stream_queue_buffer(st->stream_in, b);
}

static const struct pw_stream_events stream_in_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = pw_in_param_changed,
    .process       = pw_in_process,
};

/*
 * Pipewire output callbacks, for now we just use pw_in_process
*/
static void pw_out_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    // This is called when, for example, in apps like pwvucontrol we change the
    // output device.
    logpost(0, 3, "Program out changed");
    (void)data; (void)id; (void)param;
}


static void pw_out_process(void *data)
{
    t_pw_state *st = (t_pw_state *)data;
    if (!st || !st->stream_out) return;

    struct pw_buffer *b = pw_stream_dequeue_buffer(st->stream_out);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    if (!buf || buf->n_datas <= 0 || buf->datas[0].data == NULL) {
        pw_stream_queue_buffer(st->stream_out, b);
        return;
    }

    float *dst = (float *)buf->datas[0].data;
    uint32_t max_bytes = buf->datas[0].maxsize;
    uint32_t stride = st->out_channels * sizeof(float);
    uint32_t nframes = stride ? (max_bytes / stride) : 0;

    /* One-time notice on quantum */
    // if (nframes > 0) {
    //     if (nframes == st->desired_block) {
    //         if (!st->out_printed_match_once) {
    //             post("[pipewire] playback server honoured requested blocksize: %u", nframes);
    //             st->out_printed_match_once = 1;
    //         }
    //     } else if (!st->out_printed_mismatch_once) {
    //         post("[pipewire] playback requested %u but server provides %u frames",
    //              st->desired_block, nframes);
    //         st->out_printed_mismatch_once = 1;
    //     }
    // }

    if (nframes == 0 || st->out_channels == 0) {
        pw_stream_queue_buffer(st->stream_out, b);
        return;
    }

    size_t got = rb_read(&st->rb_out, dst, nframes, (int)st->out_channels);
    if (got < nframes) {
        /* underrun: zero remainder */
        memset(dst + got * st->out_channels, 0,
               (nframes - (uint32_t)got) * st->out_channels * sizeof(float));
    }

    if (buf->datas[0].chunk) {
        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = stride;
        buf->datas[0].chunk->size   = nframes * stride;
    }
    pw_stream_queue_buffer(st->stream_out, b);
}

static const struct pw_stream_events stream_out_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = pw_out_param_changed,
    .process       = pw_out_process,
};


/* -------------------------- Helpers: make/conn streams -------------------------- */
static int pw_make_capture_stream(t_pw_state *st)
{
    /* properties to request blocksize/rate */
    unsigned sr = (unsigned)(st->samplerate > 0 ? st->samplerate : 48000);

    char rate_prop[32];
    snprintf(rate_prop, sizeof(rate_prop), "%u/1", sr);
    char latency_prop[32];
    snprintf(latency_prop, sizeof(latency_prop), "%u/%u", (unsigned)st->desired_block, sr);

    struct pw_properties *props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Production",
                          NULL);
    pw_properties_set(props, PW_KEY_NODE_RATE,    rate_prop);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency_prop);

    st->stream_in = pw_stream_new_simple(pw_thread_loop_get_loop(st->tloop),
                                         "info.puredata.Pd", props,
                                         &stream_in_events, st);
    if (!st->stream_in) return -1;

    struct spa_audio_info_raw info;
    spa_zero(info);
    info.format   = SPA_AUDIO_FORMAT_F32;
    info.channels = st->in_channels > 0 ? (uint32_t)st->in_channels : 0;
    info.rate     = sr;

    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[3];
    uint32_t n_params = 0;

    /* 1) format */
    params[n_params++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
        SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_AUDIO_format,   SPA_POD_Id(info.format),
        SPA_FORMAT_AUDIO_channels, SPA_POD_Int((int)info.channels),
        SPA_FORMAT_AUDIO_rate,     SPA_POD_Int((int)info.rate));

    /* 2) latency param: request exact blocksize on INPUT */
    struct spa_latency_info lat;
    spa_zero(lat);
    lat.direction   = SPA_DIRECTION_INPUT;
    lat.min_quantum = (float)st->desired_block;
    lat.max_quantum = (float)st->desired_block;

    uint8_t lbuf[256];
    struct spa_pod_builder lb = SPA_POD_BUILDER_INIT(lbuf, sizeof(lbuf));
    params[n_params++] = spa_latency_build(&lb, SPA_PARAM_Latency, &lat);

    /* 3) buffers sized for one block */
    int frames     = st->blocksize;
    int stride     = (int)st->in_channels * (int)sizeof(float);
    int size_bytes = frames * stride;

    params[n_params++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(MAX_BUFFERS),
        SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,    SPA_POD_Int(size_bytes),
        SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(stride));

    uint32_t flags = PW_STREAM_FLAG_AUTOCONNECT |
                     PW_STREAM_FLAG_MAP_BUFFERS |
                     PW_STREAM_FLAG_RT_PROCESS;

    if (pw_stream_connect(st->stream_in, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, n_params) < 0) {
        return -1;
    }
    return 0;
}

static int pw_make_playback_stream(t_pw_state *st)
{
    unsigned sr = (unsigned)(st->samplerate > 0 ? st->samplerate : 48000);

    char rate_prop[32];
    snprintf(rate_prop, sizeof(rate_prop), "%u/1", sr);
    char latency_prop[32];
    snprintf(latency_prop, sizeof(latency_prop), "%u/%u", (unsigned)st->desired_block, sr);

    struct pw_properties *props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Production",
                          NULL);
    pw_properties_set(props, PW_KEY_NODE_RATE,    rate_prop);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency_prop);

    st->stream_out = pw_stream_new_simple(pw_thread_loop_get_loop(st->tloop),
                                          "info.puredata.Pd", props,
                                          &stream_out_events, st);
    if (!st->stream_out) return -1;

    struct spa_audio_info_raw info;
    spa_zero(info);
    info.format   = SPA_AUDIO_FORMAT_F32;
    info.channels = st->out_channels > 0 ? (uint32_t)st->out_channels : 0;
    info.rate     = sr;

    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[3];
    uint32_t n_params = 0;

    /* 1) format */
    params[n_params++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
        SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_AUDIO_format,   SPA_POD_Id(info.format),
        SPA_FORMAT_AUDIO_channels, SPA_POD_Int((int)info.channels),
        SPA_FORMAT_AUDIO_rate,     SPA_POD_Int((int)info.rate));

    /* 2) latency param: request exact blocksize on OUTPUT */
    struct spa_latency_info lat;
    spa_zero(lat);
    lat.direction   = SPA_DIRECTION_OUTPUT;
    lat.min_quantum = (float)st->desired_block;
    lat.max_quantum = (float)st->desired_block;

    uint8_t lbuf[256];
    struct spa_pod_builder lb = SPA_POD_BUILDER_INIT(lbuf, sizeof(lbuf));
    params[n_params++] = spa_latency_build(&lb, SPA_PARAM_Latency, &lat);

    /* 3) buffers sized for one block */
    int frames     = st->blocksize;
    int stride     = (int)st->out_channels * (int)sizeof(float);
    int size_bytes = frames * stride;

    params[n_params++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(MAX_BUFFERS),
        SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,    SPA_POD_Int(size_bytes),
        SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(stride));

    uint32_t flags = PW_STREAM_FLAG_AUTOCONNECT |
                     PW_STREAM_FLAG_MAP_BUFFERS |
                     PW_STREAM_FLAG_RT_PROCESS;

    if (pw_stream_connect(st->stream_out, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, n_params) < 0) {
        return -1;
    }
    return 0;
}


/* ------------------------------ Backend API ------------------------------ */
/*
 * Open a single PipeWire client (thread loop) with up to one capture and one
 * playback stream. Channels are totals across requested devices. We request
 * a quantum equal to Pd's blocksize and verify on first callbacks.
 */
int pipewire_open_audio(int naudioindev, int *audioindev, int nchindev,
    int *chindev, int naudiooutdev, int *audiooutdev, int nchoutdev,
    int *choutdev, int rate, int blocksize)
{
    (void)audioindev; (void)audiooutdev; (void)nchindev; (void)nchoutdev;

    /* Sum channels from device channel arrays (if provided) */
    unsigned in_ch = 0, out_ch = 0;
    if (chindev && naudioindev > 0)
        for (int i = 0; i < naudioindev; i++)
            in_ch += (unsigned)((chindev[i] > 0) ? chindev[i] : 0);
    if (choutdev && naudiooutdev > 0)
        for (int i = 0; i < naudiooutdev; i++)
            out_ch += (unsigned)((choutdev[i] > 0) ? choutdev[i] : 0);

    /* Initialize PipeWire once per process */
    if (!pw_inited) {
        pw_init(NULL, NULL);
        pw_inited = 1;
    }

    memset(&pw_state, 0, sizeof(pw_state));
    pw_state.samplerate    = rate;
    pw_state.blocksize     = blocksize;
    pw_state.desired_block = (unsigned)blocksize;
    pw_state.in_n_devices  = naudioindev;
    pw_state.out_n_devices = naudiooutdev;
    pw_state.in_channels   = in_ch;
    pw_state.out_channels  = out_ch;

    /* Create one thread loop (single client) */
    pw_state.tloop = pw_thread_loop_new("pd-pipewire", NULL);
    if (!pw_state.tloop) {
        logpost(0, 1, "[pipewire] failed to create thread loop");
        goto fail;
    }
    if (pw_thread_loop_start(pw_state.tloop) != 0) {
        logpost(0, 1, "[pipewire] failed to start thread loop");
        goto fail;
    }

    /* Hold loop while creating/connecting streams */
    pw_thread_loop_lock(pw_state.tloop);

    /* Create rings (16 Pd blocks of capacity by default) */
    if (pw_state.in_channels > 0)
        rb_init(&pw_state.rb_in,  (size_t)pw_state.blocksize * 16, (int)pw_state.in_channels);
    if (pw_state.out_channels > 0)
        rb_init(&pw_state.rb_out, (size_t)pw_state.blocksize * 16, (int)pw_state.out_channels);

    /* Build streams as requested; either/both may be zero channels */
    if (pw_state.in_channels > 0) {
        if (pw_make_capture_stream(&pw_state) < 0) {
            logpost(0, 1, "[pipewire] failed to create capture stream");
            pw_thread_loop_unlock(pw_state.tloop);
            goto fail;
        }
    }
    if (pw_state.out_channels > 0) {
        if (pw_make_playback_stream(&pw_state) < 0) {
            logpost(0, 1, "[pipewire] failed to create playback stream");
            pw_thread_loop_unlock(pw_state.tloop);
            goto fail;
        }
    }
    pw_thread_loop_unlock(pw_state.tloop);

    pw_state.running = 1;
    return 0;

fail:
    /* Clean up partial initialization */
    if (pw_state.tloop) {
        pw_thread_loop_unlock(pw_state.tloop); /* in case still locked */
        pw_thread_loop_stop(pw_state.tloop);
        pw_thread_loop_destroy(pw_state.tloop);
        pw_state.tloop = NULL;
    }
    if (pw_state.stream_in) {
        pw_stream_destroy(pw_state.stream_in);
        pw_state.stream_in = NULL;
    }
    if (pw_state.stream_out) {
        pw_stream_destroy(pw_state.stream_out);
        pw_state.stream_out = NULL;
    }
    rb_free(&pw_state.rb_in);
    rb_free(&pw_state.rb_out);
    free(pw_state.tmp_in);  pw_state.tmp_in = NULL;  pw_state.tmp_in_cap = 0;
    free(pw_state.tmp_out); pw_state.tmp_out = NULL; pw_state.tmp_out_cap = 0;

    pw_state.running = 0;
    return 1;
}

void pipewire_close_audio(void)
{
    if (!pw_state.tloop && !pw_state.stream_in && !pw_state.stream_out)
        return;

    // post("[pipewire] close");

    if (pw_state.tloop) {
        pw_thread_loop_lock(pw_state.tloop);
        if (pw_state.stream_in) {
            pw_stream_disconnect(pw_state.stream_in);
            pw_stream_destroy(pw_state.stream_in);
            pw_state.stream_in = NULL;
        }
        if (pw_state.stream_out) {
            pw_stream_disconnect(pw_state.stream_out);
            pw_stream_destroy(pw_state.stream_out);
            pw_state.stream_out = NULL;
        }
        pw_thread_loop_unlock(pw_state.tloop);

        pw_thread_loop_stop(pw_state.tloop);
        pw_thread_loop_destroy(pw_state.tloop);
        pw_state.tloop = NULL;
    }

    rb_free(&pw_state.rb_in);
    rb_free(&pw_state.rb_out);
    free(pw_state.tmp_in);  pw_state.tmp_in = NULL;  pw_state.tmp_in_cap = 0;
    free(pw_state.tmp_out); pw_state.tmp_out = NULL; pw_state.tmp_out_cap = 0;

    pw_state.in_printed_match_once = pw_state.in_printed_mismatch_once = 0;
    pw_state.out_printed_match_once = pw_state.out_printed_mismatch_once = 0;

    pw_state.running = 0;

    /* Keep pw_init() globally initialized; Pd may re-open later in same process. */
}

/* Called by Pd each DSP tick to move one block between Pd and PipeWire.
   - Interleave Pd output (sys_soundout) and push to playback ring
   - Pull from capture ring and deinterleave into sys_soundin
   Returns nonzero when audio is active; zero if backend is stopped. */
int pipewire_send_dacs(void)
{
    t_sample *muxbuffer;
    t_sample *fp, *fp2, *jp;
    int j, ch;
    int retval = SENDDACS_YES;

    /* backend inactive or no I/O channels -> nothing to do */
    if (!pw_state.running || (!pw_state.in_channels && !pw_state.out_channels))
        return SENDDACS_NO;

    /* We use the Pd scheduler blocksize as the quantum to exchange */
    const size_t frames = (size_t)DEFDACBLKSIZE;

    /* Compute ring availability for one Pd block (in frames) */
    /* Capture ring (PW -> Pd) */
    size_t in_needed = (pw_state.in_channels ? frames : 0);
    size_t in_avail = 0;
    if (pw_state.in_channels) {
        size_t cap_frames = pw_state.rb_in.cap / (size_t)pw_state.in_channels;
        size_t r = atomic_load_explicit(&pw_state.rb_in.r, memory_order_relaxed);
        size_t w = atomic_load_explicit(&pw_state.rb_in.w, memory_order_acquire);
        in_avail = (w + cap_frames - r) % cap_frames;
    }

    /* Playback ring space (Pd -> PW) */
    size_t out_needed = (pw_state.out_channels ? frames : 0);
    size_t out_free = 0;
    if (pw_state.out_channels) {
        size_t cap_frames = pw_state.rb_out.cap / (size_t)pw_state.out_channels;
        size_t r = atomic_load_explicit(&pw_state.rb_out.r, memory_order_acquire);
        size_t w = atomic_load_explicit(&pw_state.rb_out.w, memory_order_relaxed);
        out_free = (r + cap_frames - w) % cap_frames;
        if (out_free == 0) out_free = cap_frames;
        if (out_free == cap_frames) out_free--; /* keep one slot free, like rb_write */
    }

    /* If we cannot move a full Pd block, give scheduler a chance to do other work */
#ifdef THREADSIGNAL
    while ((pw_state.in_channels && in_avail < in_needed) ||
           (pw_state.out_channels && out_free < out_needed))
    {
        if (sched_idletask())
            continue; /* do other tasks first, then re-check */

        /* No explicit semaphore used here; bail out like JACK's early return path */
        return SENDDACS_NO;
    }
#else
    if ((pw_state.in_channels && in_avail < in_needed) ||
        (pw_state.out_channels && out_free < out_needed))
        return SENDDACS_NO;
#endif

    /* Interleaving scratch buffer size in samples */
    const size_t muxbufsize =
        DEFDACBLKSIZE * (pw_state.in_channels > pw_state.out_channels ?
                         pw_state.in_channels : pw_state.out_channels);

    // #define MAX_ALLOCA_SAMPLES = 16 * 1024
    int MAX_ALLOCA_SAMPLES = 16 * 1024;
    ALLOCA(t_sample, muxbuffer, muxbufsize, MAX_ALLOCA_SAMPLES);

    /* De-mux capture into STUFF->st_soundin (channel-major, blocklength DEFDACBLKSIZE) */
    if (pw_state.in_channels)
    {
        /* Read one Pd block as interleaved frames from PW->Pd ring */
        /* rb_read expects float buffer; Pd's t_sample is float in typical builds.
           If t_sample were double, a separate temp and conversion would be needed. */
        rb_read(&pw_state.rb_in, (float *)muxbuffer, frames, (int)pw_state.in_channels);

        for (fp = muxbuffer, ch = 0; ch < (int)pw_state.in_channels; ch++, fp++)
        {
            jp = STUFF->st_soundin + ch * DEFDACBLKSIZE;
            for (j = 0, fp2 = fp; j < DEFDACBLKSIZE;
                 j++, fp2 += pw_state.in_channels)
            {
                jp[j] = *fp2;
            }
        }
    }

    /* Mux playback from STUFF->st_soundout and push to Pd->PW ring */
    if (pw_state.out_channels)
    {
        for (fp = muxbuffer, ch = 0; ch < (int)pw_state.out_channels; ch++, fp++)
        {
            jp = STUFF->st_soundout + ch * DEFDACBLKSIZE;
            for (j = 0, fp2 = fp; j < DEFDACBLKSIZE;
                 j++, fp2 += pw_state.out_channels)
            {
                *fp2 = jp[j];
            }
        }
        rb_write(&pw_state.rb_out, (const float *)muxbuffer, frames, (int)pw_state.out_channels);
    }

    /* Clear Pd's output buffer for the next block, like the JACK backend does */
    if (pw_state.out_channels && STUFF->st_soundout)
        memset(STUFF->st_soundout, 0,
               DEFDACBLKSIZE * (size_t)pw_state.out_channels * sizeof(t_sample));

    FREEA(t_sample, muxbuffer, muxbufsize, MAX_ALLOCA_SAMPLES);
    return retval;
}

void pipewire_listdevs(void)
{
    post("device listing not implemented for pipewire yet\n");
}

void pipewire_getdevs(char *indevlist, int *nindevs,
    char *outdevlist, int *noutdevs, int *canmulti,
        int maxndev, int devdescsize)
{
    int i, ndev;
    *canmulti = 0;  /* supports multiple devices */
    ndev = 1;
    for (i = 0; i < ndev; i++)
    {
        sprintf(indevlist + i * devdescsize, "Pipewire");
        sprintf(outdevlist + i * devdescsize, "Pipewire");
    }
    *nindevs = *noutdevs = ndev;
}
