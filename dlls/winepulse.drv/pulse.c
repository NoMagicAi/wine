/*
 * Copyright 2011-2012 Maarten Lankhorst
 * Copyright 2010-2011 Maarten Lankhorst for CodeWeavers
 * Copyright 2011 Andrew Eikum for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include <stdarg.h>
#include <pthread.h>
#include <math.h>
#include <poll.h>

#include <pulse/pulseaudio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "mmdeviceapi.h"
#include "initguid.h"
#include "audioclient.h"

#include "unixlib.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(pulse);

static pa_context *pulse_ctx;
static pa_mainloop *pulse_ml;

/* Mixer format + period times */
static WAVEFORMATEXTENSIBLE pulse_fmt[2];
static REFERENCE_TIME pulse_min_period[2], pulse_def_period[2];

static UINT g_phys_speakers_mask = 0;

static const REFERENCE_TIME MinimumPeriod = 30000;
static const REFERENCE_TIME DefaultPeriod = 100000;

static pthread_mutex_t pulse_mutex;
static pthread_cond_t pulse_cond = PTHREAD_COND_INITIALIZER;

UINT8 mult_alaw_sample(UINT8, float);
UINT8 mult_ulaw_sample(UINT8, float);

static void WINAPI pulse_lock(void)
{
    pthread_mutex_lock(&pulse_mutex);
}

static void WINAPI pulse_unlock(void)
{
    pthread_mutex_unlock(&pulse_mutex);
}

static int WINAPI pulse_cond_wait(void)
{
    return pthread_cond_wait(&pulse_cond, &pulse_mutex);
}

static void pulse_broadcast(void)
{
    pthread_cond_broadcast(&pulse_cond);
}

static void dump_attr(const pa_buffer_attr *attr)
{
    TRACE("maxlength: %u\n", attr->maxlength);
    TRACE("minreq: %u\n", attr->minreq);
    TRACE("fragsize: %u\n", attr->fragsize);
    TRACE("tlength: %u\n", attr->tlength);
    TRACE("prebuf: %u\n", attr->prebuf);
}

/* copied from kernelbase */
static int muldiv(int a, int b, int c)
{
    LONGLONG ret;

    if (!c) return -1;

    /* We want to deal with a positive divisor to simplify the logic. */
    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    /* If the result is positive, we "add" to round. else, we subtract to round. */
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

/* Following pulseaudio design here, mainloop has the lock taken whenever
 * it is handling something for pulse, and the lock is required whenever
 * doing any pa_* call that can affect the state in any way
 *
 * pa_cond_wait is used when waiting on results, because the mainloop needs
 * the same lock taken to affect the state
 *
 * This is basically the same as the pa_threaded_mainloop implementation,
 * but that cannot be used because it uses pthread_create directly
 *
 * pa_threaded_mainloop_(un)lock -> pthread_mutex_(un)lock
 * pa_threaded_mainloop_signal -> pthread_cond_broadcast
 * pa_threaded_mainloop_wait -> pthread_cond_wait
 */
static int pulse_poll_func(struct pollfd *ufds, unsigned long nfds, int timeout, void *userdata)
{
    int r;
    pulse_unlock();
    r = poll(ufds, nfds, timeout);
    pulse_lock();
    return r;
}

static void WINAPI pulse_main_loop(void)
{
    int ret;
    pulse_ml = pa_mainloop_new();
    pa_mainloop_set_poll_func(pulse_ml, pulse_poll_func, NULL);
    pulse_lock();
    pulse_broadcast();
    pa_mainloop_run(pulse_ml, &ret);
    pulse_unlock();
    pa_mainloop_free(pulse_ml);
}

static void pulse_contextcallback(pa_context *c, void *userdata)
{
    switch (pa_context_get_state(c)) {
        default:
            FIXME("Unhandled state: %i\n", pa_context_get_state(c));
            return;

        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
        case PA_CONTEXT_TERMINATED:
            TRACE("State change to %i\n", pa_context_get_state(c));
            return;

        case PA_CONTEXT_READY:
            TRACE("Ready\n");
            break;

        case PA_CONTEXT_FAILED:
            WARN("Context failed: %s\n", pa_strerror(pa_context_errno(c)));
            break;
    }
    pulse_broadcast();
}

static void pulse_stream_state(pa_stream *s, void *user)
{
    pa_stream_state_t state = pa_stream_get_state(s);
    TRACE("Stream state changed to %i\n", state);
    pulse_broadcast();
}

static void pulse_attr_update(pa_stream *s, void *user) {
    const pa_buffer_attr *attr = pa_stream_get_buffer_attr(s);
    TRACE("New attributes or device moved:\n");
    dump_attr(attr);
}

static void pulse_underflow_callback(pa_stream *s, void *userdata)
{
    struct pulse_stream *stream = userdata;
    WARN("%p: Underflow\n", userdata);
    stream->just_underran = TRUE;
    /* re-sync */
    stream->pa_offs_bytes = stream->lcl_offs_bytes;
    stream->pa_held_bytes = stream->held_bytes;
}

static void pulse_started_callback(pa_stream *s, void *userdata)
{
    TRACE("%p: (Re)started playing\n", userdata);
}

static void pulse_op_cb(pa_stream *s, int success, void *user)
{
    TRACE("Success: %i\n", success);
    *(int*)user = success;
    pulse_broadcast();
}

static void silence_buffer(pa_sample_format_t format, BYTE *buffer, UINT32 bytes)
{
    memset(buffer, format == PA_SAMPLE_U8 ? 0x80 : 0, bytes);
}

static BOOL pulse_stream_valid(struct pulse_stream *stream)
{
    return pa_stream_get_state(stream->stream) == PA_STREAM_READY;
}

static HRESULT pulse_connect(const char *name)
{
    if (pulse_ctx && PA_CONTEXT_IS_GOOD(pa_context_get_state(pulse_ctx)))
        return S_OK;
    if (pulse_ctx)
        pa_context_unref(pulse_ctx);

    pulse_ctx = pa_context_new(pa_mainloop_get_api(pulse_ml), name);
    if (!pulse_ctx) {
        ERR("Failed to create context\n");
        return E_FAIL;
    }

    pa_context_set_state_callback(pulse_ctx, pulse_contextcallback, NULL);

    TRACE("libpulse protocol version: %u. API Version %u\n", pa_context_get_protocol_version(pulse_ctx), PA_API_VERSION);
    if (pa_context_connect(pulse_ctx, NULL, 0, NULL) < 0)
        goto fail;

    /* Wait for connection */
    while (pulse_cond_wait()) {
        pa_context_state_t state = pa_context_get_state(pulse_ctx);

        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
            goto fail;

        if (state == PA_CONTEXT_READY)
            break;
    }

    TRACE("Connected to server %s with protocol version: %i.\n",
        pa_context_get_server(pulse_ctx),
        pa_context_get_server_protocol_version(pulse_ctx));
    return S_OK;

fail:
    pa_context_unref(pulse_ctx);
    pulse_ctx = NULL;
    return E_FAIL;
}

static DWORD pulse_channel_map_to_channel_mask(const pa_channel_map *map)
{
    int i;
    DWORD mask = 0;

    for (i = 0; i < map->channels; ++i) {
        switch (map->map[i]) {
            default: FIXME("Unhandled channel %s\n", pa_channel_position_to_string(map->map[i])); break;
            case PA_CHANNEL_POSITION_FRONT_LEFT: mask |= SPEAKER_FRONT_LEFT; break;
            case PA_CHANNEL_POSITION_MONO:
            case PA_CHANNEL_POSITION_FRONT_CENTER: mask |= SPEAKER_FRONT_CENTER; break;
            case PA_CHANNEL_POSITION_FRONT_RIGHT: mask |= SPEAKER_FRONT_RIGHT; break;
            case PA_CHANNEL_POSITION_REAR_LEFT: mask |= SPEAKER_BACK_LEFT; break;
            case PA_CHANNEL_POSITION_REAR_CENTER: mask |= SPEAKER_BACK_CENTER; break;
            case PA_CHANNEL_POSITION_REAR_RIGHT: mask |= SPEAKER_BACK_RIGHT; break;
            case PA_CHANNEL_POSITION_LFE: mask |= SPEAKER_LOW_FREQUENCY; break;
            case PA_CHANNEL_POSITION_SIDE_LEFT: mask |= SPEAKER_SIDE_LEFT; break;
            case PA_CHANNEL_POSITION_SIDE_RIGHT: mask |= SPEAKER_SIDE_RIGHT; break;
            case PA_CHANNEL_POSITION_TOP_CENTER: mask |= SPEAKER_TOP_CENTER; break;
            case PA_CHANNEL_POSITION_TOP_FRONT_LEFT: mask |= SPEAKER_TOP_FRONT_LEFT; break;
            case PA_CHANNEL_POSITION_TOP_FRONT_CENTER: mask |= SPEAKER_TOP_FRONT_CENTER; break;
            case PA_CHANNEL_POSITION_TOP_FRONT_RIGHT: mask |= SPEAKER_TOP_FRONT_RIGHT; break;
            case PA_CHANNEL_POSITION_TOP_REAR_LEFT: mask |= SPEAKER_TOP_BACK_LEFT; break;
            case PA_CHANNEL_POSITION_TOP_REAR_CENTER: mask |= SPEAKER_TOP_BACK_CENTER; break;
            case PA_CHANNEL_POSITION_TOP_REAR_RIGHT: mask |= SPEAKER_TOP_BACK_RIGHT; break;
            case PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER: mask |= SPEAKER_FRONT_LEFT_OF_CENTER; break;
            case PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER: mask |= SPEAKER_FRONT_RIGHT_OF_CENTER; break;
        }
    }

    return mask;
}

/* For default PulseAudio render device, OR together all of the
 * PKEY_AudioEndpoint_PhysicalSpeakers values of the sinks. */
static void pulse_phys_speakers_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
    if (i)
        g_phys_speakers_mask |= pulse_channel_map_to_channel_mask(&i->channel_map);
}

/* For most hardware on Windows, users must choose a configuration with an even
 * number of channels (stereo, quad, 5.1, 7.1). Users can then disable
 * channels, but those channels are still reported to applications from
 * GetMixFormat! Some applications behave badly if given an odd number of
 * channels (e.g. 2.1).  Here, we find the nearest configuration that Windows
 * would report for a given channel layout. */
static void convert_channel_map(const pa_channel_map *pa_map, WAVEFORMATEXTENSIBLE *fmt)
{
    DWORD pa_mask = pulse_channel_map_to_channel_mask(pa_map);

    TRACE("got mask for PA: 0x%x\n", pa_mask);

    if (pa_map->channels == 1)
    {
        fmt->Format.nChannels = 1;
        fmt->dwChannelMask = pa_mask;
        return;
    }

    /* compare against known configurations and find smallest configuration
     * which is a superset of the given speakers */

    if (pa_map->channels <= 2 &&
            (pa_mask & ~KSAUDIO_SPEAKER_STEREO) == 0)
    {
        fmt->Format.nChannels = 2;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
        return;
    }

    if (pa_map->channels <= 4 &&
            (pa_mask & ~KSAUDIO_SPEAKER_QUAD) == 0)
    {
        fmt->Format.nChannels = 4;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_QUAD;
        return;
    }

    if (pa_map->channels <= 4 &&
            (pa_mask & ~KSAUDIO_SPEAKER_SURROUND) == 0)
    {
        fmt->Format.nChannels = 4;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_SURROUND;
        return;
    }

    if (pa_map->channels <= 6 &&
            (pa_mask & ~KSAUDIO_SPEAKER_5POINT1) == 0)
    {
        fmt->Format.nChannels = 6;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
        return;
    }

    if (pa_map->channels <= 6 &&
            (pa_mask & ~KSAUDIO_SPEAKER_5POINT1_SURROUND) == 0)
    {
        fmt->Format.nChannels = 6;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1_SURROUND;
        return;
    }

    if (pa_map->channels <= 8 &&
            (pa_mask & ~KSAUDIO_SPEAKER_7POINT1) == 0)
    {
        fmt->Format.nChannels = 8;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
        return;
    }

    if (pa_map->channels <= 8 &&
            (pa_mask & ~KSAUDIO_SPEAKER_7POINT1_SURROUND) == 0)
    {
        fmt->Format.nChannels = 8;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
        return;
    }

    /* oddball format, report truthfully */
    fmt->Format.nChannels = pa_map->channels;
    fmt->dwChannelMask = pa_mask;
}

static void pulse_probe_settings(int render, WAVEFORMATEXTENSIBLE *fmt) {
    WAVEFORMATEX *wfx = &fmt->Format;
    pa_stream *stream;
    pa_channel_map map;
    pa_sample_spec ss;
    pa_buffer_attr attr;
    int ret;
    unsigned int length = 0;

    pa_channel_map_init_auto(&map, 2, PA_CHANNEL_MAP_ALSA);
    ss.rate = 48000;
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.channels = map.channels;

    attr.maxlength = -1;
    attr.tlength = -1;
    attr.minreq = attr.fragsize = pa_frame_size(&ss);
    attr.prebuf = 0;

    stream = pa_stream_new(pulse_ctx, "format test stream", &ss, &map);
    if (stream)
        pa_stream_set_state_callback(stream, pulse_stream_state, NULL);
    if (!stream)
        ret = -1;
    else if (render)
        ret = pa_stream_connect_playback(stream, NULL, &attr,
        PA_STREAM_START_CORKED|PA_STREAM_FIX_RATE|PA_STREAM_FIX_CHANNELS|PA_STREAM_EARLY_REQUESTS, NULL, NULL);
    else
        ret = pa_stream_connect_record(stream, NULL, &attr, PA_STREAM_START_CORKED|PA_STREAM_FIX_RATE|PA_STREAM_FIX_CHANNELS|PA_STREAM_EARLY_REQUESTS);
    if (ret >= 0) {
        while (pa_mainloop_iterate(pulse_ml, 1, &ret) >= 0 &&
                pa_stream_get_state(stream) == PA_STREAM_CREATING)
        {}
        if (pa_stream_get_state(stream) == PA_STREAM_READY) {
            ss = *pa_stream_get_sample_spec(stream);
            map = *pa_stream_get_channel_map(stream);
            if (render)
                length = pa_stream_get_buffer_attr(stream)->minreq;
            else
                length = pa_stream_get_buffer_attr(stream)->fragsize;
            pa_stream_disconnect(stream);
            while (pa_mainloop_iterate(pulse_ml, 1, &ret) >= 0 &&
                    pa_stream_get_state(stream) == PA_STREAM_READY)
            {}
        }
    }

    if (stream)
        pa_stream_unref(stream);

    if (length)
        pulse_def_period[!render] = pulse_min_period[!render] = pa_bytes_to_usec(10 * length, &ss);

    if (pulse_min_period[!render] < MinimumPeriod)
        pulse_min_period[!render] = MinimumPeriod;

    if (pulse_def_period[!render] < DefaultPeriod)
        pulse_def_period[!render] = DefaultPeriod;

    wfx->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx->cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    convert_channel_map(&map, fmt);

    wfx->wBitsPerSample = 8 * pa_sample_size_of_format(ss.format);
    wfx->nSamplesPerSec = ss.rate;
    wfx->nBlockAlign = wfx->nChannels * wfx->wBitsPerSample / 8;
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    if (ss.format != PA_SAMPLE_S24_32LE)
        fmt->Samples.wValidBitsPerSample = wfx->wBitsPerSample;
    else
        fmt->Samples.wValidBitsPerSample = 24;
    if (ss.format == PA_SAMPLE_FLOAT32LE)
        fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    else
        fmt->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

/* some poorly-behaved applications call audio functions during DllMain, so we
 * have to do as much as possible without creating a new thread. this function
 * sets up a synchronous connection to verify the server is running and query
 * static data. */
static HRESULT WINAPI pulse_test_connect(const char *name, struct pulse_config *config)
{
    pa_operation *o;
    int ret;

    pulse_lock();
    pulse_ml = pa_mainloop_new();

    pa_mainloop_set_poll_func(pulse_ml, pulse_poll_func, NULL);

    pulse_ctx = pa_context_new(pa_mainloop_get_api(pulse_ml), name);
    if (!pulse_ctx) {
        ERR("Failed to create context\n");
        pa_mainloop_free(pulse_ml);
        pulse_ml = NULL;
        pulse_unlock();
        return E_FAIL;
    }

    pa_context_set_state_callback(pulse_ctx, pulse_contextcallback, NULL);

    TRACE("libpulse protocol version: %u. API Version %u\n", pa_context_get_protocol_version(pulse_ctx), PA_API_VERSION);
    if (pa_context_connect(pulse_ctx, NULL, 0, NULL) < 0)
        goto fail;

    /* Wait for connection */
    while (pa_mainloop_iterate(pulse_ml, 1, &ret) >= 0) {
        pa_context_state_t state = pa_context_get_state(pulse_ctx);

        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
            goto fail;

        if (state == PA_CONTEXT_READY)
            break;
    }

    if (pa_context_get_state(pulse_ctx) != PA_CONTEXT_READY)
        goto fail;

    TRACE("Test-connected to server %s with protocol version: %i.\n",
        pa_context_get_server(pulse_ctx),
        pa_context_get_server_protocol_version(pulse_ctx));

    pulse_probe_settings(1, &pulse_fmt[0]);
    pulse_probe_settings(0, &pulse_fmt[1]);

    g_phys_speakers_mask = 0;
    o = pa_context_get_sink_info_list(pulse_ctx, &pulse_phys_speakers_cb, NULL);
    if (o) {
        while (pa_mainloop_iterate(pulse_ml, 1, &ret) >= 0 &&
                pa_operation_get_state(o) == PA_OPERATION_RUNNING)
        {}
        pa_operation_unref(o);
    }

    pa_context_unref(pulse_ctx);
    pulse_ctx = NULL;
    pa_mainloop_free(pulse_ml);
    pulse_ml = NULL;

    config->speakers_mask = g_phys_speakers_mask;
    config->modes[0].format = pulse_fmt[0];
    config->modes[0].def_period = pulse_def_period[0];
    config->modes[0].min_period = pulse_min_period[0];
    config->modes[1].format = pulse_fmt[1];
    config->modes[1].def_period = pulse_def_period[1];
    config->modes[1].min_period = pulse_min_period[1];

    pulse_unlock();

    return S_OK;

fail:
    pa_context_unref(pulse_ctx);
    pulse_ctx = NULL;
    pa_mainloop_free(pulse_ml);
    pulse_ml = NULL;
    pulse_unlock();

    return E_FAIL;
}

static DWORD get_channel_mask(unsigned int channels)
{
    switch(channels) {
    case 0:
        return 0;
    case 1:
        return KSAUDIO_SPEAKER_MONO;
    case 2:
        return KSAUDIO_SPEAKER_STEREO;
    case 3:
        return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4:
        return KSAUDIO_SPEAKER_QUAD;    /* not _SURROUND */
    case 5:
        return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6:
        return KSAUDIO_SPEAKER_5POINT1; /* not 5POINT1_SURROUND */
    case 7:
        return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND; /* Vista deprecates 7POINT1 */
    }
    FIXME("Unknown speaker configuration: %u\n", channels);
    return 0;
}

static const enum pa_channel_position pulse_pos_from_wfx[] = {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
    PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
    PA_CHANNEL_POSITION_REAR_CENTER,
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT,
    PA_CHANNEL_POSITION_TOP_CENTER,
    PA_CHANNEL_POSITION_TOP_FRONT_LEFT,
    PA_CHANNEL_POSITION_TOP_FRONT_CENTER,
    PA_CHANNEL_POSITION_TOP_FRONT_RIGHT,
    PA_CHANNEL_POSITION_TOP_REAR_LEFT,
    PA_CHANNEL_POSITION_TOP_REAR_CENTER,
    PA_CHANNEL_POSITION_TOP_REAR_RIGHT
};

static HRESULT pulse_spec_from_waveformat(struct pulse_stream *stream, const WAVEFORMATEX *fmt)
{
    pa_channel_map_init(&stream->map);
    stream->ss.rate = fmt->nSamplesPerSec;
    stream->ss.format = PA_SAMPLE_INVALID;

    switch(fmt->wFormatTag) {
    case WAVE_FORMAT_IEEE_FLOAT:
        if (!fmt->nChannels || fmt->nChannels > 2 || fmt->wBitsPerSample != 32)
            break;
        stream->ss.format = PA_SAMPLE_FLOAT32LE;
        pa_channel_map_init_auto(&stream->map, fmt->nChannels, PA_CHANNEL_MAP_ALSA);
        break;
    case WAVE_FORMAT_PCM:
        if (!fmt->nChannels || fmt->nChannels > 2)
            break;
        if (fmt->wBitsPerSample == 8)
            stream->ss.format = PA_SAMPLE_U8;
        else if (fmt->wBitsPerSample == 16)
            stream->ss.format = PA_SAMPLE_S16LE;
        else
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        pa_channel_map_init_auto(&stream->map, fmt->nChannels, PA_CHANNEL_MAP_ALSA);
        break;
    case WAVE_FORMAT_EXTENSIBLE: {
        WAVEFORMATEXTENSIBLE *wfe = (WAVEFORMATEXTENSIBLE*)fmt;
        DWORD mask = wfe->dwChannelMask;
        DWORD i = 0, j;
        if (fmt->cbSize != (sizeof(*wfe) - sizeof(*fmt)) && fmt->cbSize != sizeof(*wfe))
            break;
        if (IsEqualGUID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
            (!wfe->Samples.wValidBitsPerSample || wfe->Samples.wValidBitsPerSample == 32) &&
            fmt->wBitsPerSample == 32)
            stream->ss.format = PA_SAMPLE_FLOAT32LE;
        else if (IsEqualGUID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM)) {
            DWORD valid = wfe->Samples.wValidBitsPerSample;
            if (!valid)
                valid = fmt->wBitsPerSample;
            if (!valid || valid > fmt->wBitsPerSample)
                break;
            switch (fmt->wBitsPerSample) {
                case 8:
                    if (valid == 8)
                        stream->ss.format = PA_SAMPLE_U8;
                    break;
                case 16:
                    if (valid == 16)
                        stream->ss.format = PA_SAMPLE_S16LE;
                    break;
                case 24:
                    if (valid == 24)
                        stream->ss.format = PA_SAMPLE_S24LE;
                    break;
                case 32:
                    if (valid == 24)
                        stream->ss.format = PA_SAMPLE_S24_32LE;
                    else if (valid == 32)
                        stream->ss.format = PA_SAMPLE_S32LE;
                    break;
                default:
                    return AUDCLNT_E_UNSUPPORTED_FORMAT;
            }
        }
        stream->map.channels = fmt->nChannels;
        if (!mask || (mask & (SPEAKER_ALL|SPEAKER_RESERVED)))
            mask = get_channel_mask(fmt->nChannels);
        for (j = 0; j < ARRAY_SIZE(pulse_pos_from_wfx) && i < fmt->nChannels; ++j) {
            if (mask & (1 << j))
                stream->map.map[i++] = pulse_pos_from_wfx[j];
        }

        /* Special case for mono since pulse appears to map it differently */
        if (mask == SPEAKER_FRONT_CENTER)
            stream->map.map[0] = PA_CHANNEL_POSITION_MONO;

        if (i < fmt->nChannels || (mask & SPEAKER_RESERVED)) {
            stream->map.channels = 0;
            ERR("Invalid channel mask: %i/%i and %x(%x)\n", i, fmt->nChannels, mask, wfe->dwChannelMask);
            break;
        }
        break;
        }
    case WAVE_FORMAT_ALAW:
    case WAVE_FORMAT_MULAW:
        if (fmt->wBitsPerSample != 8) {
            FIXME("Unsupported bpp %u for LAW\n", fmt->wBitsPerSample);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        if (fmt->nChannels != 1 && fmt->nChannels != 2) {
            FIXME("Unsupported channels %u for LAW\n", fmt->nChannels);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        stream->ss.format = fmt->wFormatTag == WAVE_FORMAT_MULAW ? PA_SAMPLE_ULAW : PA_SAMPLE_ALAW;
        pa_channel_map_init_auto(&stream->map, fmt->nChannels, PA_CHANNEL_MAP_ALSA);
        break;
    default:
        WARN("Unhandled tag %x\n", fmt->wFormatTag);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    stream->ss.channels = stream->map.channels;
    if (!pa_channel_map_valid(&stream->map) || stream->ss.format == PA_SAMPLE_INVALID) {
        ERR("Invalid format! Channel spec valid: %i, format: %i\n",
            pa_channel_map_valid(&stream->map), stream->ss.format);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    return S_OK;
}

static HRESULT pulse_stream_connect(struct pulse_stream *stream, UINT32 period_bytes)
{
    int ret;
    char buffer[64];
    static LONG number;
    pa_buffer_attr attr;

    ret = InterlockedIncrement(&number);
    sprintf(buffer, "audio stream #%i", ret);
    stream->stream = pa_stream_new(pulse_ctx, buffer, &stream->ss, &stream->map);

    if (!stream->stream) {
        WARN("pa_stream_new returned error %i\n", pa_context_errno(pulse_ctx));
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }

    pa_stream_set_state_callback(stream->stream, pulse_stream_state, stream);
    pa_stream_set_buffer_attr_callback(stream->stream, pulse_attr_update, stream);
    pa_stream_set_moved_callback(stream->stream, pulse_attr_update, stream);

    /* PulseAudio will fill in correct values */
    attr.minreq = attr.fragsize = period_bytes;
    attr.tlength = period_bytes * 3;
    attr.maxlength = stream->bufsize_frames * pa_frame_size(&stream->ss);
    attr.prebuf = pa_frame_size(&stream->ss);
    dump_attr(&attr);
    if (stream->dataflow == eRender)
        ret = pa_stream_connect_playback(stream->stream, NULL, &attr,
        PA_STREAM_START_CORKED|PA_STREAM_START_UNMUTED|PA_STREAM_ADJUST_LATENCY, NULL, NULL);
    else
        ret = pa_stream_connect_record(stream->stream, NULL, &attr,
        PA_STREAM_START_CORKED|PA_STREAM_START_UNMUTED|PA_STREAM_ADJUST_LATENCY);
    if (ret < 0) {
        WARN("Returns %i\n", ret);
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }
    while (pa_stream_get_state(stream->stream) == PA_STREAM_CREATING)
        pulse_cond_wait();
    if (pa_stream_get_state(stream->stream) != PA_STREAM_READY)
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;

    if (stream->dataflow == eRender) {
        pa_stream_set_underflow_callback(stream->stream, pulse_underflow_callback, stream);
        pa_stream_set_started_callback(stream->stream, pulse_started_callback, stream);
    }
    return S_OK;
}

static HRESULT WINAPI pulse_create_stream(const char *name, EDataFlow dataflow, AUDCLNT_SHAREMODE mode,
                                          DWORD flags, REFERENCE_TIME duration, REFERENCE_TIME period,
                                          const WAVEFORMATEX *fmt, UINT32 *channel_count,
                                          struct pulse_stream **ret)
{
    struct pulse_stream *stream;
    unsigned int i, bufsize_bytes;
    HRESULT hr;

    if (FAILED(hr = pulse_connect(name)))
        return hr;

    if (!(stream = RtlAllocateHeap(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*stream))))
        return E_OUTOFMEMORY;

    stream->dataflow = dataflow;
    for (i = 0; i < ARRAY_SIZE(stream->vol); ++i)
        stream->vol[i] = 1.f;

    hr = pulse_spec_from_waveformat(stream, fmt);
    TRACE("Obtaining format returns %08x\n", hr);

    if (FAILED(hr))
        goto exit;

    period = pulse_def_period[dataflow == eCapture];
    if (duration < 3 * period)
        duration = 3 * period;

    stream->period_bytes = pa_frame_size(&stream->ss) * muldiv(period, stream->ss.rate, 10000000);

    stream->bufsize_frames = ceil((duration / 10000000.) * fmt->nSamplesPerSec);
    bufsize_bytes = stream->bufsize_frames * pa_frame_size(&stream->ss);
    stream->mmdev_period_usec = period / 10;

    stream->share = mode;
    stream->flags = flags;
    hr = pulse_stream_connect(stream, stream->period_bytes);
    if (SUCCEEDED(hr)) {
        UINT32 unalign;
        const pa_buffer_attr *attr = pa_stream_get_buffer_attr(stream->stream);
        stream->attr = *attr;
        /* Update frames according to new size */
        dump_attr(attr);
        if (dataflow == eRender) {
            stream->real_bufsize_bytes = stream->bufsize_frames * 2 * pa_frame_size(&stream->ss);
            stream->local_buffer = RtlAllocateHeap(GetProcessHeap(), 0, stream->real_bufsize_bytes);
            if(!stream->local_buffer)
                hr = E_OUTOFMEMORY;
        } else {
            UINT32 i, capture_packets;

            if ((unalign = bufsize_bytes % stream->period_bytes))
                bufsize_bytes += stream->period_bytes - unalign;
            stream->bufsize_frames = bufsize_bytes / pa_frame_size(&stream->ss);
            stream->real_bufsize_bytes = bufsize_bytes;

            capture_packets = stream->real_bufsize_bytes / stream->period_bytes;

            stream->local_buffer = RtlAllocateHeap(GetProcessHeap(), 0, stream->real_bufsize_bytes + capture_packets * sizeof(ACPacket));
            if (!stream->local_buffer)
                hr = E_OUTOFMEMORY;
            else {
                ACPacket *cur_packet = (ACPacket*)((char*)stream->local_buffer + stream->real_bufsize_bytes);
                BYTE *data = stream->local_buffer;
                silence_buffer(stream->ss.format, stream->local_buffer, stream->real_bufsize_bytes);
                list_init(&stream->packet_free_head);
                list_init(&stream->packet_filled_head);
                for (i = 0; i < capture_packets; ++i, ++cur_packet) {
                    list_add_tail(&stream->packet_free_head, &cur_packet->entry);
                    cur_packet->data = data;
                    data += stream->period_bytes;
                }
            }
        }
    }

exit:
    if (FAILED(hr)) {
        free(stream->local_buffer);
        if (stream->stream) {
            pa_stream_disconnect(stream->stream);
            pa_stream_unref(stream->stream);
            RtlFreeHeap(GetProcessHeap(), 0, stream);
        }
        return hr;
    }

    *channel_count = stream->ss.channels;
    *ret = stream;
    return S_OK;
}

static void WINAPI pulse_release_stream(struct pulse_stream *stream, HANDLE timer)
{
    if(timer) {
        stream->please_quit = TRUE;
        NtWaitForSingleObject(timer, FALSE, NULL);
        NtClose(timer);
    }

    pulse_lock();
    if (PA_STREAM_IS_GOOD(pa_stream_get_state(stream->stream))) {
        pa_stream_disconnect(stream->stream);
        while (PA_STREAM_IS_GOOD(pa_stream_get_state(stream->stream)))
            pulse_cond_wait();
    }
    pa_stream_unref(stream->stream);
    pulse_unlock();

    RtlFreeHeap(GetProcessHeap(), 0, stream->tmp_buffer);
    RtlFreeHeap(GetProcessHeap(), 0, stream->peek_buffer);
    RtlFreeHeap(GetProcessHeap(), 0, stream->local_buffer);
    RtlFreeHeap(GetProcessHeap(), 0, stream);
}

static int write_buffer(const struct pulse_stream *stream, BYTE *buffer, UINT32 bytes)
{
    const float *vol = stream->vol;
    UINT32 i, channels, mute = 0;
    BOOL adjust = FALSE;
    BYTE *end;

    if (!bytes) return 0;

    /* Adjust the buffer based on the volume for each channel */
    channels = stream->ss.channels;
    for (i = 0; i < channels; i++)
    {
        adjust |= vol[i] != 1.0f;
        if (vol[i] == 0.0f)
            mute++;
    }
    if (mute == channels)
    {
        silence_buffer(stream->ss.format, buffer, bytes);
        goto write;
    }
    if (!adjust) goto write;

    end = buffer + bytes;
    switch (stream->ss.format)
    {
#ifndef WORDS_BIGENDIAN
#define PROCESS_BUFFER(type) do         \
{                                       \
    type *p = (type*)buffer;            \
    do                                  \
    {                                   \
        for (i = 0; i < channels; i++)  \
            p[i] = p[i] * vol[i];       \
        p += i;                         \
    } while ((BYTE*)p != end);          \
} while (0)
    case PA_SAMPLE_S16LE:
        PROCESS_BUFFER(INT16);
        break;
    case PA_SAMPLE_S32LE:
        PROCESS_BUFFER(INT32);
        break;
    case PA_SAMPLE_FLOAT32LE:
        PROCESS_BUFFER(float);
        break;
#undef PROCESS_BUFFER
    case PA_SAMPLE_S24_32LE:
    {
        UINT32 *p = (UINT32*)buffer;
        do
        {
            for (i = 0; i < channels; i++)
            {
                p[i] = (INT32)((INT32)(p[i] << 8) * vol[i]);
                p[i] >>= 8;
            }
            p += i;
        } while ((BYTE*)p != end);
        break;
    }
    case PA_SAMPLE_S24LE:
    {
        /* do it 12 bytes at a time until it is no longer possible */
        UINT32 *q = (UINT32*)buffer;
        BYTE *p;

        i = 0;
        while (end - (BYTE*)q >= 12)
        {
            UINT32 v[4], k;
            v[0] = q[0] << 8;
            v[1] = q[1] << 16 | (q[0] >> 16 & ~0xff);
            v[2] = q[2] << 24 | (q[1] >> 8  & ~0xff);
            v[3] = q[2] & ~0xff;
            for (k = 0; k < 4; k++)
            {
                v[k] = (INT32)((INT32)v[k] * vol[i]);
                if (++i == channels) i = 0;
            }
            *q++ = v[0] >> 8  | (v[1] & ~0xff) << 16;
            *q++ = v[1] >> 16 | (v[2] & ~0xff) << 8;
            *q++ = v[2] >> 24 | (v[3] & ~0xff);
        }
        p = (BYTE*)q;
        while (p != end)
        {
            UINT32 v = (INT32)((INT32)(p[0] << 8 | p[1] << 16 | p[2] << 24) * vol[i]);
            *p++ = v >> 8  & 0xff;
            *p++ = v >> 16 & 0xff;
            *p++ = v >> 24;
            if (++i == channels) i = 0;
        }
        break;
    }
#endif
    case PA_SAMPLE_U8:
    {
        UINT8 *p = (UINT8*)buffer;
        do
        {
            for (i = 0; i < channels; i++)
                p[i] = (int)((p[i] - 128) * vol[i]) + 128;
            p += i;
        } while ((BYTE*)p != end);
        break;
    }
    case PA_SAMPLE_ALAW:
    {
        UINT8 *p = (UINT8*)buffer;
        do
        {
            for (i = 0; i < channels; i++)
                p[i] = mult_alaw_sample(p[i], vol[i]);
            p += i;
        } while ((BYTE*)p != end);
        break;
    }
    case PA_SAMPLE_ULAW:
    {
        UINT8 *p = (UINT8*)buffer;
        do
        {
            for (i = 0; i < channels; i++)
                p[i] = mult_ulaw_sample(p[i], vol[i]);
            p += i;
        } while ((BYTE*)p != end);
        break;
    }
    default:
        TRACE("Unhandled format %i, not adjusting volume.\n", stream->ss.format);
        break;
    }

write:
    return pa_stream_write(stream->stream, buffer, bytes, NULL, 0, PA_SEEK_RELATIVE);
}

static void pulse_write(struct pulse_stream *stream)
{
    /* write as much data to PA as we can */
    UINT32 to_write;
    BYTE *buf = stream->local_buffer + stream->pa_offs_bytes;
    UINT32 bytes = pa_stream_writable_size(stream->stream);

    if (stream->just_underran)
    {
        /* prebuffer with silence if needed */
        if(stream->pa_held_bytes < bytes){
            to_write = bytes - stream->pa_held_bytes;
            TRACE("prebuffering %u frames of silence\n",
                    (int)(to_write / pa_frame_size(&stream->ss)));
            buf = RtlAllocateHeap(GetProcessHeap(), HEAP_ZERO_MEMORY, to_write);
            pa_stream_write(stream->stream, buf, to_write, NULL, 0, PA_SEEK_RELATIVE);
            RtlFreeHeap(GetProcessHeap(), 0, buf);
        }

        stream->just_underran = FALSE;
    }

    buf = stream->local_buffer + stream->pa_offs_bytes;
    TRACE("held: %u, avail: %u\n",
            stream->pa_held_bytes, bytes);
    bytes = min(stream->pa_held_bytes, bytes);

    if (stream->pa_offs_bytes + bytes > stream->real_bufsize_bytes)
    {
        to_write = stream->real_bufsize_bytes - stream->pa_offs_bytes;
        TRACE("writing small chunk of %u bytes\n", to_write);
        write_buffer(stream, buf, to_write);
        stream->pa_held_bytes -= to_write;
        to_write = bytes - to_write;
        stream->pa_offs_bytes = 0;
        buf = stream->local_buffer;
    }
    else
        to_write = bytes;

    TRACE("writing main chunk of %u bytes\n", to_write);
    write_buffer(stream, buf, to_write);
    stream->pa_offs_bytes += to_write;
    stream->pa_offs_bytes %= stream->real_bufsize_bytes;
    stream->pa_held_bytes -= to_write;
}

static void pulse_read(struct pulse_stream *stream)
{
    size_t bytes = pa_stream_readable_size(stream->stream);

    TRACE("Readable total: %zu, fragsize: %u\n", bytes, pa_stream_get_buffer_attr(stream->stream)->fragsize);

    bytes += stream->peek_len - stream->peek_ofs;

    while (bytes >= stream->period_bytes)
    {
        BYTE *dst = NULL, *src;
        size_t src_len, copy, rem = stream->period_bytes;

        if (stream->started)
        {
            LARGE_INTEGER stamp, freq;
            ACPacket *p, *next;

            if (!(p = (ACPacket*)list_head(&stream->packet_free_head)))
            {
                p = (ACPacket*)list_head(&stream->packet_filled_head);
                if (!p) return;
                if (!p->discont) {
                    next = (ACPacket*)p->entry.next;
                    next->discont = 1;
                } else
                    p = (ACPacket*)list_tail(&stream->packet_filled_head);
            }
            else
            {
                stream->held_bytes += stream->period_bytes;
            }
            NtQueryPerformanceCounter(&stamp, &freq);
            p->qpcpos = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
            p->discont = 0;
            list_remove(&p->entry);
            list_add_tail(&stream->packet_filled_head, &p->entry);

            dst = p->data;
        }

        while (rem)
        {
            if (stream->peek_len)
            {
                copy = min(rem, stream->peek_len - stream->peek_ofs);

                if (dst)
                {
                    memcpy(dst, stream->peek_buffer + stream->peek_ofs, copy);
                    dst += copy;
                }

                rem -= copy;
                stream->peek_ofs += copy;
                if(stream->peek_len == stream->peek_ofs)
                    stream->peek_len = stream->peek_ofs = 0;

            }
            else if (pa_stream_peek(stream->stream, (const void**)&src, &src_len) == 0 && src_len)
            {
                copy = min(rem, src_len);

                if (dst) {
                    if(src)
                        memcpy(dst, src, copy);
                    else
                        silence_buffer(stream->ss.format, dst, copy);

                    dst += copy;
                }

                rem -= copy;

                if (copy < src_len)
                {
                    if (src_len > stream->peek_buffer_len)
                    {
                        RtlFreeHeap(GetProcessHeap(), 0, stream->peek_buffer);
                        stream->peek_buffer = RtlAllocateHeap(GetProcessHeap(), 0, src_len);
                        stream->peek_buffer_len = src_len;
                    }

                    if(src)
                        memcpy(stream->peek_buffer, src + copy, src_len - copy);
                    else
                        silence_buffer(stream->ss.format, stream->peek_buffer, src_len - copy);

                    stream->peek_len = src_len - copy;
                    stream->peek_ofs = 0;
                }

                pa_stream_drop(stream->stream);
            }
        }

        bytes -= stream->period_bytes;
    }
}

static void WINAPI pulse_timer_loop(struct pulse_stream *stream)
{
    LARGE_INTEGER delay;
    UINT32 adv_bytes;
    int success;
    pa_operation *o;

    pulse_lock();
    delay.QuadPart = -stream->mmdev_period_usec * 10;
    pa_stream_get_time(stream->stream, &stream->last_time);
    pulse_unlock();

    while (!stream->please_quit)
    {
        pa_usec_t now, adv_usec = 0;
        int err;

        NtDelayExecution(FALSE, &delay);

        pulse_lock();

        delay.QuadPart = -stream->mmdev_period_usec * 10;

        o = pa_stream_update_timing_info(stream->stream, pulse_op_cb, &success);
        if (o)
        {
            while (pa_operation_get_state(o) == PA_OPERATION_RUNNING)
                pulse_cond_wait();
            pa_operation_unref(o);
        }
        err = pa_stream_get_time(stream->stream, &now);
        if (err == 0)
        {
            TRACE("got now: %s, last time: %s\n", wine_dbgstr_longlong(now), wine_dbgstr_longlong(stream->last_time));
            if (stream->started && (stream->dataflow == eCapture || stream->held_bytes))
            {
                if(stream->just_underran)
                {
                    stream->last_time = now;
                    stream->just_started = TRUE;
                }

                if (stream->just_started)
                {
                    /* let it play out a period to absorb some latency and get accurate timing */
                    pa_usec_t diff = now - stream->last_time;

                    if (diff > stream->mmdev_period_usec)
                    {
                        stream->just_started = FALSE;
                        stream->last_time = now;
                    }
                }
                else
                {
                    INT32 adjust = stream->last_time + stream->mmdev_period_usec - now;

                    adv_usec = now - stream->last_time;

                    if(adjust > ((INT32)(stream->mmdev_period_usec / 2)))
                        adjust = stream->mmdev_period_usec / 2;
                    else if(adjust < -((INT32)(stream->mmdev_period_usec / 2)))
                        adjust = -1 * stream->mmdev_period_usec / 2;

                    delay.QuadPart = -(stream->mmdev_period_usec + adjust) * 10;

                    stream->last_time += stream->mmdev_period_usec;
                }

                if (stream->dataflow == eRender)
                {
                    pulse_write(stream);

                    /* regardless of what PA does, advance one period */
                    adv_bytes = min(stream->period_bytes, stream->held_bytes);
                    stream->lcl_offs_bytes += adv_bytes;
                    stream->lcl_offs_bytes %= stream->real_bufsize_bytes;
                    stream->held_bytes -= adv_bytes;
                }
                else if(stream->dataflow == eCapture)
                {
                    pulse_read(stream);
                }
            }
            else
            {
                stream->last_time = now;
                delay.QuadPart = -stream->mmdev_period_usec * 10;
            }
        }

        if (stream->event)
            NtSetEvent(stream->event, NULL);

        TRACE("%p after update, adv usec: %d, held: %u, delay usec: %u\n",
                stream, (int)adv_usec,
                (int)(stream->held_bytes/ pa_frame_size(&stream->ss)),
                (unsigned int)(-delay.QuadPart / 10));

        pulse_unlock();
    }
}

static HRESULT WINAPI pulse_start(struct pulse_stream *stream)
{
    HRESULT hr = S_OK;
    int success;
    pa_operation *o;

    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        return hr;
    }

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
    {
        pulse_unlock();
        return AUDCLNT_E_EVENTHANDLE_NOT_SET;
    }

    if (stream->started)
    {
        pulse_unlock();
        return AUDCLNT_E_NOT_STOPPED;
    }

    pulse_write(stream);

    if (pa_stream_is_corked(stream->stream))
    {
        o = pa_stream_cork(stream->stream, 0, pulse_op_cb, &success);
        if (o)
        {
            while(pa_operation_get_state(o) == PA_OPERATION_RUNNING)
                pulse_cond_wait();
            pa_operation_unref(o);
        }
        else
            success = 0;
        if (!success)
            hr = E_FAIL;
    }

    if (SUCCEEDED(hr))
    {
        stream->started = TRUE;
        stream->just_started = TRUE;
    }
    pulse_unlock();
    return hr;
}

static HRESULT WINAPI pulse_stop(struct pulse_stream *stream)
{
    HRESULT hr = S_OK;
    pa_operation *o;
    int success;

    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }

    if (!stream->started)
    {
        pulse_unlock();
        return S_FALSE;
    }

    if (stream->dataflow == eRender)
    {
        o = pa_stream_cork(stream->stream, 1, pulse_op_cb, &success);
        if (o)
        {
            while(pa_operation_get_state(o) == PA_OPERATION_RUNNING)
                pulse_cond_wait();
            pa_operation_unref(o);
        }
        else
            success = 0;
        if (!success)
            hr = E_FAIL;
    }
    if (SUCCEEDED(hr))
        stream->started = FALSE;
    pulse_unlock();
    return hr;
}

static HRESULT WINAPI pulse_reset(struct pulse_stream *stream)
{
    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }

    if (stream->started)
    {
        pulse_unlock();
        return AUDCLNT_E_NOT_STOPPED;
    }

    if (stream->locked)
    {
        pulse_unlock();
        return AUDCLNT_E_BUFFER_OPERATION_PENDING;
    }

    if (stream->dataflow == eRender)
    {
        /* If there is still data in the render buffer it needs to be removed from the server */
        int success = 0;
        if (stream->held_bytes)
        {
            pa_operation *o = pa_stream_flush(stream->stream, pulse_op_cb, &success);
            if (o)
            {
                while (pa_operation_get_state(o) == PA_OPERATION_RUNNING)
                    pulse_cond_wait();
                pa_operation_unref(o);
            }
        }
        if (success || !stream->held_bytes)
        {
            stream->clock_lastpos = stream->clock_written = 0;
            stream->pa_offs_bytes = stream->lcl_offs_bytes = 0;
            stream->held_bytes = stream->pa_held_bytes = 0;
        }
    }
    else
    {
        ACPacket *p;
        stream->clock_written += stream->held_bytes;
        stream->held_bytes = 0;

        if ((p = stream->locked_ptr))
        {
            stream->locked_ptr = NULL;
            list_add_tail(&stream->packet_free_head, &p->entry);
        }
        list_move_tail(&stream->packet_free_head, &stream->packet_filled_head);
    }
    pulse_unlock();
    return S_OK;
}

static void alloc_tmp_buffer(struct pulse_stream *stream, UINT32 bytes)
{
    if (stream->tmp_buffer_bytes >= bytes)
        return;

    RtlFreeHeap(GetProcessHeap(), 0, stream->tmp_buffer);
    stream->tmp_buffer = RtlAllocateHeap(GetProcessHeap(), 0, bytes);
    stream->tmp_buffer_bytes = bytes;
}

static HRESULT WINAPI pulse_get_render_buffer(struct pulse_stream *stream, UINT32 frames, BYTE **data)
{
    size_t bytes;
    UINT32 wri_offs_bytes;

    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }

    if (stream->locked)
    {
        pulse_unlock();
        return AUDCLNT_E_OUT_OF_ORDER;
    }

    if (!frames)
    {
        pulse_unlock();
        *data = NULL;
        return S_OK;
    }

    if (stream->held_bytes / pa_frame_size(&stream->ss) + frames > stream->bufsize_frames)
    {
        pulse_unlock();
        return AUDCLNT_E_BUFFER_TOO_LARGE;
    }

    bytes = frames * pa_frame_size(&stream->ss);
    wri_offs_bytes = (stream->lcl_offs_bytes + stream->held_bytes) % stream->real_bufsize_bytes;
    if (wri_offs_bytes + bytes > stream->real_bufsize_bytes)
    {
        alloc_tmp_buffer(stream, bytes);
        *data = stream->tmp_buffer;
        stream->locked = -bytes;
    }
    else
    {
        *data = stream->local_buffer + wri_offs_bytes;
        stream->locked = bytes;
    }

    silence_buffer(stream->ss.format, *data, bytes);

    pulse_unlock();
    return S_OK;
}

static void pulse_wrap_buffer(struct pulse_stream *stream, BYTE *buffer, UINT32 written_bytes)
{
    UINT32 wri_offs_bytes = (stream->lcl_offs_bytes + stream->held_bytes) % stream->real_bufsize_bytes;
    UINT32 chunk_bytes = stream->real_bufsize_bytes - wri_offs_bytes;

    if (written_bytes <= chunk_bytes)
    {
        memcpy(stream->local_buffer + wri_offs_bytes, buffer, written_bytes);
    }
    else
    {
        memcpy(stream->local_buffer + wri_offs_bytes, buffer, chunk_bytes);
        memcpy(stream->local_buffer, buffer + chunk_bytes, written_bytes - chunk_bytes);
    }
}

static HRESULT WINAPI pulse_release_render_buffer(struct pulse_stream *stream, UINT32 written_frames,
                                                  DWORD flags)
{
    UINT32 written_bytes;
    BYTE *buffer;

    pulse_lock();
    if (!stream->locked || !written_frames)
    {
        stream->locked = 0;
        pulse_unlock();
        return written_frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
    }

    if (written_frames * pa_frame_size(&stream->ss) > (stream->locked >= 0 ? stream->locked : -stream->locked))
    {
        pulse_unlock();
        return AUDCLNT_E_INVALID_SIZE;
    }

    if (stream->locked >= 0)
        buffer = stream->local_buffer + (stream->lcl_offs_bytes + stream->held_bytes) % stream->real_bufsize_bytes;
    else
        buffer = stream->tmp_buffer;

    written_bytes = written_frames * pa_frame_size(&stream->ss);
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        silence_buffer(stream->ss.format, buffer, written_bytes);

    if (stream->locked < 0)
        pulse_wrap_buffer(stream, buffer, written_bytes);

    stream->held_bytes += written_bytes;
    stream->pa_held_bytes += written_bytes;
    if (stream->pa_held_bytes > stream->real_bufsize_bytes)
    {
        stream->pa_offs_bytes += stream->pa_held_bytes - stream->real_bufsize_bytes;
        stream->pa_offs_bytes %= stream->real_bufsize_bytes;
        stream->pa_held_bytes = stream->real_bufsize_bytes;
    }
    stream->clock_written += written_bytes;
    stream->locked = 0;

    TRACE("Released %u, held %zu\n", written_frames, stream->held_bytes / pa_frame_size(&stream->ss));

    pulse_unlock();
    return S_OK;
}

static HRESULT WINAPI pulse_get_buffer_size(struct pulse_stream *stream, UINT32 *out)
{
    HRESULT hr = S_OK;

    pulse_lock();
    if (!pulse_stream_valid(stream))
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
    else
        *out = stream->bufsize_frames;
    pulse_unlock();

    return hr;
}

static void WINAPI pulse_set_volumes(struct pulse_stream *stream, float master_volume,
                                     const float *volumes, const float *session_volumes)
{
    unsigned int i;

    for (i = 0; i < stream->ss.channels; i++)
        stream->vol[i] = volumes[i] * master_volume * session_volumes[i];
}

static HRESULT WINAPI pulse_set_event_handle(struct pulse_stream *stream, HANDLE event)
{
    HRESULT hr = S_OK;

    pulse_lock();
    if (!pulse_stream_valid(stream))
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
    else if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        hr = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    else if (stream->event)
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    else
        stream->event = event;
    pulse_unlock();

    return hr;
}

static const struct unix_funcs unix_funcs =
{
    pulse_lock,
    pulse_unlock,
    pulse_cond_wait,
    pulse_main_loop,
    pulse_create_stream,
    pulse_release_stream,
    pulse_start,
    pulse_stop,
    pulse_reset,
    pulse_timer_loop,
    pulse_get_render_buffer,
    pulse_release_render_buffer,
    pulse_get_buffer_size,
    pulse_set_volumes,
    pulse_set_event_handle,
    pulse_test_connect,
};

NTSTATUS CDECL __wine_init_unix_lib(HMODULE module, DWORD reason, const void *ptr_in, void *ptr_out)
{
    pthread_mutexattr_t attr;

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);

        if (pthread_mutex_init(&pulse_mutex, &attr) != 0)
            pthread_mutex_init(&pulse_mutex, NULL);

        *(const struct unix_funcs **)ptr_out = &unix_funcs;
        break;
    case DLL_PROCESS_DETACH:
        if (pulse_ctx)
        {
            pa_context_disconnect(pulse_ctx);
            pa_context_unref(pulse_ctx);
        }
        if (pulse_ml)
            pa_mainloop_quit(pulse_ml, 0);

    }

    return STATUS_SUCCESS;
}
