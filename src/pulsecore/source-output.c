/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/utf8.h>
#include <pulse/xmalloc.h>
#include <pulse/util.h>
#include <pulse/internal.h>

#include <pulsecore/mix.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>

#include "source-output.h"

#define MEMBLOCKQ_MAXLENGTH (32*1024*1024)

PA_DEFINE_PUBLIC_CLASS(pa_source_output, pa_msgobject);

static void source_output_free(pa_object* mo);
static void set_real_ratio(pa_source_output *o, const pa_cvolume *v);

pa_source_output_new_data* pa_source_output_new_data_init(pa_source_output_new_data *data) {
    pa_assert(data);

    pa_zero(*data);
    data->resample_method = PA_RESAMPLER_INVALID;
    data->proplist = pa_proplist_new();
    data->volume_writable = TRUE;

    return data;
}

void pa_source_output_new_data_set_sample_spec(pa_source_output_new_data *data, const pa_sample_spec *spec) {
    pa_assert(data);

    if ((data->sample_spec_is_set = !!spec))
        data->sample_spec = *spec;
}

void pa_source_output_new_data_set_channel_map(pa_source_output_new_data *data, const pa_channel_map *map) {
    pa_assert(data);

    if ((data->channel_map_is_set = !!map))
        data->channel_map = *map;
}

pa_bool_t pa_source_output_new_data_is_passthrough(pa_source_output_new_data *data) {
    pa_assert(data);

    if (PA_LIKELY(data->format) && PA_UNLIKELY(!pa_format_info_is_pcm(data->format)))
        return TRUE;

    if (PA_UNLIKELY(data->flags & PA_SOURCE_OUTPUT_PASSTHROUGH))
        return TRUE;

    return FALSE;
}

void pa_source_output_new_data_set_volume(pa_source_output_new_data *data, const pa_cvolume *volume) {
    pa_assert(data);
    pa_assert(data->volume_writable);

    if ((data->volume_is_set = !!volume))
        data->volume = *volume;
}

void pa_source_output_new_data_apply_volume_factor(pa_source_output_new_data *data, const pa_cvolume *volume_factor) {
    pa_assert(data);
    pa_assert(volume_factor);

    if (data->volume_factor_is_set)
        pa_sw_cvolume_multiply(&data->volume_factor, &data->volume_factor, volume_factor);
    else {
        data->volume_factor_is_set = TRUE;
        data->volume_factor = *volume_factor;
    }
}

void pa_source_output_new_data_apply_volume_factor_source(pa_source_output_new_data *data, const pa_cvolume *volume_factor) {
    pa_assert(data);
    pa_assert(volume_factor);

    if (data->volume_factor_source_is_set)
        pa_sw_cvolume_multiply(&data->volume_factor_source, &data->volume_factor_source, volume_factor);
    else {
        data->volume_factor_source_is_set = TRUE;
        data->volume_factor_source = *volume_factor;
    }
}

void pa_source_output_new_data_set_muted(pa_source_output_new_data *data, pa_bool_t mute) {
    pa_assert(data);

    data->muted_is_set = TRUE;
    data->muted = !!mute;
}

pa_bool_t pa_source_output_new_data_set_source(pa_source_output_new_data *data, pa_source *s, pa_bool_t save) {
    pa_bool_t ret = TRUE;
    pa_idxset *formats = NULL;

    pa_assert(data);
    pa_assert(s);

    if (!data->req_formats) {
        /* We're not working with the extended API */
        data->source = s;
        data->save_source = save;
    } else {
        /* Extended API: let's see if this source supports the formats the client would like */
        formats = pa_source_check_formats(s, data->req_formats);

        if (formats && !pa_idxset_isempty(formats)) {
            /* Source supports at least one of the requested formats */
            data->source = s;
            data->save_source = save;
            if (data->nego_formats)
                pa_idxset_free(data->nego_formats, (pa_free_cb_t) pa_format_info_free);
            data->nego_formats = formats;
        } else {
            /* Source doesn't support any of the formats requested by the client */
            if (formats)
                pa_idxset_free(formats, (pa_free_cb_t) pa_format_info_free);
            ret = FALSE;
        }
    }

    return ret;
}

pa_bool_t pa_source_output_new_data_set_formats(pa_source_output_new_data *data, pa_idxset *formats) {
    pa_assert(data);
    pa_assert(formats);

    if (data->req_formats)
        pa_idxset_free(formats, (pa_free_cb_t) pa_format_info_free);

    data->req_formats = formats;

    if (data->source) {
        /* Trigger format negotiation */
        return pa_source_output_new_data_set_source(data, data->source, data->save_source);
    }

    return TRUE;
}

void pa_source_output_new_data_done(pa_source_output_new_data *data) {
    pa_assert(data);

    if (data->req_formats)
        pa_idxset_free(data->req_formats, (pa_free_cb_t) pa_format_info_free);

    if (data->nego_formats)
        pa_idxset_free(data->nego_formats, (pa_free_cb_t) pa_format_info_free);

    if (data->format)
        pa_format_info_free(data->format);

    pa_proplist_free(data->proplist);
}

/* Called from main context */
static void reset_callbacks(pa_source_output *o) {
    pa_assert(o);

    o->push = NULL;
    o->process_rewind = NULL;
    o->update_max_rewind = NULL;
    o->update_source_requested_latency = NULL;
    o->update_source_latency_range = NULL;
    o->update_source_fixed_latency = NULL;
    o->attach = NULL;
    o->detach = NULL;
    o->suspend = NULL;
    o->suspend_within_thread = NULL;
    o->moving = NULL;
    o->kill = NULL;
    o->get_latency = NULL;
    o->state_change = NULL;
    o->may_move_to = NULL;
    o->send_event = NULL;
    o->volume_changed = NULL;
    o->mute_changed = NULL;
}

/* Called from main context */
int pa_source_output_new(
        pa_source_output**_o,
        pa_core *core,
        pa_source_output_new_data *data) {

    pa_source_output *o;
    pa_resampler *resampler = NULL;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    pa_channel_map original_cm;
    int r;
    char *pt;
    pa_sample_spec ss;
    pa_channel_map map;

    pa_assert(_o);
    pa_assert(core);
    pa_assert(data);
    pa_assert_ctl_context();

    if (data->client)
        pa_proplist_update(data->proplist, PA_UPDATE_MERGE, data->client->proplist);

    if (data->destination_source && (data->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER))
        data->volume_writable = FALSE;

    if (!data->req_formats) {
        /* From this point on, we want to work only with formats, and get back
         * to using the sample spec and channel map after all decisions w.r.t.
         * routing are complete. */
        pa_idxset *tmp = pa_idxset_new(NULL, NULL);
        pa_format_info *f = pa_format_info_from_sample_spec(&data->sample_spec,
                data->channel_map_is_set ? &data->channel_map : NULL);
        pa_idxset_put(tmp, f, NULL);
        pa_source_output_new_data_set_formats(data, tmp);
    }

    if ((r = pa_hook_fire(&core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW], data)) < 0)
        return r;

    pa_return_val_if_fail(!data->driver || pa_utf8_valid(data->driver), -PA_ERR_INVALID);

    if (!data->source) {
        pa_source *source = pa_namereg_get(core, NULL, PA_NAMEREG_SOURCE);
        pa_return_val_if_fail(source, -PA_ERR_NOENTITY);
        pa_source_output_new_data_set_source(data, source, FALSE);
    }

    /* Routing's done, we have a source. Now let's fix the format and set up the
     * sample spec */

    /* If something didn't pick a format for us, pick the top-most format since
     * we assume this is sorted in priority order */
    if (!data->format && data->nego_formats && !pa_idxset_isempty(data->nego_formats))
        data->format = pa_format_info_copy(pa_idxset_first(data->nego_formats, NULL));

    pa_return_val_if_fail(data->format, -PA_ERR_NOTSUPPORTED);

    /* Now populate the sample spec and format according to the final
     * format that we've negotiated */
    pa_return_val_if_fail(pa_format_info_to_sample_spec(data->format, &ss, &map) == 0, -PA_ERR_INVALID);
    pa_source_output_new_data_set_sample_spec(data, &ss);
    if (pa_format_info_is_pcm(data->format) && pa_channel_map_valid(&map))
        pa_source_output_new_data_set_channel_map(data, &map);

    pa_return_val_if_fail(PA_SOURCE_IS_LINKED(pa_source_get_state(data->source)), -PA_ERR_BADSTATE);
    pa_return_val_if_fail(!data->direct_on_input || data->direct_on_input->sink == data->source->monitor_of, -PA_ERR_INVALID);

    if (!data->sample_spec_is_set)
        data->sample_spec = data->source->sample_spec;

    pa_return_val_if_fail(pa_sample_spec_valid(&data->sample_spec), -PA_ERR_INVALID);

    if (!data->channel_map_is_set) {
        if (pa_channel_map_compatible(&data->source->channel_map, &data->sample_spec))
            data->channel_map = data->source->channel_map;
        else
            pa_channel_map_init_extend(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
    }

    pa_return_val_if_fail(pa_channel_map_compatible(&data->channel_map, &data->sample_spec), -PA_ERR_INVALID);

    /* Don't restore (or save) stream volume for passthrough streams and
     * prevent attenuation/gain */
    if (pa_source_output_new_data_is_passthrough(data)) {
        data->volume_is_set = TRUE;
        pa_cvolume_reset(&data->volume, data->sample_spec.channels);
        data->volume_is_absolute = TRUE;
        data->save_volume = FALSE;
    }

    if (!data->volume_is_set) {
        pa_cvolume_reset(&data->volume, data->sample_spec.channels);
        data->volume_is_absolute = FALSE;
        data->save_volume = FALSE;
    }

    if (!data->volume_writable)
        data->save_volume = false;

    pa_return_val_if_fail(pa_cvolume_compatible(&data->volume, &data->sample_spec), -PA_ERR_INVALID);

    if (!data->volume_factor_is_set)
        pa_cvolume_reset(&data->volume_factor, data->sample_spec.channels);

    pa_return_val_if_fail(pa_cvolume_compatible(&data->volume_factor, &data->sample_spec), -PA_ERR_INVALID);

    if (!data->volume_factor_source_is_set)
        pa_cvolume_reset(&data->volume_factor_source, data->source->sample_spec.channels);

    pa_return_val_if_fail(pa_cvolume_compatible(&data->volume_factor_source, &data->source->sample_spec), -PA_ERR_INVALID);

    if (!data->muted_is_set)
        data->muted = FALSE;

    if (data->flags & PA_SOURCE_OUTPUT_FIX_FORMAT) {
        pa_return_val_if_fail(pa_format_info_is_pcm(data->format), -PA_ERR_INVALID);
        data->sample_spec.format = data->source->sample_spec.format;
        pa_format_info_set_sample_format(data->format, data->sample_spec.format);
    }

    if (data->flags & PA_SOURCE_OUTPUT_FIX_RATE) {
        pa_return_val_if_fail(pa_format_info_is_pcm(data->format), -PA_ERR_INVALID);
        pa_format_info_set_rate(data->format, data->sample_spec.rate);
        data->sample_spec.rate = data->source->sample_spec.rate;
    }

    original_cm = data->channel_map;

    if (data->flags & PA_SOURCE_OUTPUT_FIX_CHANNELS) {
        pa_return_val_if_fail(pa_format_info_is_pcm(data->format), -PA_ERR_INVALID);
        data->sample_spec.channels = data->source->sample_spec.channels;
        data->channel_map = data->source->channel_map;
        pa_format_info_set_channels(data->format, data->sample_spec.channels);
        pa_format_info_set_channel_map(data->format, &data->channel_map);
    }

    pa_assert(pa_sample_spec_valid(&data->sample_spec));
    pa_assert(pa_channel_map_valid(&data->channel_map));

    if (!(data->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) &&
        !pa_sample_spec_equal(&data->sample_spec, &data->source->sample_spec)){
        /* try to change source rate. This is done before the FIXATE hook since
           module-suspend-on-idle can resume a source */

        pa_log_info("Trying to change sample rate");
        if (pa_source_update_rate(data->source, data->sample_spec.rate, pa_source_output_new_data_is_passthrough(data)) == TRUE)
            pa_log_info("Rate changed to %u Hz", data->source->sample_spec.rate);
    }

    if (pa_source_output_new_data_is_passthrough(data) &&
        !pa_sample_spec_equal(&data->sample_spec, &data->source->sample_spec)) {
        /* rate update failed, or other parts of sample spec didn't match */

        pa_log_debug("Could not update source sample spec to match passthrough stream");
        return -PA_ERR_NOTSUPPORTED;
    }

    /* Due to the fixing of the sample spec the volume might not match anymore */
    pa_cvolume_remap(&data->volume, &original_cm, &data->channel_map);

    if (data->resample_method == PA_RESAMPLER_INVALID)
        data->resample_method = core->resample_method;

    pa_return_val_if_fail(data->resample_method < PA_RESAMPLER_MAX, -PA_ERR_INVALID);

    if ((r = pa_hook_fire(&core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_FIXATE], data)) < 0)
        return r;

    if ((data->flags & PA_SOURCE_OUTPUT_NO_CREATE_ON_SUSPEND) &&
        pa_source_get_state(data->source) == PA_SOURCE_SUSPENDED) {
        pa_log("Failed to create source output: source is suspended.");
        return -PA_ERR_BADSTATE;
    }

    if (pa_idxset_size(data->source->outputs) >= PA_MAX_OUTPUTS_PER_SOURCE) {
        pa_log("Failed to create source output: too many outputs per source.");
        return -PA_ERR_TOOLARGE;
    }

    if ((data->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) ||
        !pa_sample_spec_equal(&data->sample_spec, &data->source->sample_spec) ||
        !pa_channel_map_equal(&data->channel_map, &data->source->channel_map)) {

        if (!pa_source_output_new_data_is_passthrough(data)) /* no resampler for passthrough content */
            if (!(resampler = pa_resampler_new(
                        core->mempool,
                        &data->source->sample_spec, &data->source->channel_map,
                        &data->sample_spec, &data->channel_map,
                        data->resample_method,
                        ((data->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) ? PA_RESAMPLER_VARIABLE_RATE : 0) |
                        ((data->flags & PA_SOURCE_OUTPUT_NO_REMAP) ? PA_RESAMPLER_NO_REMAP : 0) |
                        (core->disable_remixing || (data->flags & PA_SOURCE_OUTPUT_NO_REMIX) ? PA_RESAMPLER_NO_REMIX : 0) |
                        (core->disable_lfe_remixing ? PA_RESAMPLER_NO_LFE : 0)))) {
                pa_log_warn("Unsupported resampling operation.");
                return -PA_ERR_NOTSUPPORTED;
            }
    }

    o = pa_msgobject_new(pa_source_output);
    o->parent.parent.free = source_output_free;
    o->parent.process_msg = pa_source_output_process_msg;

    o->core = core;
    o->state = PA_SOURCE_OUTPUT_INIT;
    o->flags = data->flags;
    o->proplist = pa_proplist_copy(data->proplist);
    o->driver = pa_xstrdup(pa_path_get_filename(data->driver));
    o->module = data->module;
    o->source = data->source;
    o->destination_source = data->destination_source;
    o->client = data->client;

    o->requested_resample_method = data->resample_method;
    o->actual_resample_method = resampler ? pa_resampler_get_method(resampler) : PA_RESAMPLER_INVALID;
    o->sample_spec = data->sample_spec;
    o->channel_map = data->channel_map;
    o->format = pa_format_info_copy(data->format);

    if (!data->volume_is_absolute && pa_source_flat_volume_enabled(o->source)) {
        pa_cvolume remapped;

        /* When the 'absolute' bool is not set then we'll treat the volume
         * as relative to the source volume even in flat volume mode */
        remapped = data->source->reference_volume;
        pa_cvolume_remap(&remapped, &data->source->channel_map, &data->channel_map);
        pa_sw_cvolume_multiply(&o->volume, &data->volume, &remapped);
    } else
        o->volume = data->volume;

    o->volume_factor = data->volume_factor;
    o->volume_factor_source = data->volume_factor_source;
    o->real_ratio = o->reference_ratio = data->volume;
    pa_cvolume_reset(&o->soft_volume, o->sample_spec.channels);
    pa_cvolume_reset(&o->real_ratio, o->sample_spec.channels);
    o->volume_writable = data->volume_writable;
    o->save_volume = data->save_volume;
    o->save_source = data->save_source;
    o->save_muted = data->save_muted;

    o->muted = data->muted;

    o->direct_on_input = data->direct_on_input;

    reset_callbacks(o);
    o->userdata = NULL;

    o->thread_info.state = o->state;
    o->thread_info.attached = FALSE;
    o->thread_info.sample_spec = o->sample_spec;
    o->thread_info.resampler = resampler;
    o->thread_info.soft_volume = o->soft_volume;
    o->thread_info.muted = o->muted;
    o->thread_info.requested_source_latency = (pa_usec_t) -1;
    o->thread_info.direct_on_input = o->direct_on_input;

    o->thread_info.delay_memblockq = pa_memblockq_new(
            "source output delay_memblockq",
            0,
            MEMBLOCKQ_MAXLENGTH,
            0,
            &o->source->sample_spec,
            0,
            1,
            0,
            &o->source->silence);

    pa_assert_se(pa_idxset_put(core->source_outputs, o, &o->index) == 0);
    pa_assert_se(pa_idxset_put(o->source->outputs, pa_source_output_ref(o), NULL) == 0);

    if (o->client)
        pa_assert_se(pa_idxset_put(o->client->source_outputs, o, NULL) >= 0);

    if (o->direct_on_input)
        pa_assert_se(pa_idxset_put(o->direct_on_input->direct_outputs, o, NULL) == 0);

    pt = pa_proplist_to_string_sep(o->proplist, "\n    ");
    pa_log_info("Created output %u \"%s\" on %s with sample spec %s and channel map %s\n    %s",
                o->index,
                pa_strnull(pa_proplist_gets(o->proplist, PA_PROP_MEDIA_NAME)),
                o->source->name,
                pa_sample_spec_snprint(st, sizeof(st), &o->sample_spec),
                pa_channel_map_snprint(cm, sizeof(cm), &o->channel_map),
                pt);
    pa_xfree(pt);

    /* Don't forget to call pa_source_output_put! */

    *_o = o;
    return 0;
}

/* Called from main context */
static void update_n_corked(pa_source_output *o, pa_source_output_state_t state) {
    pa_assert(o);
    pa_assert_ctl_context();

    if (!o->source)
        return;

    if (o->state == PA_SOURCE_OUTPUT_CORKED && state != PA_SOURCE_OUTPUT_CORKED)
        pa_assert_se(o->source->n_corked -- >= 1);
    else if (o->state != PA_SOURCE_OUTPUT_CORKED && state == PA_SOURCE_OUTPUT_CORKED)
        o->source->n_corked++;
}

/* Called from main context */
static void source_output_set_state(pa_source_output *o, pa_source_output_state_t state) {

    pa_assert(o);
    pa_assert_ctl_context();

    if (o->state == state)
        return;

    if (o->state == PA_SOURCE_OUTPUT_CORKED && state == PA_SOURCE_OUTPUT_RUNNING && pa_source_used_by(o->source) == 0 &&
        !pa_sample_spec_equal(&o->sample_spec, &o->source->sample_spec)) {
        /* We were uncorked and the source was not playing anything -- let's try
         * to update the sample rate to avoid resampling */
        pa_source_update_rate(o->source, o->sample_spec.rate, pa_source_output_is_passthrough(o));
    }

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), 0, NULL) == 0);

    update_n_corked(o, state);
    o->state = state;

    if (state != PA_SOURCE_OUTPUT_UNLINKED) {
        pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_STATE_CHANGED], o);

        if (PA_SOURCE_OUTPUT_IS_LINKED(state))
            pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
    }

    pa_source_update_status(o->source);
}

/* Called from main context */
void pa_source_output_unlink(pa_source_output*o) {
    pa_bool_t linked;
    pa_assert(o);
    pa_assert_ctl_context();

    /* See pa_sink_unlink() for a couple of comments how this function
     * works */

    pa_source_output_ref(o);

    linked = PA_SOURCE_OUTPUT_IS_LINKED(o->state);

    if (linked)
        pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK], o);

    if (o->direct_on_input)
        pa_idxset_remove_by_data(o->direct_on_input->direct_outputs, o, NULL);

    pa_idxset_remove_by_data(o->core->source_outputs, o, NULL);

    if (o->source)
        if (pa_idxset_remove_by_data(o->source->outputs, o, NULL))
            pa_source_output_unref(o);

    if (o->client)
        pa_idxset_remove_by_data(o->client->source_outputs, o, NULL);

    update_n_corked(o, PA_SOURCE_OUTPUT_UNLINKED);
    o->state = PA_SOURCE_OUTPUT_UNLINKED;

    if (linked && o->source) {
        if (pa_source_output_is_passthrough(o))
            pa_source_leave_passthrough(o->source);

        /* We might need to update the source's volume if we are in flat volume mode. */
        if (pa_source_flat_volume_enabled(o->source))
            pa_source_set_volume(o->source, NULL, FALSE, FALSE);

        if (o->source->asyncmsgq)
            pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_REMOVE_OUTPUT, o, 0, NULL) == 0);
    }

    reset_callbacks(o);

    if (linked) {
        pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_REMOVE, o->index);
        pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK_POST], o);
    }

    if (o->source) {
        if (PA_SOURCE_IS_LINKED(pa_source_get_state(o->source)))
            pa_source_update_status(o->source);

        o->source = NULL;
    }

    pa_core_maybe_vacuum(o->core);

    pa_source_output_unref(o);
}

/* Called from main context */
static void source_output_free(pa_object* mo) {
    pa_source_output *o = PA_SOURCE_OUTPUT(mo);

    pa_assert(o);
    pa_assert_ctl_context();
    pa_assert(pa_source_output_refcnt(o) == 0);

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state))
        pa_source_output_unlink(o);

    pa_log_info("Freeing output %u \"%s\"", o->index, pa_strnull(pa_proplist_gets(o->proplist, PA_PROP_MEDIA_NAME)));

    if (o->thread_info.delay_memblockq)
        pa_memblockq_free(o->thread_info.delay_memblockq);

    if (o->thread_info.resampler)
        pa_resampler_free(o->thread_info.resampler);

    if (o->format)
        pa_format_info_free(o->format);

    if (o->proplist)
        pa_proplist_free(o->proplist);

    pa_xfree(o->driver);
    pa_xfree(o);
}

/* Called from main context */
void pa_source_output_put(pa_source_output *o) {
    pa_source_output_state_t state;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    pa_assert(o->state == PA_SOURCE_OUTPUT_INIT);

    /* The following fields must be initialized properly */
    pa_assert(o->push);
    pa_assert(o->kill);

    state = o->flags & PA_SOURCE_OUTPUT_START_CORKED ? PA_SOURCE_OUTPUT_CORKED : PA_SOURCE_OUTPUT_RUNNING;

    update_n_corked(o, state);
    o->state = state;

    /* We might need to update the source's volume if we are in flat volume mode. */
    if (pa_source_flat_volume_enabled(o->source))
        pa_source_set_volume(o->source, NULL, FALSE, o->save_volume);
    else {
        if (o->destination_source && (o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER)) {
            pa_assert(pa_cvolume_is_norm(&o->volume));
            pa_assert(pa_cvolume_is_norm(&o->reference_ratio));
        }

        set_real_ratio(o, &o->volume);
    }

    if (pa_source_output_is_passthrough(o))
        pa_source_enter_passthrough(o->source);

    o->thread_info.soft_volume = o->soft_volume;
    o->thread_info.muted = o->muted;

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_ADD_OUTPUT, o, 0, NULL) == 0);

    pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_NEW, o->index);
    pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PUT], o);

    pa_source_update_status(o->source);
}

/* Called from main context */
void pa_source_output_kill(pa_source_output*o) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));

    o->kill(o);
}

/* Called from main context */
pa_usec_t pa_source_output_get_latency(pa_source_output *o, pa_usec_t *source_latency) {
    pa_usec_t r[2] = { 0, 0 };

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_GET_LATENCY, r, 0, NULL) == 0);

    if (o->get_latency)
        r[0] += o->get_latency(o);

    if (source_latency)
        *source_latency = r[1];

    return r[0];
}

/* Called from thread context */
void pa_source_output_push(pa_source_output *o, const pa_memchunk *chunk) {
    pa_bool_t need_volume_factor_source;
    pa_bool_t volume_is_norm;
    size_t length;
    size_t limit, mbs = 0;

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state));
    pa_assert(chunk);
    pa_assert(pa_frame_aligned(chunk->length, &o->source->sample_spec));

    if (!o->push || o->thread_info.state == PA_SOURCE_OUTPUT_CORKED)
        return;

    pa_assert(o->thread_info.state == PA_SOURCE_OUTPUT_RUNNING);

    if (pa_memblockq_push(o->thread_info.delay_memblockq, chunk) < 0) {
        pa_log_debug("Delay queue overflow!");
        pa_memblockq_seek(o->thread_info.delay_memblockq, (int64_t) chunk->length, PA_SEEK_RELATIVE, TRUE);
    }

    limit = o->process_rewind ? 0 : o->source->thread_info.max_rewind;

    volume_is_norm = pa_cvolume_is_norm(&o->thread_info.soft_volume) && !o->thread_info.muted;
    need_volume_factor_source = !pa_cvolume_is_norm(&o->volume_factor_source);

    if (limit > 0 && o->source->monitor_of) {
        pa_usec_t latency;
        size_t n;

        /* Hmm, check the latency for knowing how much of the buffered
         * data is actually still unplayed and might hence still
         * change. This is suboptimal. Ideally we'd have a call like
         * pa_sink_get_changeable_size() or so that tells us how much
         * of the queued data is actually still changeable. Hence
         * FIXME! */

        latency = pa_sink_get_latency_within_thread(o->source->monitor_of);

        n = pa_usec_to_bytes(latency, &o->source->sample_spec);

        if (n < limit)
            limit = n;
    }

    /* Implement the delay queue */
    while ((length = pa_memblockq_get_length(o->thread_info.delay_memblockq)) > limit) {
        pa_memchunk qchunk;
        pa_bool_t nvfs = need_volume_factor_source;

        length -= limit;

        pa_assert_se(pa_memblockq_peek(o->thread_info.delay_memblockq, &qchunk) >= 0);

        if (qchunk.length > length)
            qchunk.length = length;

        pa_assert(qchunk.length > 0);

        /* It might be necessary to adjust the volume here */
        if (!volume_is_norm) {
            pa_memchunk_make_writable(&qchunk, 0);

            if (o->thread_info.muted) {
                pa_silence_memchunk(&qchunk, &o->source->sample_spec);
                nvfs = FALSE;

            } else if (!o->thread_info.resampler && nvfs) {
                pa_cvolume v;

                /* If we don't need a resampler we can merge the
                 * post and the pre volume adjustment into one */

                pa_sw_cvolume_multiply(&v, &o->thread_info.soft_volume, &o->volume_factor_source);
                pa_volume_memchunk(&qchunk, &o->source->sample_spec, &v);
                nvfs = FALSE;

            } else
                pa_volume_memchunk(&qchunk, &o->source->sample_spec, &o->thread_info.soft_volume);
        }

        if (!o->thread_info.resampler) {
            if (nvfs) {
                pa_memchunk_make_writable(&qchunk, 0);
                pa_volume_memchunk(&qchunk, &o->thread_info.sample_spec, &o->volume_factor_source);
            }

            o->push(o, &qchunk);
        } else {
            pa_memchunk rchunk;

            if (mbs == 0)
                mbs = pa_resampler_max_block_size(o->thread_info.resampler);

            if (qchunk.length > mbs)
                qchunk.length = mbs;

            pa_resampler_run(o->thread_info.resampler, &qchunk, &rchunk);

            if (rchunk.length > 0) {
                if (nvfs) {
                    pa_memchunk_make_writable(&rchunk, 0);
                    pa_volume_memchunk(&rchunk, &o->thread_info.sample_spec, &o->volume_factor_source);
                }

                o->push(o, &rchunk);
            }

            if (rchunk.memblock)
                pa_memblock_unref(rchunk.memblock);
        }

        pa_memblock_unref(qchunk.memblock);
        pa_memblockq_drop(o->thread_info.delay_memblockq, qchunk.length);
    }
}

/* Called from thread context */
void pa_source_output_process_rewind(pa_source_output *o, size_t nbytes /* in source sample spec */) {

    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &o->source->sample_spec));

    if (nbytes <= 0)
        return;

    if (o->process_rewind) {
        pa_assert(pa_memblockq_get_length(o->thread_info.delay_memblockq) == 0);

        if (o->thread_info.resampler)
            nbytes = pa_resampler_result(o->thread_info.resampler, nbytes);

        pa_log_debug("Have to rewind %lu bytes on implementor.", (unsigned long) nbytes);

        if (nbytes > 0)
            o->process_rewind(o, nbytes);

        if (o->thread_info.resampler)
            pa_resampler_reset(o->thread_info.resampler);

    } else
        pa_memblockq_rewind(o->thread_info.delay_memblockq, nbytes);
}

/* Called from thread context */
size_t pa_source_output_get_max_rewind(pa_source_output *o) {
    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);

    return o->thread_info.resampler ? pa_resampler_request(o->thread_info.resampler, o->source->thread_info.max_rewind) : o->source->thread_info.max_rewind;
}

/* Called from thread context */
void pa_source_output_update_max_rewind(pa_source_output *o, size_t nbytes  /* in the source's sample spec */) {
    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->thread_info.state));
    pa_assert(pa_frame_aligned(nbytes, &o->source->sample_spec));

    if (o->update_max_rewind)
        o->update_max_rewind(o, o->thread_info.resampler ? pa_resampler_result(o->thread_info.resampler, nbytes) : nbytes);
}

/* Called from thread context */
pa_usec_t pa_source_output_set_requested_latency_within_thread(pa_source_output *o, pa_usec_t usec) {
    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);

    if (!(o->source->flags & PA_SOURCE_DYNAMIC_LATENCY))
        usec = o->source->thread_info.fixed_latency;

    if (usec != (pa_usec_t) -1)
        usec = PA_CLAMP(usec, o->source->thread_info.min_latency, o->source->thread_info.max_latency);

    o->thread_info.requested_source_latency = usec;
    pa_source_invalidate_requested_latency(o->source, TRUE);

    return usec;
}

/* Called from main context */
pa_usec_t pa_source_output_set_requested_latency(pa_source_output *o, pa_usec_t usec) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state) && o->source) {
        pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_SET_REQUESTED_LATENCY, &usec, 0, NULL) == 0);
        return usec;
    }

    /* If this source output is not realized yet or is being moved, we
     * have to touch the thread info data directly */

    if (o->source) {
        if (!(o->source->flags & PA_SOURCE_DYNAMIC_LATENCY))
            usec = pa_source_get_fixed_latency(o->source);

        if (usec != (pa_usec_t) -1) {
            pa_usec_t min_latency, max_latency;
            pa_source_get_latency_range(o->source, &min_latency, &max_latency);
            usec = PA_CLAMP(usec, min_latency, max_latency);
        }
    }

    o->thread_info.requested_source_latency = usec;

    return usec;
}

/* Called from main context */
pa_usec_t pa_source_output_get_requested_latency(pa_source_output *o) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state) && o->source) {
        pa_usec_t usec = 0;
        pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_GET_REQUESTED_LATENCY, &usec, 0, NULL) == 0);
        return usec;
    }

    /* If this source output is not realized yet or is being moved, we
     * have to touch the thread info data directly */

    return o->thread_info.requested_source_latency;
}

/* Called from main context */
void pa_source_output_set_volume(pa_source_output *o, const pa_cvolume *volume, pa_bool_t save, pa_bool_t absolute) {
    pa_cvolume v;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(volume);
    pa_assert(pa_cvolume_valid(volume));
    pa_assert(volume->channels == 1 || pa_cvolume_compatible(volume, &o->sample_spec));
    pa_assert(o->volume_writable);

    if (!absolute && pa_source_flat_volume_enabled(o->source)) {
        v = o->source->reference_volume;
        pa_cvolume_remap(&v, &o->source->channel_map, &o->channel_map);

        if (pa_cvolume_compatible(volume, &o->sample_spec))
            volume = pa_sw_cvolume_multiply(&v, &v, volume);
        else
            volume = pa_sw_cvolume_multiply_scalar(&v, &v, pa_cvolume_max(volume));
    } else {
        if (!pa_cvolume_compatible(volume, &o->sample_spec)) {
            v = o->volume;
            volume = pa_cvolume_scale(&v, pa_cvolume_max(volume));
        }
    }

    if (pa_cvolume_equal(volume, &o->volume)) {
        o->save_volume = o->save_volume || save;
        return;
    }

    o->volume = *volume;
    o->save_volume = save;

    if (pa_source_flat_volume_enabled(o->source)) {
        /* We are in flat volume mode, so let's update all source input
         * volumes and update the flat volume of the source */

        pa_source_set_volume(o->source, NULL, TRUE, save);

    } else {
        /* OK, we are in normal volume mode. The volume only affects
         * ourselves */
        set_real_ratio(o, volume);

        /* Copy the new soft_volume to the thread_info struct */
        pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_SET_SOFT_VOLUME, NULL, 0, NULL) == 0);
    }

    /* The volume changed, let's tell people so */
    if (o->volume_changed)
        o->volume_changed(o);

    /* The virtual volume changed, let's tell people so */
    pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
}

/* Called from main context */
static void set_real_ratio(pa_source_output *o, const pa_cvolume *v) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(!v || pa_cvolume_compatible(v, &o->sample_spec));

    /* This basically calculates:
     *
     * o->real_ratio := v
     * o->soft_volume := o->real_ratio * o->volume_factor */

    if (v)
        o->real_ratio = *v;
    else
        pa_cvolume_reset(&o->real_ratio, o->sample_spec.channels);

    pa_sw_cvolume_multiply(&o->soft_volume, &o->real_ratio, &o->volume_factor);
    /* We don't copy the data to the thread_info data. That's left for someone else to do */
}

/* Called from main or I/O context */
pa_bool_t pa_source_output_is_passthrough(pa_source_output *o) {
    pa_source_output_assert_ref(o);

    if (PA_UNLIKELY(!pa_format_info_is_pcm(o->format)))
        return TRUE;

    if (PA_UNLIKELY(o->flags & PA_SOURCE_OUTPUT_PASSTHROUGH))
        return TRUE;

    return FALSE;
}

/* Called from main context */
pa_bool_t pa_source_output_is_volume_readable(pa_source_output *o) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    return !pa_source_output_is_passthrough(o);
}

/* Called from main context */
pa_cvolume *pa_source_output_get_volume(pa_source_output *o, pa_cvolume *volume, pa_bool_t absolute) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(pa_source_output_is_volume_readable(o));

    if (absolute || !pa_source_flat_volume_enabled(o->source))
        *volume = o->volume;
    else
        *volume = o->reference_ratio;

    return volume;
}

/* Called from main context */
void pa_source_output_set_mute(pa_source_output *o, pa_bool_t mute, pa_bool_t save) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));

    if (!o->muted == !mute) {
        o->save_muted = o->save_muted || mute;
        return;
    }

    o->muted = mute;
    o->save_muted = save;

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_SET_SOFT_MUTE, NULL, 0, NULL) == 0);

    /* The mute status changed, let's tell people so */
    if (o->mute_changed)
        o->mute_changed(o);

    pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
}

/* Called from main context */
pa_bool_t pa_source_output_get_mute(pa_source_output *o) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));

    return o->muted;
}

/* Called from main thread */
void pa_source_output_update_proplist(pa_source_output *o, pa_update_mode_t mode, pa_proplist *p) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    if (p)
        pa_proplist_update(o->proplist, mode, p);

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state)) {
        pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PROPLIST_CHANGED], o);
        pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
    }
}

/* Called from main context */
void pa_source_output_cork(pa_source_output *o, pa_bool_t b) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));

    source_output_set_state(o, b ? PA_SOURCE_OUTPUT_CORKED : PA_SOURCE_OUTPUT_RUNNING);
}

/* Called from main context */
int pa_source_output_set_rate(pa_source_output *o, uint32_t rate) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_return_val_if_fail(o->thread_info.resampler, -PA_ERR_BADSTATE);

    if (o->sample_spec.rate == rate)
        return 0;

    o->sample_spec.rate = rate;

    pa_asyncmsgq_post(o->source->asyncmsgq, PA_MSGOBJECT(o), PA_SOURCE_OUTPUT_MESSAGE_SET_RATE, PA_UINT_TO_PTR(rate), 0, NULL, NULL);

    pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
    return 0;
}

/* Called from main context */
void pa_source_output_set_name(pa_source_output *o, const char *name) {
    const char *old;
    pa_assert_ctl_context();
    pa_source_output_assert_ref(o);

    if (!name && !pa_proplist_contains(o->proplist, PA_PROP_MEDIA_NAME))
        return;

    old = pa_proplist_gets(o->proplist, PA_PROP_MEDIA_NAME);

    if (old && name && pa_streq(old, name))
        return;

    if (name)
        pa_proplist_sets(o->proplist, PA_PROP_MEDIA_NAME, name);
    else
        pa_proplist_unset(o->proplist, PA_PROP_MEDIA_NAME);

    if (PA_SOURCE_OUTPUT_IS_LINKED(o->state)) {
        pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PROPLIST_CHANGED], o);
        pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
    }
}

/* Called from main context */
pa_resample_method_t pa_source_output_get_resample_method(pa_source_output *o) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    return o->actual_resample_method;
}

/* Called from main context */
pa_bool_t pa_source_output_may_move(pa_source_output *o) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));

    if (o->flags & PA_SOURCE_OUTPUT_DONT_MOVE)
        return FALSE;

    if (o->direct_on_input)
        return FALSE;

    return TRUE;
}

static pa_bool_t find_filter_source_output(pa_source_output *target, pa_source *s) {
    int i = 0;
    while (s && s->output_from_master) {
        if (s->output_from_master == target)
            return TRUE;
        s = s->output_from_master->source;
        pa_assert(i++ < 100);
    }
    return FALSE;
}

/* Called from main context */
pa_bool_t pa_source_output_may_move_to(pa_source_output *o, pa_source *dest) {
    pa_source_output_assert_ref(o);
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_source_assert_ref(dest);

    if (dest == o->source)
        return TRUE;

    if (!pa_source_output_may_move(o))
        return FALSE;

    /* Make sure we're not creating a filter source cycle */
    if (find_filter_source_output(o, dest)) {
        pa_log_debug("Can't connect output to %s, as that would create a cycle.", dest->name);
        return FALSE;
    }

    if (pa_idxset_size(dest->outputs) >= PA_MAX_OUTPUTS_PER_SOURCE) {
        pa_log_warn("Failed to move source output: too many outputs per source.");
        return FALSE;
    }

    if (o->may_move_to)
        if (!o->may_move_to(o, dest))
            return FALSE;

    return TRUE;
}

/* Called from main context */
int pa_source_output_start_move(pa_source_output *o) {
    pa_source *origin;
    int r;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(o->source);

    if (!pa_source_output_may_move(o))
        return -PA_ERR_NOTSUPPORTED;

    if ((r = pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_START], o)) < 0)
        return r;

    origin = o->source;

    pa_idxset_remove_by_data(o->source->outputs, o, NULL);

    if (pa_source_output_get_state(o) == PA_SOURCE_OUTPUT_CORKED)
        pa_assert_se(origin->n_corked-- >= 1);

    if (pa_source_output_is_passthrough(o))
        pa_source_leave_passthrough(o->source);

    if (pa_source_flat_volume_enabled(o->source))
        /* We might need to update the source's volume if we are in flat
         * volume mode. */
        pa_source_set_volume(o->source, NULL, FALSE, FALSE);

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_REMOVE_OUTPUT, o, 0, NULL) == 0);

    pa_source_update_status(o->source);
    o->source = NULL;

    pa_source_output_unref(o);

    return 0;
}

/* Called from main context. If it has an origin source that uses volume sharing,
 * then also the origin source and all streams connected to it need to update
 * their volume - this function does all that by using recursion. */
static void update_volume_due_to_moving(pa_source_output *o, pa_source *dest) {
    pa_cvolume old_volume;

    pa_assert(o);
    pa_assert(dest);
    pa_assert(o->source); /* The destination source should already be set. */

    if (o->destination_source && (o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER)) {
        pa_source *root_source;
        pa_source_output *destination_source_output;
        uint32_t idx;

        root_source = pa_source_get_master(o->source);

        if (PA_UNLIKELY(!root_source))
            return;

        if (pa_source_flat_volume_enabled(o->source)) {
            /* Ok, so the origin source uses volume sharing, and flat volume is
             * enabled. The volume will have to be updated as follows:
             *
             *     o->volume := o->source->real_volume
             *         (handled later by pa_source_set_volume)
             *     o->reference_ratio := o->volume / o->source->reference_volume
             *         (handled later by pa_source_set_volume)
             *     o->real_ratio stays unchanged
             *         (streams whose origin source uses volume sharing should
             *          always have real_ratio of 0 dB)
             *     o->soft_volume stays unchanged
             *         (streams whose origin source uses volume sharing should
             *          always have volume_factor as soft_volume, so no change
             *          should be needed) */

            pa_assert(pa_cvolume_is_norm(&o->real_ratio));
            pa_assert(pa_cvolume_equal(&o->soft_volume, &o->volume_factor));

            /* Notifications will be sent by pa_source_set_volume(). */

        } else {
            /* Ok, so the origin source uses volume sharing, and flat volume is
             * disabled. The volume will have to be updated as follows:
             *
             *     o->volume := 0 dB
             *     o->reference_ratio := 0 dB
             *     o->real_ratio stays unchanged
             *         (streams whose origin source uses volume sharing should
             *          always have real_ratio of 0 dB)
             *     o->soft_volume stays unchanged
             *         (streams whose origin source uses volume sharing should
             *          always have volume_factor as soft_volume, so no change
             *          should be needed) */

            old_volume = o->volume;
            pa_cvolume_reset(&o->volume, o->volume.channels);
            pa_cvolume_reset(&o->reference_ratio, o->reference_ratio.channels);
            pa_assert(pa_cvolume_is_norm(&o->real_ratio));
            pa_assert(pa_cvolume_equal(&o->soft_volume, &o->volume_factor));

            /* Notify others about the changed source output volume. */
            if (!pa_cvolume_equal(&o->volume, &old_volume)) {
                if (o->volume_changed)
                    o->volume_changed(o);

                pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
            }
        }

        /* Additionally, the origin source volume needs updating:
         *
         *     o->destination_source->reference_volume := root_source->reference_volume
         *     o->destination_source->real_volume := root_source->real_volume
         *     o->destination_source->soft_volume stays unchanged
         *         (sources that use volume sharing should always have
         *          soft_volume of 0 dB) */

        old_volume = o->destination_source->reference_volume;

        o->destination_source->reference_volume = root_source->reference_volume;
        pa_cvolume_remap(&o->destination_source->reference_volume, &root_source->channel_map, &o->destination_source->channel_map);

        o->destination_source->real_volume = root_source->real_volume;
        pa_cvolume_remap(&o->destination_source->real_volume, &root_source->channel_map, &o->destination_source->channel_map);

        pa_assert(pa_cvolume_is_norm(&o->destination_source->soft_volume));

        /* Notify others about the changed source volume. If you wonder whether
         * o->destination_source->set_volume() should be called somewhere, that's not
         * the case, because sources that use volume sharing shouldn't have any
         * internal volume that set_volume() would update. If you wonder
         * whether the thread_info variables should be synced, yes, they
         * should, and it's done by the PA_SOURCE_MESSAGE_FINISH_MOVE message
         * handler. */
        if (!pa_cvolume_equal(&o->destination_source->reference_volume, &old_volume))
            pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, o->destination_source->index);

        /* Recursively update origin source outputs. */
        PA_IDXSET_FOREACH(destination_source_output, o->destination_source->outputs, idx)
            update_volume_due_to_moving(destination_source_output, dest);

    } else {
        old_volume = o->volume;

        if (pa_source_flat_volume_enabled(o->source)) {
            /* Ok, so this is a regular stream, and flat volume is enabled. The
             * volume will have to be updated as follows:
             *
             *     o->volume := o->reference_ratio * o->source->reference_volume
             *     o->reference_ratio stays unchanged
             *     o->real_ratio := o->volume / o->source->real_volume
             *         (handled later by pa_source_set_volume)
             *     o->soft_volume := o->real_ratio * o->volume_factor
             *         (handled later by pa_source_set_volume) */

            o->volume = o->source->reference_volume;
            pa_cvolume_remap(&o->volume, &o->source->channel_map, &o->channel_map);
            pa_sw_cvolume_multiply(&o->volume, &o->volume, &o->reference_ratio);

        } else {
            /* Ok, so this is a regular stream, and flat volume is disabled.
             * The volume will have to be updated as follows:
             *
             *     o->volume := o->reference_ratio
             *     o->reference_ratio stays unchanged
             *     o->real_ratio := o->reference_ratio
             *     o->soft_volume := o->real_ratio * o->volume_factor */

            o->volume = o->reference_ratio;
            o->real_ratio = o->reference_ratio;
            pa_sw_cvolume_multiply(&o->soft_volume, &o->real_ratio, &o->volume_factor);
        }

        /* Notify others about the changed source output volume. */
        if (!pa_cvolume_equal(&o->volume, &old_volume)) {
            /* XXX: In case o->source has flat volume enabled, then real_ratio
             * and soft_volume are not updated yet. Let's hope that the
             * callback implementation doesn't care about those variables... */
            if (o->volume_changed)
                o->volume_changed(o);

            pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
        }
    }

    /* If o->source == dest, then recursion has finished, and we can finally call
     * pa_source_set_volume(), which will do the rest of the updates. */
    if ((o->source == dest) && pa_source_flat_volume_enabled(o->source))
        pa_source_set_volume(o->source, NULL, FALSE, o->save_volume);
}

/* Called from main context */
int pa_source_output_finish_move(pa_source_output *o, pa_source *dest, pa_bool_t save) {
    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(!o->source);
    pa_source_assert_ref(dest);

    if (!pa_source_output_may_move_to(o, dest))
        return -PA_ERR_NOTSUPPORTED;

    if (pa_source_output_is_passthrough(o) && !pa_source_check_format(dest, o->format)) {
        pa_proplist *p = pa_proplist_new();
        pa_log_debug("New source doesn't support stream format, sending format-changed and killing");
        /* Tell the client what device we want to be on if it is going to
         * reconnect */
        pa_proplist_sets(p, "device", dest->name);
        pa_source_output_send_event(o, PA_STREAM_EVENT_FORMAT_LOST, p);
        pa_proplist_free(p);
        return -PA_ERR_NOTSUPPORTED;
    }

    if (!(o->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) &&
        !pa_sample_spec_equal(&o->sample_spec, &dest->sample_spec)){
        /* try to change dest sink rate if possible without glitches.
           module-suspend-on-idle resumes destination source with
           SOURCE_OUTPUT_MOVE_FINISH hook */

        pa_log_info("Trying to change sample rate");
        if (pa_source_update_rate(dest, o->sample_spec.rate, pa_source_output_is_passthrough(o)) == TRUE)
            pa_log_info("Rate changed to %u Hz", dest->sample_spec.rate);
    }

    if (o->moving)
        o->moving(o, dest);

    o->source = dest;
    o->save_source = save;
    pa_idxset_put(o->source->outputs, pa_source_output_ref(o), NULL);

    pa_cvolume_remap(&o->volume_factor_source, &o->channel_map, &o->source->channel_map);

    if (pa_source_output_get_state(o) == PA_SOURCE_OUTPUT_CORKED)
        o->source->n_corked++;

    pa_source_output_update_rate(o);

    pa_source_update_status(dest);

    update_volume_due_to_moving(o, dest);

    if (pa_source_output_is_passthrough(o))
        pa_source_enter_passthrough(o->source);

    pa_assert_se(pa_asyncmsgq_send(o->source->asyncmsgq, PA_MSGOBJECT(o->source), PA_SOURCE_MESSAGE_ADD_OUTPUT, o, 0, NULL) == 0);

    pa_log_debug("Successfully moved source output %i to %s.", o->index, dest->name);

    /* Notify everyone */
    pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_FINISH], o);
    pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);

    return 0;
}

/* Called from main context */
void pa_source_output_fail_move(pa_source_output *o) {

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(!o->source);

    /* Check if someone wants this source output? */
    if (pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_FAIL], o) == PA_HOOK_STOP)
        return;

    if (o->moving)
        o->moving(o, NULL);

    pa_source_output_kill(o);
}

/* Called from main context */
int pa_source_output_move_to(pa_source_output *o, pa_source *dest, pa_bool_t save) {
    int r;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_OUTPUT_IS_LINKED(o->state));
    pa_assert(o->source);
    pa_source_assert_ref(dest);

    if (dest == o->source)
        return 0;

    if (!pa_source_output_may_move_to(o, dest))
        return -PA_ERR_NOTSUPPORTED;

    pa_source_output_ref(o);

    if ((r = pa_source_output_start_move(o)) < 0) {
        pa_source_output_unref(o);
        return r;
    }

    if ((r = pa_source_output_finish_move(o, dest, save)) < 0) {
        pa_source_output_fail_move(o);
        pa_source_output_unref(o);
        return r;
    }

    pa_source_output_unref(o);

    return 0;
}

/* Called from IO thread context */
void pa_source_output_set_state_within_thread(pa_source_output *o, pa_source_output_state_t state) {
    pa_source_output_assert_ref(o);
    pa_source_output_assert_io_context(o);

    if (state == o->thread_info.state)
        return;

    if (o->state_change)
        o->state_change(o, state);

    o->thread_info.state = state;
}

/* Called from IO thread context, except when it is not */
int pa_source_output_process_msg(pa_msgobject *mo, int code, void *userdata, int64_t offset, pa_memchunk* chunk) {
    pa_source_output *o = PA_SOURCE_OUTPUT(mo);
    pa_source_output_assert_ref(o);

    switch (code) {

        case PA_SOURCE_OUTPUT_MESSAGE_GET_LATENCY: {
            pa_usec_t *r = userdata;

            r[0] += pa_bytes_to_usec(pa_memblockq_get_length(o->thread_info.delay_memblockq), &o->source->sample_spec);
            r[1] += pa_source_get_latency_within_thread(o->source);

            return 0;
        }

        case PA_SOURCE_OUTPUT_MESSAGE_SET_RATE:

            o->thread_info.sample_spec.rate = PA_PTR_TO_UINT(userdata);
            pa_resampler_set_output_rate(o->thread_info.resampler, PA_PTR_TO_UINT(userdata));
            return 0;

        case PA_SOURCE_OUTPUT_MESSAGE_SET_STATE:

            pa_source_output_set_state_within_thread(o, PA_PTR_TO_UINT(userdata));

            return 0;

        case PA_SOURCE_OUTPUT_MESSAGE_SET_REQUESTED_LATENCY: {
            pa_usec_t *usec = userdata;

            *usec = pa_source_output_set_requested_latency_within_thread(o, *usec);

            return 0;
        }

        case PA_SOURCE_OUTPUT_MESSAGE_GET_REQUESTED_LATENCY: {
            pa_usec_t *r = userdata;

            *r = o->thread_info.requested_source_latency;
            return 0;
        }

        case PA_SOURCE_OUTPUT_MESSAGE_SET_SOFT_VOLUME:
            if (!pa_cvolume_equal(&o->thread_info.soft_volume, &o->soft_volume)) {
                o->thread_info.soft_volume = o->soft_volume;
            }
            return 0;

        case PA_SOURCE_OUTPUT_MESSAGE_SET_SOFT_MUTE:
            if (o->thread_info.muted != o->muted) {
                o->thread_info.muted = o->muted;
            }
            return 0;
    }

    return -PA_ERR_NOTIMPLEMENTED;
}

/* Called from main context */
void pa_source_output_send_event(pa_source_output *o, const char *event, pa_proplist *data) {
    pa_proplist *pl = NULL;
    pa_source_output_send_event_hook_data hook_data;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();
    pa_assert(event);

    if (!o->send_event)
        return;

    if (!data)
        data = pl = pa_proplist_new();

    hook_data.source_output = o;
    hook_data.data = data;
    hook_data.event = event;

    if (pa_hook_fire(&o->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_SEND_EVENT], &hook_data) < 0)
        goto finish;

    o->send_event(o, event, data);

finish:
    if (pl)
        pa_proplist_free(pl);
}

/* Called from main context */
/* Updates the source output's resampler with whatever the current source
 * requires -- useful when the underlying source's rate might have changed */
int pa_source_output_update_rate(pa_source_output *o) {
    pa_resampler *new_resampler;
    char *memblockq_name;

    pa_source_output_assert_ref(o);
    pa_assert_ctl_context();

    if (o->thread_info.resampler &&
        pa_sample_spec_equal(pa_resampler_input_sample_spec(o->thread_info.resampler), &o->source->sample_spec) &&
        pa_channel_map_equal(pa_resampler_input_channel_map(o->thread_info.resampler), &o->source->channel_map))

        new_resampler = o->thread_info.resampler;

    else if (!pa_source_output_is_passthrough(o) &&
        ((o->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) ||
         !pa_sample_spec_equal(&o->sample_spec, &o->source->sample_spec) ||
         !pa_channel_map_equal(&o->channel_map, &o->source->channel_map))) {

        new_resampler = pa_resampler_new(o->core->mempool,
                                     &o->source->sample_spec, &o->source->channel_map,
                                     &o->sample_spec, &o->channel_map,
                                     o->requested_resample_method,
                                     ((o->flags & PA_SOURCE_OUTPUT_VARIABLE_RATE) ? PA_RESAMPLER_VARIABLE_RATE : 0) |
                                     ((o->flags & PA_SOURCE_OUTPUT_NO_REMAP) ? PA_RESAMPLER_NO_REMAP : 0) |
                                     (o->core->disable_remixing || (o->flags & PA_SOURCE_OUTPUT_NO_REMIX) ? PA_RESAMPLER_NO_REMIX : 0));

        if (!new_resampler) {
            pa_log_warn("Unsupported resampling operation.");
            return -PA_ERR_NOTSUPPORTED;
        }
    } else
        new_resampler = NULL;

    if (new_resampler == o->thread_info.resampler)
        return 0;

    if (o->thread_info.resampler)
        pa_resampler_free(o->thread_info.resampler);

    o->thread_info.resampler = new_resampler;

    pa_memblockq_free(o->thread_info.delay_memblockq);

    memblockq_name = pa_sprintf_malloc("source output delay_memblockq [%u]", o->index);
    o->thread_info.delay_memblockq = pa_memblockq_new(
            memblockq_name,
            0,
            MEMBLOCKQ_MAXLENGTH,
            0,
            &o->source->sample_spec,
            0,
            1,
            0,
            &o->source->silence);
    pa_xfree(memblockq_name);

    o->actual_resample_method = new_resampler ? pa_resampler_get_method(new_resampler) : PA_RESAMPLER_INVALID;

    pa_log_debug("Updated resampler for source output %d", o->index);

    return 0;
}
