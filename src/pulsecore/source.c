/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <pulse/format.h>
#include <pulse/utf8.h>
#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/rtclock.h>
#include <pulse/internal.h>

#include <pulsecore/core-util.h>
#include <pulsecore/source-output.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/mix.h>
#include <pulsecore/flist.h>

#include "source.h"

#define ABSOLUTE_MIN_LATENCY (500)
#define ABSOLUTE_MAX_LATENCY (10*PA_USEC_PER_SEC)
#define DEFAULT_FIXED_LATENCY (250*PA_USEC_PER_MSEC)

PA_DEFINE_PUBLIC_CLASS(pa_source, pa_msgobject);

struct pa_source_volume_change {
    pa_usec_t at;
    pa_cvolume hw_volume;

    PA_LLIST_FIELDS(pa_source_volume_change);
};

struct source_message_set_port {
    pa_device_port *port;
    int ret;
};

static void source_free(pa_object *o);

static void pa_source_volume_change_push(pa_source *s);
static void pa_source_volume_change_flush(pa_source *s);

pa_source_new_data* pa_source_new_data_init(pa_source_new_data *data) {
    pa_assert(data);

    pa_zero(*data);
    data->proplist = pa_proplist_new();
    data->ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    return data;
}

void pa_source_new_data_set_name(pa_source_new_data *data, const char *name) {
    pa_assert(data);

    pa_xfree(data->name);
    data->name = pa_xstrdup(name);
}

void pa_source_new_data_set_sample_spec(pa_source_new_data *data, const pa_sample_spec *spec) {
    pa_assert(data);

    if ((data->sample_spec_is_set = !!spec))
        data->sample_spec = *spec;
}

void pa_source_new_data_set_channel_map(pa_source_new_data *data, const pa_channel_map *map) {
    pa_assert(data);

    if ((data->channel_map_is_set = !!map))
        data->channel_map = *map;
}

void pa_source_new_data_set_alternate_sample_rate(pa_source_new_data *data, const uint32_t alternate_sample_rate) {
    pa_assert(data);

    data->alternate_sample_rate_is_set = TRUE;
    data->alternate_sample_rate = alternate_sample_rate;
}

void pa_source_new_data_set_volume(pa_source_new_data *data, const pa_cvolume *volume) {
    pa_assert(data);

    if ((data->volume_is_set = !!volume))
        data->volume = *volume;
}

void pa_source_new_data_set_muted(pa_source_new_data *data, pa_bool_t mute) {
    pa_assert(data);

    data->muted_is_set = TRUE;
    data->muted = !!mute;
}

void pa_source_new_data_set_port(pa_source_new_data *data, const char *port) {
    pa_assert(data);

    pa_xfree(data->active_port);
    data->active_port = pa_xstrdup(port);
}

void pa_source_new_data_done(pa_source_new_data *data) {
    pa_assert(data);

    pa_proplist_free(data->proplist);

    if (data->ports)
        pa_hashmap_free(data->ports, (pa_free_cb_t) pa_device_port_unref);

    pa_xfree(data->name);
    pa_xfree(data->active_port);
}

/* Called from main context */
static void reset_callbacks(pa_source *s) {
    pa_assert(s);

    s->set_state = NULL;
    s->get_volume = NULL;
    s->set_volume = NULL;
    s->write_volume = NULL;
    s->get_mute = NULL;
    s->set_mute = NULL;
    s->update_requested_latency = NULL;
    s->set_port = NULL;
    s->get_formats = NULL;
    s->update_rate = NULL;
}

/* Called from main context */
pa_source* pa_source_new(
        pa_core *core,
        pa_source_new_data *data,
        pa_source_flags_t flags) {

    pa_source *s;
    const char *name;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    char *pt;

    pa_assert(core);
    pa_assert(data);
    pa_assert(data->name);
    pa_assert_ctl_context();

    s = pa_msgobject_new(pa_source);

    if (!(name = pa_namereg_register(core, data->name, PA_NAMEREG_SOURCE, s, data->namereg_fail))) {
        pa_log_debug("Failed to register name %s.", data->name);
        pa_xfree(s);
        return NULL;
    }

    pa_source_new_data_set_name(data, name);

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_SOURCE_NEW], data) < 0) {
        pa_xfree(s);
        pa_namereg_unregister(core, name);
        return NULL;
    }

    /* FIXME, need to free s here on failure */

    pa_return_null_if_fail(!data->driver || pa_utf8_valid(data->driver));
    pa_return_null_if_fail(data->name && pa_utf8_valid(data->name) && data->name[0]);

    pa_return_null_if_fail(data->sample_spec_is_set && pa_sample_spec_valid(&data->sample_spec));

    if (!data->channel_map_is_set)
        pa_return_null_if_fail(pa_channel_map_init_auto(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT));

    pa_return_null_if_fail(pa_channel_map_valid(&data->channel_map));
    pa_return_null_if_fail(data->channel_map.channels == data->sample_spec.channels);

    /* FIXME: There should probably be a general function for checking whether
     * the source volume is allowed to be set, like there is for source outputs. */
    pa_assert(!data->volume_is_set || !(flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER));

    if (!data->volume_is_set) {
        pa_cvolume_reset(&data->volume, data->sample_spec.channels);
        data->save_volume = FALSE;
    }

    pa_return_null_if_fail(pa_cvolume_valid(&data->volume));
    pa_return_null_if_fail(pa_cvolume_compatible(&data->volume, &data->sample_spec));

    if (!data->muted_is_set)
        data->muted = FALSE;

    if (data->card)
        pa_proplist_update(data->proplist, PA_UPDATE_MERGE, data->card->proplist);

    pa_device_init_description(data->proplist);
    pa_device_init_icon(data->proplist, FALSE);
    pa_device_init_intended_roles(data->proplist);

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_SOURCE_FIXATE], data) < 0) {
        pa_xfree(s);
        pa_namereg_unregister(core, name);
        return NULL;
    }

    s->parent.parent.free = source_free;
    s->parent.process_msg = pa_source_process_msg;

    s->core = core;
    s->state = PA_SOURCE_INIT;
    s->flags = flags;
    s->priority = 0;
    s->suspend_cause = data->suspend_cause;
    pa_source_set_mixer_dirty(s, FALSE);
    s->name = pa_xstrdup(name);
    s->proplist = pa_proplist_copy(data->proplist);
    s->driver = pa_xstrdup(pa_path_get_filename(data->driver));
    s->module = data->module;
    s->card = data->card;

    s->priority = pa_device_init_priority(s->proplist);

    s->sample_spec = data->sample_spec;
    s->channel_map = data->channel_map;
    s->default_sample_rate = s->sample_spec.rate;

    if (data->alternate_sample_rate_is_set)
        s->alternate_sample_rate = data->alternate_sample_rate;
    else
        s->alternate_sample_rate = s->core->alternate_sample_rate;

    if (s->sample_spec.rate == s->alternate_sample_rate) {
        pa_log_warn("Default and alternate sample rates are the same.");
        s->alternate_sample_rate = 0;
    }

    s->outputs = pa_idxset_new(NULL, NULL);
    s->n_corked = 0;
    s->monitor_of = NULL;
    s->output_from_master = NULL;

    s->reference_volume = s->real_volume = data->volume;
    pa_cvolume_reset(&s->soft_volume, s->sample_spec.channels);
    s->base_volume = PA_VOLUME_NORM;
    s->n_volume_steps = PA_VOLUME_NORM+1;
    s->muted = data->muted;
    s->refresh_volume = s->refresh_muted = FALSE;

    reset_callbacks(s);
    s->userdata = NULL;

    s->asyncmsgq = NULL;

    /* As a minor optimization we just steal the list instead of
     * copying it here */
    s->ports = data->ports;
    data->ports = NULL;

    s->active_port = NULL;
    s->save_port = FALSE;

    if (data->active_port)
        if ((s->active_port = pa_hashmap_get(s->ports, data->active_port)))
            s->save_port = data->save_port;

    if (!s->active_port) {
        void *state;
        pa_device_port *p;

        PA_HASHMAP_FOREACH(p, s->ports, state)
            if (!s->active_port || p->priority > s->active_port->priority)
                s->active_port = p;
    }

    if (s->active_port)
        s->latency_offset = s->active_port->latency_offset;
    else
        s->latency_offset = 0;

    s->save_volume = data->save_volume;
    s->save_muted = data->save_muted;

    pa_silence_memchunk_get(
            &core->silence_cache,
            core->mempool,
            &s->silence,
            &s->sample_spec,
            0);

    s->thread_info.rtpoll = NULL;
    s->thread_info.outputs = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    s->thread_info.soft_volume = s->soft_volume;
    s->thread_info.soft_muted = s->muted;
    s->thread_info.state = s->state;
    s->thread_info.max_rewind = 0;
    s->thread_info.requested_latency_valid = FALSE;
    s->thread_info.requested_latency = 0;
    s->thread_info.min_latency = ABSOLUTE_MIN_LATENCY;
    s->thread_info.max_latency = ABSOLUTE_MAX_LATENCY;
    s->thread_info.fixed_latency = flags & PA_SOURCE_DYNAMIC_LATENCY ? 0 : DEFAULT_FIXED_LATENCY;

    PA_LLIST_HEAD_INIT(pa_source_volume_change, s->thread_info.volume_changes);
    s->thread_info.volume_changes_tail = NULL;
    pa_sw_cvolume_multiply(&s->thread_info.current_hw_volume, &s->soft_volume, &s->real_volume);
    s->thread_info.volume_change_safety_margin = core->deferred_volume_safety_margin_usec;
    s->thread_info.volume_change_extra_delay = core->deferred_volume_extra_delay_usec;
    s->thread_info.latency_offset = s->latency_offset;

    /* FIXME: This should probably be moved to pa_source_put() */
    pa_assert_se(pa_idxset_put(core->sources, s, &s->index) >= 0);

    if (s->card)
        pa_assert_se(pa_idxset_put(s->card->sources, s, NULL) >= 0);

    pt = pa_proplist_to_string_sep(s->proplist, "\n    ");
    pa_log_info("Created source %u \"%s\" with sample spec %s and channel map %s\n    %s",
                s->index,
                s->name,
                pa_sample_spec_snprint(st, sizeof(st), &s->sample_spec),
                pa_channel_map_snprint(cm, sizeof(cm), &s->channel_map),
                pt);
    pa_xfree(pt);

    return s;
}

/* Called from main context */
static int source_set_state(pa_source *s, pa_source_state_t state) {
    int ret;
    pa_bool_t suspend_change;
    pa_source_state_t original_state;

    pa_assert(s);
    pa_assert_ctl_context();

    if (s->state == state)
        return 0;

    original_state = s->state;

    suspend_change =
        (original_state == PA_SOURCE_SUSPENDED && PA_SOURCE_IS_OPENED(state)) ||
        (PA_SOURCE_IS_OPENED(original_state) && state == PA_SOURCE_SUSPENDED);

    if (s->set_state)
        if ((ret = s->set_state(s, state)) < 0)
            return ret;

    if (s->asyncmsgq)
        if ((ret = pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), 0, NULL)) < 0) {

            if (s->set_state)
                s->set_state(s, original_state);

            return ret;
        }

    s->state = state;

    if (state != PA_SOURCE_UNLINKED) { /* if we enter UNLINKED state pa_source_unlink() will fire the appropriate events */
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_STATE_CHANGED], s);
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    }

    if (suspend_change) {
        pa_source_output *o;
        uint32_t idx;

        /* We're suspending or resuming, tell everyone about it */

        PA_IDXSET_FOREACH(o, s->outputs, idx)
            if (s->state == PA_SOURCE_SUSPENDED &&
                (o->flags & PA_SOURCE_OUTPUT_KILL_ON_SUSPEND))
                pa_source_output_kill(o);
            else if (o->suspend)
                o->suspend(o, state == PA_SOURCE_SUSPENDED);
    }

    return 0;
}

void pa_source_set_get_volume_callback(pa_source *s, pa_source_cb_t cb) {
    pa_assert(s);

    s->get_volume = cb;
}

void pa_source_set_set_volume_callback(pa_source *s, pa_source_cb_t cb) {
    pa_source_flags_t flags;

    pa_assert(s);
    pa_assert(!s->write_volume || cb);

    s->set_volume = cb;

    /* Save the current flags so we can tell if they've changed */
    flags = s->flags;

    if (cb) {
        /* The source implementor is responsible for setting decibel volume support */
        s->flags |= PA_SOURCE_HW_VOLUME_CTRL;
    } else {
        s->flags &= ~PA_SOURCE_HW_VOLUME_CTRL;
        /* See note below in pa_source_put() about volume sharing and decibel volumes */
        pa_source_enable_decibel_volume(s, !(s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER));
    }

    /* If the flags have changed after init, let any clients know via a change event */
    if (s->state != PA_SOURCE_INIT && flags != s->flags)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

void pa_source_set_write_volume_callback(pa_source *s, pa_source_cb_t cb) {
    pa_source_flags_t flags;

    pa_assert(s);
    pa_assert(!cb || s->set_volume);

    s->write_volume = cb;

    /* Save the current flags so we can tell if they've changed */
    flags = s->flags;

    if (cb)
        s->flags |= PA_SOURCE_DEFERRED_VOLUME;
    else
        s->flags &= ~PA_SOURCE_DEFERRED_VOLUME;

    /* If the flags have changed after init, let any clients know via a change event */
    if (s->state != PA_SOURCE_INIT && flags != s->flags)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

void pa_source_set_get_mute_callback(pa_source *s, pa_source_cb_t cb) {
    pa_assert(s);

    s->get_mute = cb;
}

void pa_source_set_set_mute_callback(pa_source *s, pa_source_cb_t cb) {
    pa_source_flags_t flags;

    pa_assert(s);

    s->set_mute = cb;

    /* Save the current flags so we can tell if they've changed */
    flags = s->flags;

    if (cb)
        s->flags |= PA_SOURCE_HW_MUTE_CTRL;
    else
        s->flags &= ~PA_SOURCE_HW_MUTE_CTRL;

    /* If the flags have changed after init, let any clients know via a change event */
    if (s->state != PA_SOURCE_INIT && flags != s->flags)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

static void enable_flat_volume(pa_source *s, pa_bool_t enable) {
    pa_source_flags_t flags;

    pa_assert(s);

    /* Always follow the overall user preference here */
    enable = enable && s->core->flat_volumes;

    /* Save the current flags so we can tell if they've changed */
    flags = s->flags;

    if (enable)
        s->flags |= PA_SOURCE_FLAT_VOLUME;
    else
        s->flags &= ~PA_SOURCE_FLAT_VOLUME;

    /* If the flags have changed after init, let any clients know via a change event */
    if (s->state != PA_SOURCE_INIT && flags != s->flags)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

void pa_source_enable_decibel_volume(pa_source *s, pa_bool_t enable) {
    pa_source_flags_t flags;

    pa_assert(s);

    /* Save the current flags so we can tell if they've changed */
    flags = s->flags;

    if (enable) {
        s->flags |= PA_SOURCE_DECIBEL_VOLUME;
        enable_flat_volume(s, TRUE);
    } else {
        s->flags &= ~PA_SOURCE_DECIBEL_VOLUME;
        enable_flat_volume(s, FALSE);
    }

    /* If the flags have changed after init, let any clients know via a change event */
    if (s->state != PA_SOURCE_INIT && flags != s->flags)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

/* Called from main context */
void pa_source_put(pa_source *s) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    pa_assert(s->state == PA_SOURCE_INIT);
    pa_assert(!(s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER) || s->output_from_master);

    /* The following fields must be initialized properly when calling _put() */
    pa_assert(s->asyncmsgq);
    pa_assert(s->thread_info.min_latency <= s->thread_info.max_latency);

    /* Generally, flags should be initialized via pa_source_new(). As a
     * special exception we allow some volume related flags to be set
     * between _new() and _put() by the callback setter functions above.
     *
     * Thus we implement a couple safeguards here which ensure the above
     * setters were used (or at least the implementor made manual changes
     * in a compatible way).
     *
     * Note: All of these flags set here can change over the life time
     * of the source. */
    pa_assert(!(s->flags & PA_SOURCE_HW_VOLUME_CTRL) || s->set_volume);
    pa_assert(!(s->flags & PA_SOURCE_DEFERRED_VOLUME) || s->write_volume);
    pa_assert(!(s->flags & PA_SOURCE_HW_MUTE_CTRL) || s->set_mute);

    /* XXX: Currently decibel volume is disabled for all sources that use volume
     * sharing. When the master source supports decibel volume, it would be good
     * to have the flag also in the filter source, but currently we don't do that
     * so that the flags of the filter source never change when it's moved from
     * a master source to another. One solution for this problem would be to
     * remove user-visible volume altogether from filter sources when volume
     * sharing is used, but the current approach was easier to implement... */
    /* We always support decibel volumes in software, otherwise we leave it to
     * the source implementor to set this flag as needed.
     *
     * Note: This flag can also change over the life time of the source. */
    if (!(s->flags & PA_SOURCE_HW_VOLUME_CTRL) && !(s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER))
        pa_source_enable_decibel_volume(s, TRUE);

    /* If the source implementor support DB volumes by itself, we should always
     * try and enable flat volumes too */
    if ((s->flags & PA_SOURCE_DECIBEL_VOLUME))
        enable_flat_volume(s, TRUE);

    if (s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER) {
        pa_source *root_source = pa_source_get_master(s);

        pa_assert(PA_LIKELY(root_source));

        s->reference_volume = root_source->reference_volume;
        pa_cvolume_remap(&s->reference_volume, &root_source->channel_map, &s->channel_map);

        s->real_volume = root_source->real_volume;
        pa_cvolume_remap(&s->real_volume, &root_source->channel_map, &s->channel_map);
    } else
        /* We assume that if the sink implementor changed the default
         * volume he did so in real_volume, because that is the usual
         * place where he is supposed to place his changes.  */
        s->reference_volume = s->real_volume;

    s->thread_info.soft_volume = s->soft_volume;
    s->thread_info.soft_muted = s->muted;
    pa_sw_cvolume_multiply(&s->thread_info.current_hw_volume, &s->soft_volume, &s->real_volume);

    pa_assert((s->flags & PA_SOURCE_HW_VOLUME_CTRL)
              || (s->base_volume == PA_VOLUME_NORM
                  && ((s->flags & PA_SOURCE_DECIBEL_VOLUME || (s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER)))));
    pa_assert(!(s->flags & PA_SOURCE_DECIBEL_VOLUME) || s->n_volume_steps == PA_VOLUME_NORM+1);
    pa_assert(!(s->flags & PA_SOURCE_DYNAMIC_LATENCY) == (s->thread_info.fixed_latency != 0));

    if (s->suspend_cause)
        pa_assert_se(source_set_state(s, PA_SOURCE_SUSPENDED) == 0);
    else
        pa_assert_se(source_set_state(s, PA_SOURCE_IDLE) == 0);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_NEW, s->index);
    pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_PUT], s);
}

/* Called from main context */
void pa_source_unlink(pa_source *s) {
    pa_bool_t linked;
    pa_source_output *o, *j = NULL;

    pa_assert(s);
    pa_assert_ctl_context();

    /* See pa_sink_unlink() for a couple of comments how this function
     * works. */

    linked = PA_SOURCE_IS_LINKED(s->state);

    if (linked)
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK], s);

    if (s->state != PA_SOURCE_UNLINKED)
        pa_namereg_unregister(s->core, s->name);
    pa_idxset_remove_by_data(s->core->sources, s, NULL);

    if (s->card)
        pa_idxset_remove_by_data(s->card->sources, s, NULL);

    while ((o = pa_idxset_first(s->outputs, NULL))) {
        pa_assert(o != j);
        pa_source_output_kill(o);
        j = o;
    }

    if (linked)
        source_set_state(s, PA_SOURCE_UNLINKED);
    else
        s->state = PA_SOURCE_UNLINKED;

    reset_callbacks(s);

    if (linked) {
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK_POST], s);
    }
}

/* Called from main context */
static void source_free(pa_object *o) {
    pa_source *s = PA_SOURCE(o);

    pa_assert(s);
    pa_assert_ctl_context();
    pa_assert(pa_source_refcnt(s) == 0);

    if (PA_SOURCE_IS_LINKED(s->state))
        pa_source_unlink(s);

    pa_log_info("Freeing source %u \"%s\"", s->index, s->name);

    pa_idxset_free(s->outputs, NULL);
    pa_hashmap_free(s->thread_info.outputs, (pa_free_cb_t) pa_source_output_unref);

    if (s->silence.memblock)
        pa_memblock_unref(s->silence.memblock);

    pa_xfree(s->name);
    pa_xfree(s->driver);

    if (s->proplist)
        pa_proplist_free(s->proplist);

    if (s->ports)
        pa_hashmap_free(s->ports, (pa_free_cb_t) pa_device_port_unref);

    pa_xfree(s);
}

/* Called from main context, and not while the IO thread is active, please */
void pa_source_set_asyncmsgq(pa_source *s, pa_asyncmsgq *q) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    s->asyncmsgq = q;
}

/* Called from main context, and not while the IO thread is active, please */
void pa_source_update_flags(pa_source *s, pa_source_flags_t mask, pa_source_flags_t value) {
    pa_source_flags_t old_flags;
    pa_source_output *output;
    uint32_t idx;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    /* For now, allow only a minimal set of flags to be changed. */
    pa_assert((mask & ~(PA_SOURCE_DYNAMIC_LATENCY|PA_SOURCE_LATENCY)) == 0);

    old_flags = s->flags;
    s->flags = (s->flags & ~mask) | (value & mask);

    if (s->flags == old_flags)
        return;

    if ((s->flags & PA_SOURCE_LATENCY) != (old_flags & PA_SOURCE_LATENCY))
        pa_log_debug("Source %s: LATENCY flag %s.", s->name, (s->flags & PA_SOURCE_LATENCY) ? "enabled" : "disabled");

    if ((s->flags & PA_SOURCE_DYNAMIC_LATENCY) != (old_flags & PA_SOURCE_DYNAMIC_LATENCY))
        pa_log_debug("Source %s: DYNAMIC_LATENCY flag %s.",
                     s->name, (s->flags & PA_SOURCE_DYNAMIC_LATENCY) ? "enabled" : "disabled");

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE | PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_FLAGS_CHANGED], s);

    PA_IDXSET_FOREACH(output, s->outputs, idx) {
        if (output->destination_source)
            pa_source_update_flags(output->destination_source, mask, value);
    }
}

/* Called from IO context, or before _put() from main context */
void pa_source_set_rtpoll(pa_source *s, pa_rtpoll *p) {
    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);

    s->thread_info.rtpoll = p;
}

/* Called from main context */
int pa_source_update_status(pa_source*s) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    if (s->state == PA_SOURCE_SUSPENDED)
        return 0;

    return source_set_state(s, pa_source_used_by(s) ? PA_SOURCE_RUNNING : PA_SOURCE_IDLE);
}

/* Called from any context - must be threadsafe */
void pa_source_set_mixer_dirty(pa_source *s, pa_bool_t is_dirty)
{
    pa_atomic_store(&s->mixer_dirty, is_dirty ? 1 : 0);
}

/* Called from main context */
int pa_source_suspend(pa_source *s, pa_bool_t suspend, pa_suspend_cause_t cause) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));
    pa_assert(cause != 0);

    if (s->monitor_of && cause != PA_SUSPEND_PASSTHROUGH)
        return -PA_ERR_NOTSUPPORTED;

    if (suspend)
        s->suspend_cause |= cause;
    else
        s->suspend_cause &= ~cause;

    if (!(s->suspend_cause & PA_SUSPEND_SESSION) && (pa_atomic_load(&s->mixer_dirty) != 0)) {
        /* This might look racy but isn't: If somebody sets mixer_dirty exactly here,
           it'll be handled just fine. */
        pa_source_set_mixer_dirty(s, FALSE);
        pa_log_debug("Mixer is now accessible. Updating alsa mixer settings.");
        if (s->active_port && s->set_port) {
            if (s->flags & PA_SOURCE_DEFERRED_VOLUME) {
                struct source_message_set_port msg = { .port = s->active_port, .ret = 0 };
                pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_PORT, &msg, 0, NULL) == 0);
            }
            else
                s->set_port(s, s->active_port);
        }
        else {
            if (s->set_mute)
                s->set_mute(s);
            if (s->set_volume)
                s->set_volume(s);
        }
    }

    if ((pa_source_get_state(s) == PA_SOURCE_SUSPENDED) == !!s->suspend_cause)
        return 0;

    pa_log_debug("Suspend cause of source %s is 0x%04x, %s", s->name, s->suspend_cause, s->suspend_cause ? "suspending" : "resuming");

    if (s->suspend_cause)
        return source_set_state(s, PA_SOURCE_SUSPENDED);
    else
        return source_set_state(s, pa_source_used_by(s) ? PA_SOURCE_RUNNING : PA_SOURCE_IDLE);
}

/* Called from main context */
int pa_source_sync_suspend(pa_source *s) {
    pa_sink_state_t state;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));
    pa_assert(s->monitor_of);

    state = pa_sink_get_state(s->monitor_of);

    if (state == PA_SINK_SUSPENDED)
        return source_set_state(s, PA_SOURCE_SUSPENDED);

    pa_assert(PA_SINK_IS_OPENED(state));

    return source_set_state(s, pa_source_used_by(s) ? PA_SOURCE_RUNNING : PA_SOURCE_IDLE);
}

/* Called from main context */
pa_queue *pa_source_move_all_start(pa_source *s, pa_queue *q) {
    pa_source_output *o, *n;
    uint32_t idx;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    if (!q)
        q = pa_queue_new();

    for (o = PA_SOURCE_OUTPUT(pa_idxset_first(s->outputs, &idx)); o; o = n) {
        n = PA_SOURCE_OUTPUT(pa_idxset_next(s->outputs, &idx));

        pa_source_output_ref(o);

        if (pa_source_output_start_move(o) >= 0)
            pa_queue_push(q, o);
        else
            pa_source_output_unref(o);
    }

    return q;
}

/* Called from main context */
void pa_source_move_all_finish(pa_source *s, pa_queue *q, pa_bool_t save) {
    pa_source_output *o;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));
    pa_assert(q);

    while ((o = PA_SOURCE_OUTPUT(pa_queue_pop(q)))) {
        if (pa_source_output_finish_move(o, s, save) < 0)
            pa_source_output_fail_move(o);

        pa_source_output_unref(o);
    }

    pa_queue_free(q, NULL);
}

/* Called from main context */
void pa_source_move_all_fail(pa_queue *q) {
    pa_source_output *o;

    pa_assert_ctl_context();
    pa_assert(q);

    while ((o = PA_SOURCE_OUTPUT(pa_queue_pop(q)))) {
        pa_source_output_fail_move(o);
        pa_source_output_unref(o);
    }

    pa_queue_free(q, NULL);
}

/* Called from IO thread context */
void pa_source_process_rewind(pa_source *s, size_t nbytes) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);
    pa_assert(PA_SOURCE_IS_LINKED(s->thread_info.state));

    if (nbytes <= 0)
        return;

    if (s->thread_info.state == PA_SOURCE_SUSPENDED)
        return;

    pa_log_debug("Processing rewind...");

    PA_HASHMAP_FOREACH(o, s->thread_info.outputs, state) {
        pa_source_output_assert_ref(o);
        pa_source_output_process_rewind(o, nbytes);
    }
}

/* Called from IO thread context */
void pa_source_post(pa_source*s, const pa_memchunk *chunk) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);
    pa_assert(PA_SOURCE_IS_LINKED(s->thread_info.state));
    pa_assert(chunk);

    if (s->thread_info.state == PA_SOURCE_SUSPENDED)
        return;

    if (s->thread_info.soft_muted || !pa_cvolume_is_norm(&s->thread_info.soft_volume)) {
        pa_memchunk vchunk = *chunk;

        pa_memblock_ref(vchunk.memblock);
        pa_memchunk_make_writable(&vchunk, 0);

        if (s->thread_info.soft_muted || pa_cvolume_is_muted(&s->thread_info.soft_volume))
            pa_silence_memchunk(&vchunk, &s->sample_spec);
        else
            pa_volume_memchunk(&vchunk, &s->sample_spec, &s->thread_info.soft_volume);

        while ((o = pa_hashmap_iterate(s->thread_info.outputs, &state, NULL))) {
            pa_source_output_assert_ref(o);

            if (!o->thread_info.direct_on_input)
                pa_source_output_push(o, &vchunk);
        }

        pa_memblock_unref(vchunk.memblock);
    } else {

        while ((o = pa_hashmap_iterate(s->thread_info.outputs, &state, NULL))) {
            pa_source_output_assert_ref(o);

            if (!o->thread_info.direct_on_input)
                pa_source_output_push(o, chunk);
        }
    }
}

/* Called from IO thread context */
void pa_source_post_direct(pa_source*s, pa_source_output *o, const pa_memchunk *chunk) {
    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);
    pa_assert(PA_SOURCE_IS_LINKED(s->thread_info.state));
    pa_source_output_assert_ref(o);
    pa_assert(o->thread_info.direct_on_input);
    pa_assert(chunk);

    if (s->thread_info.state == PA_SOURCE_SUSPENDED)
        return;

    if (s->thread_info.soft_muted || !pa_cvolume_is_norm(&s->thread_info.soft_volume)) {
        pa_memchunk vchunk = *chunk;

        pa_memblock_ref(vchunk.memblock);
        pa_memchunk_make_writable(&vchunk, 0);

        if (s->thread_info.soft_muted || pa_cvolume_is_muted(&s->thread_info.soft_volume))
            pa_silence_memchunk(&vchunk, &s->sample_spec);
        else
            pa_volume_memchunk(&vchunk, &s->sample_spec, &s->thread_info.soft_volume);

        pa_source_output_push(o, &vchunk);

        pa_memblock_unref(vchunk.memblock);
    } else
        pa_source_output_push(o, chunk);
}

/* Called from main thread */
pa_bool_t pa_source_update_rate(pa_source *s, uint32_t rate, pa_bool_t passthrough)
{
    if (s->update_rate) {
        uint32_t desired_rate = rate;
        uint32_t default_rate = s->default_sample_rate;
        uint32_t alternate_rate = s->alternate_sample_rate;
        uint32_t idx;
        pa_source_output *o;
        pa_bool_t use_alternate = FALSE;

        if (PA_UNLIKELY(default_rate == alternate_rate)) {
            pa_log_warn("Default and alternate sample rates are the same.");
            return FALSE;
        }

        if (PA_SOURCE_IS_RUNNING(s->state)) {
            pa_log_info("Cannot update rate, SOURCE_IS_RUNNING, will keep using %u Hz",
                        s->sample_spec.rate);
            return FALSE;
        }

        if (PA_UNLIKELY (desired_rate < 8000 ||
                         desired_rate > PA_RATE_MAX))
            return FALSE;

        if (!passthrough) {
            pa_assert(default_rate % 4000 || default_rate % 11025);
            pa_assert(alternate_rate % 4000 || alternate_rate % 11025);

            if (default_rate % 4000) {
                /* default is a 11025 multiple */
                if ((alternate_rate % 4000 == 0) && (desired_rate % 4000 == 0))
                    use_alternate=TRUE;
            } else {
                /* default is 4000 multiple */
                if ((alternate_rate % 11025 == 0) && (desired_rate % 11025 == 0))
                    use_alternate=TRUE;
            }

            if (use_alternate)
                desired_rate = alternate_rate;
            else
                desired_rate = default_rate;
        } else {
            desired_rate = rate; /* use stream sampling rate, discard default/alternate settings */
        }

        if (desired_rate == s->sample_spec.rate)
            return FALSE;

        if (!passthrough && pa_source_used_by(s) > 0)
            return FALSE;

        pa_log_debug("Suspending source %s due to changing the sample rate.", s->name);
        pa_source_suspend(s, TRUE, PA_SUSPEND_IDLE); /* needed before rate update, will be resumed automatically */

        if (s->update_rate(s, desired_rate) == TRUE) {
            pa_log_info("Changed sampling rate successfully ");

            PA_IDXSET_FOREACH(o, s->outputs, idx) {
                if (o->state == PA_SOURCE_OUTPUT_CORKED)
                    pa_source_output_update_rate(o);
            }
            return TRUE;
        }
    }
    return FALSE;
}

/* Called from main thread */
pa_usec_t pa_source_get_latency(pa_source *s) {
    pa_usec_t usec;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    if (s->state == PA_SOURCE_SUSPENDED)
        return 0;

    if (!(s->flags & PA_SOURCE_LATENCY))
        return 0;

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_LATENCY, &usec, 0, NULL) == 0);

    /* usec is unsigned, so check that the offset can be added to usec without
     * underflowing. */
    if (-s->latency_offset <= (int64_t) usec)
        usec += s->latency_offset;
    else
        usec = 0;

    return usec;
}

/* Called from IO thread */
pa_usec_t pa_source_get_latency_within_thread(pa_source *s) {
    pa_usec_t usec = 0;
    pa_msgobject *o;

    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);
    pa_assert(PA_SOURCE_IS_LINKED(s->thread_info.state));

    /* The returned value is supposed to be in the time domain of the sound card! */

    if (s->thread_info.state == PA_SOURCE_SUSPENDED)
        return 0;

    if (!(s->flags & PA_SOURCE_LATENCY))
        return 0;

    o = PA_MSGOBJECT(s);

    /* FIXME: We probably should make this a proper vtable callback instead of going through process_msg() */

    if (o->process_msg(o, PA_SOURCE_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
        return -1;

    /* usec is unsigned, so check that the offset can be added to usec without
     * underflowing. */
    if (-s->thread_info.latency_offset <= (int64_t) usec)
        usec += s->thread_info.latency_offset;
    else
        usec = 0;

    return usec;
}

/* Called from the main thread (and also from the IO thread while the main
 * thread is waiting).
 *
 * When a source uses volume sharing, it never has the PA_SOURCE_FLAT_VOLUME flag
 * set. Instead, flat volume mode is detected by checking whether the root source
 * has the flag set. */
pa_bool_t pa_source_flat_volume_enabled(pa_source *s) {
    pa_source_assert_ref(s);

    s = pa_source_get_master(s);

    if (PA_LIKELY(s))
        return (s->flags & PA_SOURCE_FLAT_VOLUME);
    else
        return FALSE;
}

/* Called from the main thread (and also from the IO thread while the main
 * thread is waiting). */
pa_source *pa_source_get_master(pa_source *s) {
    pa_source_assert_ref(s);

    while (s && (s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER)) {
        if (PA_UNLIKELY(!s->output_from_master))
            return NULL;

        s = s->output_from_master->source;
    }

    return s;
}

/* Called from main context */
pa_bool_t pa_source_is_passthrough(pa_source *s) {

    pa_source_assert_ref(s);

    /* NB Currently only monitor sources support passthrough mode */
    return (s->monitor_of && pa_sink_is_passthrough(s->monitor_of));
}

/* Called from main context */
void pa_source_enter_passthrough(pa_source *s) {
    pa_cvolume volume;

    /* set the volume to NORM */
    s->saved_volume = *pa_source_get_volume(s, TRUE);
    s->saved_save_volume = s->save_volume;

    pa_cvolume_set(&volume, s->sample_spec.channels, PA_MIN(s->base_volume, PA_VOLUME_NORM));
    pa_source_set_volume(s, &volume, TRUE, FALSE);
}

/* Called from main context */
void pa_source_leave_passthrough(pa_source *s) {
    /* Restore source volume to what it was before we entered passthrough mode */
    pa_source_set_volume(s, &s->saved_volume, TRUE, s->saved_save_volume);

    pa_cvolume_init(&s->saved_volume);
    s->saved_save_volume = FALSE;
}

/* Called from main context. */
static void compute_reference_ratio(pa_source_output *o) {
    unsigned c = 0;
    pa_cvolume remapped;

    pa_assert(o);
    pa_assert(pa_source_flat_volume_enabled(o->source));

    /*
     * Calculates the reference ratio from the source's reference
     * volume. This basically calculates:
     *
     * o->reference_ratio = o->volume / o->source->reference_volume
     */

    remapped = o->source->reference_volume;
    pa_cvolume_remap(&remapped, &o->source->channel_map, &o->channel_map);

    o->reference_ratio.channels = o->sample_spec.channels;

    for (c = 0; c < o->sample_spec.channels; c++) {

        /* We don't update when the source volume is 0 anyway */
        if (remapped.values[c] <= PA_VOLUME_MUTED)
            continue;

        /* Don't update the reference ratio unless necessary */
        if (pa_sw_volume_multiply(
                    o->reference_ratio.values[c],
                    remapped.values[c]) == o->volume.values[c])
            continue;

        o->reference_ratio.values[c] = pa_sw_volume_divide(
                o->volume.values[c],
                remapped.values[c]);
    }
}

/* Called from main context. Only called for the root source in volume sharing
 * cases, except for internal recursive calls. */
static void compute_reference_ratios(pa_source *s) {
    uint32_t idx;
    pa_source_output *o;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));
    pa_assert(pa_source_flat_volume_enabled(s));

    PA_IDXSET_FOREACH(o, s->outputs, idx) {
        compute_reference_ratio(o);

        if (o->destination_source && (o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER))
            compute_reference_ratios(o->destination_source);
    }
}

/* Called from main context. Only called for the root source in volume sharing
 * cases, except for internal recursive calls. */
static void compute_real_ratios(pa_source *s) {
    pa_source_output *o;
    uint32_t idx;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));
    pa_assert(pa_source_flat_volume_enabled(s));

    PA_IDXSET_FOREACH(o, s->outputs, idx) {
        unsigned c;
        pa_cvolume remapped;

        if (o->destination_source && (o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER)) {
            /* The origin source uses volume sharing, so this input's real ratio
             * is handled as a special case - the real ratio must be 0 dB, and
             * as a result i->soft_volume must equal i->volume_factor. */
            pa_cvolume_reset(&o->real_ratio, o->real_ratio.channels);
            o->soft_volume = o->volume_factor;

            compute_real_ratios(o->destination_source);

            continue;
        }

        /*
         * This basically calculates:
         *
         * i->real_ratio := i->volume / s->real_volume
         * i->soft_volume := i->real_ratio * i->volume_factor
         */

        remapped = s->real_volume;
        pa_cvolume_remap(&remapped, &s->channel_map, &o->channel_map);

        o->real_ratio.channels = o->sample_spec.channels;
        o->soft_volume.channels = o->sample_spec.channels;

        for (c = 0; c < o->sample_spec.channels; c++) {

            if (remapped.values[c] <= PA_VOLUME_MUTED) {
                /* We leave o->real_ratio untouched */
                o->soft_volume.values[c] = PA_VOLUME_MUTED;
                continue;
            }

            /* Don't lose accuracy unless necessary */
            if (pa_sw_volume_multiply(
                        o->real_ratio.values[c],
                        remapped.values[c]) != o->volume.values[c])

                o->real_ratio.values[c] = pa_sw_volume_divide(
                        o->volume.values[c],
                        remapped.values[c]);

            o->soft_volume.values[c] = pa_sw_volume_multiply(
                    o->real_ratio.values[c],
                    o->volume_factor.values[c]);
        }

        /* We don't copy the soft_volume to the thread_info data
         * here. That must be done by the caller */
    }
}

static pa_cvolume *cvolume_remap_minimal_impact(
        pa_cvolume *v,
        const pa_cvolume *template,
        const pa_channel_map *from,
        const pa_channel_map *to) {

    pa_cvolume t;

    pa_assert(v);
    pa_assert(template);
    pa_assert(from);
    pa_assert(to);
    pa_assert(pa_cvolume_compatible_with_channel_map(v, from));
    pa_assert(pa_cvolume_compatible_with_channel_map(template, to));

    /* Much like pa_cvolume_remap(), but tries to minimize impact when
     * mapping from source output to source volumes:
     *
     * If template is a possible remapping from v it is used instead
     * of remapping anew.
     *
     * If the channel maps don't match we set an all-channel volume on
     * the source to ensure that changing a volume on one stream has no
     * effect that cannot be compensated for in another stream that
     * does not have the same channel map as the source. */

    if (pa_channel_map_equal(from, to))
        return v;

    t = *template;
    if (pa_cvolume_equal(pa_cvolume_remap(&t, to, from), v)) {
        *v = *template;
        return v;
    }

    pa_cvolume_set(v, to->channels, pa_cvolume_max(v));
    return v;
}

/* Called from main thread. Only called for the root source in volume sharing
 * cases, except for internal recursive calls. */
static void get_maximum_output_volume(pa_source *s, pa_cvolume *max_volume, const pa_channel_map *channel_map) {
    pa_source_output *o;
    uint32_t idx;

    pa_source_assert_ref(s);
    pa_assert(max_volume);
    pa_assert(channel_map);
    pa_assert(pa_source_flat_volume_enabled(s));

    PA_IDXSET_FOREACH(o, s->outputs, idx) {
        pa_cvolume remapped;

        if (o->destination_source && (o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER)) {
            get_maximum_output_volume(o->destination_source, max_volume, channel_map);

            /* Ignore this output. The origin source uses volume sharing, so this
             * output's volume will be set to be equal to the root source's real
             * volume. Obviously this output's current volume must not then
             * affect what the root source's real volume will be. */
            continue;
        }

        remapped = o->volume;
        cvolume_remap_minimal_impact(&remapped, max_volume, &o->channel_map, channel_map);
        pa_cvolume_merge(max_volume, max_volume, &remapped);
    }
}

/* Called from main thread. Only called for the root source in volume sharing
 * cases, except for internal recursive calls. */
static pa_bool_t has_outputs(pa_source *s) {
    pa_source_output *o;
    uint32_t idx;

    pa_source_assert_ref(s);

    PA_IDXSET_FOREACH(o, s->outputs, idx) {
        if (!o->destination_source || !(o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER) || has_outputs(o->destination_source))
            return TRUE;
    }

    return FALSE;
}

/* Called from main thread. Only called for the root source in volume sharing
 * cases, except for internal recursive calls. */
static void update_real_volume(pa_source *s, const pa_cvolume *new_volume, pa_channel_map *channel_map) {
    pa_source_output *o;
    uint32_t idx;

    pa_source_assert_ref(s);
    pa_assert(new_volume);
    pa_assert(channel_map);

    s->real_volume = *new_volume;
    pa_cvolume_remap(&s->real_volume, channel_map, &s->channel_map);

    PA_IDXSET_FOREACH(o, s->outputs, idx) {
        if (o->destination_source && (o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER)) {
            if (pa_source_flat_volume_enabled(s)) {
                pa_cvolume old_volume = o->volume;

                /* Follow the root source's real volume. */
                o->volume = *new_volume;
                pa_cvolume_remap(&o->volume, channel_map, &o->channel_map);
                compute_reference_ratio(o);

                /* The volume changed, let's tell people so */
                if (!pa_cvolume_equal(&old_volume, &o->volume)) {
                    if (o->volume_changed)
                        o->volume_changed(o);

                    pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
                }
            }

            update_real_volume(o->destination_source, new_volume, channel_map);
        }
    }
}

/* Called from main thread. Only called for the root source in shared volume
 * cases. */
static void compute_real_volume(pa_source *s) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));
    pa_assert(pa_source_flat_volume_enabled(s));
    pa_assert(!(s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER));

    /* This determines the maximum volume of all streams and sets
     * s->real_volume accordingly. */

    if (!has_outputs(s)) {
        /* In the special case that we have no source outputs we leave the
         * volume unmodified. */
        update_real_volume(s, &s->reference_volume, &s->channel_map);
        return;
    }

    pa_cvolume_mute(&s->real_volume, s->channel_map.channels);

    /* First let's determine the new maximum volume of all outputs
     * connected to this source */
    get_maximum_output_volume(s, &s->real_volume, &s->channel_map);
    update_real_volume(s, &s->real_volume, &s->channel_map);

    /* Then, let's update the real ratios/soft volumes of all outputs
     * connected to this source */
    compute_real_ratios(s);
}

/* Called from main thread. Only called for the root source in shared volume
 * cases, except for internal recursive calls. */
static void propagate_reference_volume(pa_source *s) {
    pa_source_output *o;
    uint32_t idx;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));
    pa_assert(pa_source_flat_volume_enabled(s));

    /* This is called whenever the source volume changes that is not
     * caused by a source output volume change. We need to fix up the
     * source output volumes accordingly */

    PA_IDXSET_FOREACH(o, s->outputs, idx) {
        pa_cvolume old_volume;

        if (o->destination_source && (o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER)) {
            propagate_reference_volume(o->destination_source);

            /* Since the origin source uses volume sharing, this output's volume
             * needs to be updated to match the root source's real volume, but
             * that will be done later in update_shared_real_volume(). */
            continue;
        }

        old_volume = o->volume;

        /* This basically calculates:
         *
         * o->volume := o->reference_volume * o->reference_ratio  */

        o->volume = s->reference_volume;
        pa_cvolume_remap(&o->volume, &s->channel_map, &o->channel_map);
        pa_sw_cvolume_multiply(&o->volume, &o->volume, &o->reference_ratio);

        /* The volume changed, let's tell people so */
        if (!pa_cvolume_equal(&old_volume, &o->volume)) {

            if (o->volume_changed)
                o->volume_changed(o);

            pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
        }
    }
}

/* Called from main thread. Only called for the root source in volume sharing
 * cases, except for internal recursive calls. The return value indicates
 * whether any reference volume actually changed. */
static pa_bool_t update_reference_volume(pa_source *s, const pa_cvolume *v, const pa_channel_map *channel_map, pa_bool_t save) {
    pa_cvolume volume;
    pa_bool_t reference_volume_changed;
    pa_source_output *o;
    uint32_t idx;

    pa_source_assert_ref(s);
    pa_assert(PA_SOURCE_IS_LINKED(s->state));
    pa_assert(v);
    pa_assert(channel_map);
    pa_assert(pa_cvolume_valid(v));

    volume = *v;
    pa_cvolume_remap(&volume, channel_map, &s->channel_map);

    reference_volume_changed = !pa_cvolume_equal(&volume, &s->reference_volume);
    s->reference_volume = volume;

    s->save_volume = (!reference_volume_changed && s->save_volume) || save;

    if (reference_volume_changed)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    else if (!(s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER))
        /* If the root source's volume doesn't change, then there can't be any
         * changes in the other source in the source tree either.
         *
         * It's probably theoretically possible that even if the root source's
         * volume changes slightly, some filter source doesn't change its volume
         * due to rounding errors. If that happens, we still want to propagate
         * the changed root source volume to the sources connected to the
         * intermediate source that didn't change its volume. This theoretical
         * possibility is the reason why we have that !(s->flags &
         * PA_SOURCE_SHARE_VOLUME_WITH_MASTER) condition. Probably nobody would
         * notice even if we returned here FALSE always if
         * reference_volume_changed is FALSE. */
        return FALSE;

    PA_IDXSET_FOREACH(o, s->outputs, idx) {
        if (o->destination_source && (o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER))
            update_reference_volume(o->destination_source, v, channel_map, FALSE);
    }

    return TRUE;
}

/* Called from main thread */
void pa_source_set_volume(
        pa_source *s,
        const pa_cvolume *volume,
        pa_bool_t send_msg,
        pa_bool_t save) {

    pa_cvolume new_reference_volume;
    pa_source *root_source;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));
    pa_assert(!volume || pa_cvolume_valid(volume));
    pa_assert(volume || pa_source_flat_volume_enabled(s));
    pa_assert(!volume || volume->channels == 1 || pa_cvolume_compatible(volume, &s->sample_spec));

    /* make sure we don't change the volume in PASSTHROUGH mode ...
     * ... *except* if we're being invoked to reset the volume to ensure 0 dB gain */
    if (pa_source_is_passthrough(s) && (!volume || !pa_cvolume_is_norm(volume))) {
        pa_log_warn("Cannot change volume, source is monitor of a PASSTHROUGH sink");
        return;
    }

    /* In case of volume sharing, the volume is set for the root source first,
     * from which it's then propagated to the sharing sources. */
    root_source = pa_source_get_master(s);

    if (PA_UNLIKELY(!root_source))
        return;

    /* As a special exception we accept mono volumes on all sources --
     * even on those with more complex channel maps */

    if (volume) {
        if (pa_cvolume_compatible(volume, &s->sample_spec))
            new_reference_volume = *volume;
        else {
            new_reference_volume = s->reference_volume;
            pa_cvolume_scale(&new_reference_volume, pa_cvolume_max(volume));
        }

        pa_cvolume_remap(&new_reference_volume, &s->channel_map, &root_source->channel_map);

        if (update_reference_volume(root_source, &new_reference_volume, &root_source->channel_map, save)) {
            if (pa_source_flat_volume_enabled(root_source)) {
                /* OK, propagate this volume change back to the outputs */
                propagate_reference_volume(root_source);

                /* And now recalculate the real volume */
                compute_real_volume(root_source);
            } else
                update_real_volume(root_source, &root_source->reference_volume, &root_source->channel_map);
        }

    } else {
        /* If volume is NULL we synchronize the source's real and
         * reference volumes with the stream volumes. */

        pa_assert(pa_source_flat_volume_enabled(root_source));

        /* Ok, let's determine the new real volume */
        compute_real_volume(root_source);

        /* Let's 'push' the reference volume if necessary */
        pa_cvolume_merge(&new_reference_volume, &s->reference_volume, &root_source->real_volume);
        /* If the source and it's root don't have the same number of channels, we need to remap */
        if (s != root_source && !pa_channel_map_equal(&s->channel_map, &root_source->channel_map))
            pa_cvolume_remap(&new_reference_volume, &s->channel_map, &root_source->channel_map);
        update_reference_volume(root_source, &new_reference_volume, &root_source->channel_map, save);

        /* Now that the reference volume is updated, we can update the streams'
         * reference ratios. */
        compute_reference_ratios(root_source);
    }

    if (root_source->set_volume) {
        /* If we have a function set_volume(), then we do not apply a
         * soft volume by default. However, set_volume() is free to
         * apply one to root_source->soft_volume */

        pa_cvolume_reset(&root_source->soft_volume, root_source->sample_spec.channels);
        if (!(root_source->flags & PA_SOURCE_DEFERRED_VOLUME))
            root_source->set_volume(root_source);

    } else
        /* If we have no function set_volume(), then the soft volume
         * becomes the real volume */
        root_source->soft_volume = root_source->real_volume;

    /* This tells the source that soft volume and/or real volume changed */
    if (send_msg)
        pa_assert_se(pa_asyncmsgq_send(root_source->asyncmsgq, PA_MSGOBJECT(root_source), PA_SOURCE_MESSAGE_SET_SHARED_VOLUME, NULL, 0, NULL) == 0);
}

/* Called from the io thread if sync volume is used, otherwise from the main thread.
 * Only to be called by source implementor */
void pa_source_set_soft_volume(pa_source *s, const pa_cvolume *volume) {

    pa_source_assert_ref(s);
    pa_assert(!(s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER));

    if (s->flags & PA_SOURCE_DEFERRED_VOLUME)
        pa_source_assert_io_context(s);
    else
        pa_assert_ctl_context();

    if (!volume)
        pa_cvolume_reset(&s->soft_volume, s->sample_spec.channels);
    else
        s->soft_volume = *volume;

    if (PA_SOURCE_IS_LINKED(s->state) && !(s->flags & PA_SOURCE_DEFERRED_VOLUME))
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_VOLUME, NULL, 0, NULL) == 0);
    else
        s->thread_info.soft_volume = s->soft_volume;
}

/* Called from the main thread. Only called for the root source in volume sharing
 * cases, except for internal recursive calls. */
static void propagate_real_volume(pa_source *s, const pa_cvolume *old_real_volume) {
    pa_source_output *o;
    uint32_t idx;

    pa_source_assert_ref(s);
    pa_assert(old_real_volume);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    /* This is called when the hardware's real volume changes due to
     * some external event. We copy the real volume into our
     * reference volume and then rebuild the stream volumes based on
     * i->real_ratio which should stay fixed. */

    if (!(s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER)) {
        if (pa_cvolume_equal(old_real_volume, &s->real_volume))
            return;

        /* 1. Make the real volume the reference volume */
        update_reference_volume(s, &s->real_volume, &s->channel_map, TRUE);
    }

    if (pa_source_flat_volume_enabled(s)) {

        PA_IDXSET_FOREACH(o, s->outputs, idx) {
            pa_cvolume old_volume = o->volume;

            /* 2. Since the source's reference and real volumes are equal
             * now our ratios should be too. */
            o->reference_ratio = o->real_ratio;

            /* 3. Recalculate the new stream reference volume based on the
             * reference ratio and the sink's reference volume.
             *
             * This basically calculates:
             *
             * o->volume = s->reference_volume * o->reference_ratio
             *
             * This is identical to propagate_reference_volume() */
            o->volume = s->reference_volume;
            pa_cvolume_remap(&o->volume, &s->channel_map, &o->channel_map);
            pa_sw_cvolume_multiply(&o->volume, &o->volume, &o->reference_ratio);

            /* Notify if something changed */
            if (!pa_cvolume_equal(&old_volume, &o->volume)) {

                if (o->volume_changed)
                    o->volume_changed(o);

                pa_subscription_post(o->core, PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT|PA_SUBSCRIPTION_EVENT_CHANGE, o->index);
            }

            if (o->destination_source && (o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER))
                propagate_real_volume(o->destination_source, old_real_volume);
        }
    }

    /* Something got changed in the hardware. It probably makes sense
     * to save changed hw settings given that hw volume changes not
     * triggered by PA are almost certainly done by the user. */
    if (!(s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER))
        s->save_volume = TRUE;
}

/* Called from io thread */
void pa_source_update_volume_and_mute(pa_source *s) {
    pa_assert(s);
    pa_source_assert_io_context(s);

    pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_UPDATE_VOLUME_AND_MUTE, NULL, 0, NULL, NULL);
}

/* Called from main thread */
const pa_cvolume *pa_source_get_volume(pa_source *s, pa_bool_t force_refresh) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    if (s->refresh_volume || force_refresh) {
        struct pa_cvolume old_real_volume;

        pa_assert(!(s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER));

        old_real_volume = s->real_volume;

        if (!(s->flags & PA_SOURCE_DEFERRED_VOLUME) && s->get_volume)
            s->get_volume(s);

        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_VOLUME, NULL, 0, NULL) == 0);

        update_real_volume(s, &s->real_volume, &s->channel_map);
        propagate_real_volume(s, &old_real_volume);
    }

    return &s->reference_volume;
}

/* Called from main thread. In volume sharing cases, only the root source may
 * call this. */
void pa_source_volume_changed(pa_source *s, const pa_cvolume *new_real_volume) {
    pa_cvolume old_real_volume;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));
    pa_assert(!(s->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER));

    /* The source implementor may call this if the volume changed to make sure everyone is notified */

    old_real_volume = s->real_volume;
    update_real_volume(s, new_real_volume, &s->channel_map);
    propagate_real_volume(s, &old_real_volume);
}

/* Called from main thread */
void pa_source_set_mute(pa_source *s, pa_bool_t mute, pa_bool_t save) {
    pa_bool_t old_muted;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    old_muted = s->muted;
    s->muted = mute;
    s->save_muted = (old_muted == s->muted && s->save_muted) || save;

    if (!(s->flags & PA_SOURCE_DEFERRED_VOLUME) && s->set_mute)
        s->set_mute(s);

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_MUTE, NULL, 0, NULL) == 0);

    if (old_muted != s->muted)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

/* Called from main thread */
pa_bool_t pa_source_get_mute(pa_source *s, pa_bool_t force_refresh) {

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    if (s->refresh_muted || force_refresh) {
        pa_bool_t old_muted = s->muted;

        if (!(s->flags & PA_SOURCE_DEFERRED_VOLUME) && s->get_mute)
            s->get_mute(s);

        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_MUTE, NULL, 0, NULL) == 0);

        if (old_muted != s->muted) {
            s->save_muted = TRUE;

            pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);

            /* Make sure the soft mute status stays in sync */
            pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_MUTE, NULL, 0, NULL) == 0);
        }
    }

    return s->muted;
}

/* Called from main thread */
void pa_source_mute_changed(pa_source *s, pa_bool_t new_muted) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    /* The source implementor may call this if the mute state changed to make sure everyone is notified */

    if (s->muted == new_muted)
        return;

    s->muted = new_muted;
    s->save_muted = TRUE;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

/* Called from main thread */
pa_bool_t pa_source_update_proplist(pa_source *s, pa_update_mode_t mode, pa_proplist *p) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    if (p)
        pa_proplist_update(s->proplist, mode, p);

    if (PA_SOURCE_IS_LINKED(s->state)) {
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_PROPLIST_CHANGED], s);
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
    }

    return TRUE;
}

/* Called from main thread */
/* FIXME -- this should be dropped and be merged into pa_source_update_proplist() */
void pa_source_set_description(pa_source *s, const char *description) {
    const char *old;
    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    if (!description && !pa_proplist_contains(s->proplist, PA_PROP_DEVICE_DESCRIPTION))
        return;

    old = pa_proplist_gets(s->proplist, PA_PROP_DEVICE_DESCRIPTION);

    if (old && description && pa_streq(old, description))
        return;

    if (description)
        pa_proplist_sets(s->proplist, PA_PROP_DEVICE_DESCRIPTION, description);
    else
        pa_proplist_unset(s->proplist, PA_PROP_DEVICE_DESCRIPTION);

    if (PA_SOURCE_IS_LINKED(s->state)) {
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_PROPLIST_CHANGED], s);
    }
}

/* Called from main thread */
unsigned pa_source_linked_by(pa_source *s) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    return pa_idxset_size(s->outputs);
}

/* Called from main thread */
unsigned pa_source_used_by(pa_source *s) {
    unsigned ret;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    ret = pa_idxset_size(s->outputs);
    pa_assert(ret >= s->n_corked);

    return ret - s->n_corked;
}

/* Called from main thread */
unsigned pa_source_check_suspend(pa_source *s) {
    unsigned ret;
    pa_source_output *o;
    uint32_t idx;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    if (!PA_SOURCE_IS_LINKED(s->state))
        return 0;

    ret = 0;

    PA_IDXSET_FOREACH(o, s->outputs, idx) {
        pa_source_output_state_t st;

        st = pa_source_output_get_state(o);

        /* We do not assert here. It is perfectly valid for a source output to
         * be in the INIT state (i.e. created, marked done but not yet put)
         * and we should not care if it's unlinked as it won't contribute
         * towards our busy status.
         */
        if (!PA_SOURCE_OUTPUT_IS_LINKED(st))
            continue;

        if (st == PA_SOURCE_OUTPUT_CORKED)
            continue;

        if (o->flags & PA_SOURCE_OUTPUT_DONT_INHIBIT_AUTO_SUSPEND)
            continue;

        ret ++;
    }

    return ret;
}

/* Called from the IO thread */
static void sync_output_volumes_within_thread(pa_source *s) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);

    PA_HASHMAP_FOREACH(o, s->thread_info.outputs, state) {
        if (pa_cvolume_equal(&o->thread_info.soft_volume, &o->soft_volume))
            continue;

        o->thread_info.soft_volume = o->soft_volume;
        //pa_source_output_request_rewind(o, 0, TRUE, FALSE, FALSE);
    }
}

/* Called from the IO thread. Only called for the root source in volume sharing
 * cases, except for internal recursive calls. */
static void set_shared_volume_within_thread(pa_source *s) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);

    PA_MSGOBJECT(s)->process_msg(PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_VOLUME_SYNCED, NULL, 0, NULL);

    PA_HASHMAP_FOREACH(o, s->thread_info.outputs, state) {
        if (o->destination_source && (o->destination_source->flags & PA_SOURCE_SHARE_VOLUME_WITH_MASTER))
            set_shared_volume_within_thread(o->destination_source);
    }
}

/* Called from IO thread, except when it is not */
int pa_source_process_msg(pa_msgobject *object, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_source *s = PA_SOURCE(object);
    pa_source_assert_ref(s);

    switch ((pa_source_message_t) code) {

        case PA_SOURCE_MESSAGE_ADD_OUTPUT: {
            pa_source_output *o = PA_SOURCE_OUTPUT(userdata);

            pa_hashmap_put(s->thread_info.outputs, PA_UINT32_TO_PTR(o->index), pa_source_output_ref(o));

            if (o->direct_on_input) {
                o->thread_info.direct_on_input = o->direct_on_input;
                pa_hashmap_put(o->thread_info.direct_on_input->thread_info.direct_outputs, PA_UINT32_TO_PTR(o->index), o);
            }

            pa_assert(!o->thread_info.attached);
            o->thread_info.attached = TRUE;

            if (o->attach)
                o->attach(o);

            pa_source_output_set_state_within_thread(o, o->state);

            if (o->thread_info.requested_source_latency != (pa_usec_t) -1)
                pa_source_output_set_requested_latency_within_thread(o, o->thread_info.requested_source_latency);

            pa_source_output_update_max_rewind(o, s->thread_info.max_rewind);

            /* We don't just invalidate the requested latency here,
             * because if we are in a move we might need to fix up the
             * requested latency. */
            pa_source_output_set_requested_latency_within_thread(o, o->thread_info.requested_source_latency);

            /* In flat volume mode we need to update the volume as
             * well */
            return object->process_msg(object, PA_SOURCE_MESSAGE_SET_SHARED_VOLUME, NULL, 0, NULL);
        }

        case PA_SOURCE_MESSAGE_REMOVE_OUTPUT: {
            pa_source_output *o = PA_SOURCE_OUTPUT(userdata);

            pa_source_output_set_state_within_thread(o, o->state);

            if (o->detach)
                o->detach(o);

            pa_assert(o->thread_info.attached);
            o->thread_info.attached = FALSE;

            if (o->thread_info.direct_on_input) {
                pa_hashmap_remove(o->thread_info.direct_on_input->thread_info.direct_outputs, PA_UINT32_TO_PTR(o->index));
                o->thread_info.direct_on_input = NULL;
            }

            if (pa_hashmap_remove(s->thread_info.outputs, PA_UINT32_TO_PTR(o->index)))
                pa_source_output_unref(o);

            pa_source_invalidate_requested_latency(s, TRUE);

            /* In flat volume mode we need to update the volume as
             * well */
            return object->process_msg(object, PA_SOURCE_MESSAGE_SET_SHARED_VOLUME, NULL, 0, NULL);
        }

        case PA_SOURCE_MESSAGE_SET_SHARED_VOLUME: {
            pa_source *root_source = pa_source_get_master(s);

            if (PA_LIKELY(root_source))
                set_shared_volume_within_thread(root_source);

            return 0;
        }

        case PA_SOURCE_MESSAGE_SET_VOLUME_SYNCED:

            if (s->flags & PA_SOURCE_DEFERRED_VOLUME) {
                s->set_volume(s);
                pa_source_volume_change_push(s);
            }
            /* Fall through ... */

        case PA_SOURCE_MESSAGE_SET_VOLUME:

            if (!pa_cvolume_equal(&s->thread_info.soft_volume, &s->soft_volume)) {
                s->thread_info.soft_volume = s->soft_volume;
            }

            /* Fall through ... */

        case PA_SOURCE_MESSAGE_SYNC_VOLUMES:
            sync_output_volumes_within_thread(s);
            return 0;

        case PA_SOURCE_MESSAGE_GET_VOLUME:

            if ((s->flags & PA_SOURCE_DEFERRED_VOLUME) && s->get_volume) {
                s->get_volume(s);
                pa_source_volume_change_flush(s);
                pa_sw_cvolume_divide(&s->thread_info.current_hw_volume, &s->real_volume, &s->soft_volume);
            }

            /* In case source implementor reset SW volume. */
            if (!pa_cvolume_equal(&s->thread_info.soft_volume, &s->soft_volume)) {
                s->thread_info.soft_volume = s->soft_volume;
            }

            return 0;

        case PA_SOURCE_MESSAGE_SET_MUTE:

            if (s->thread_info.soft_muted != s->muted) {
                s->thread_info.soft_muted = s->muted;
            }

            if (s->flags & PA_SOURCE_DEFERRED_VOLUME && s->set_mute)
                s->set_mute(s);

            return 0;

        case PA_SOURCE_MESSAGE_GET_MUTE:

            if (s->flags & PA_SOURCE_DEFERRED_VOLUME && s->get_mute)
                s->get_mute(s);

            return 0;

        case PA_SOURCE_MESSAGE_SET_STATE: {

            pa_bool_t suspend_change =
                (s->thread_info.state == PA_SOURCE_SUSPENDED && PA_SOURCE_IS_OPENED(PA_PTR_TO_UINT(userdata))) ||
                (PA_SOURCE_IS_OPENED(s->thread_info.state) && PA_PTR_TO_UINT(userdata) == PA_SOURCE_SUSPENDED);

            s->thread_info.state = PA_PTR_TO_UINT(userdata);

            if (suspend_change) {
                pa_source_output *o;
                void *state = NULL;

                while ((o = pa_hashmap_iterate(s->thread_info.outputs, &state, NULL)))
                    if (o->suspend_within_thread)
                        o->suspend_within_thread(o, s->thread_info.state == PA_SOURCE_SUSPENDED);
            }

            return 0;
        }

        case PA_SOURCE_MESSAGE_DETACH:

            /* Detach all streams */
            pa_source_detach_within_thread(s);
            return 0;

        case PA_SOURCE_MESSAGE_ATTACH:

            /* Reattach all streams */
            pa_source_attach_within_thread(s);
            return 0;

        case PA_SOURCE_MESSAGE_GET_REQUESTED_LATENCY: {

            pa_usec_t *usec = userdata;
            *usec = pa_source_get_requested_latency_within_thread(s);

            /* Yes, that's right, the IO thread will see -1 when no
             * explicit requested latency is configured, the main
             * thread will see max_latency */
            if (*usec == (pa_usec_t) -1)
                *usec = s->thread_info.max_latency;

            return 0;
        }

        case PA_SOURCE_MESSAGE_SET_LATENCY_RANGE: {
            pa_usec_t *r = userdata;

            pa_source_set_latency_range_within_thread(s, r[0], r[1]);

            return 0;
        }

        case PA_SOURCE_MESSAGE_GET_LATENCY_RANGE: {
            pa_usec_t *r = userdata;

            r[0] = s->thread_info.min_latency;
            r[1] = s->thread_info.max_latency;

            return 0;
        }

        case PA_SOURCE_MESSAGE_GET_FIXED_LATENCY:

            *((pa_usec_t*) userdata) = s->thread_info.fixed_latency;
            return 0;

        case PA_SOURCE_MESSAGE_SET_FIXED_LATENCY:

            pa_source_set_fixed_latency_within_thread(s, (pa_usec_t) offset);
            return 0;

        case PA_SOURCE_MESSAGE_GET_MAX_REWIND:

            *((size_t*) userdata) = s->thread_info.max_rewind;
            return 0;

        case PA_SOURCE_MESSAGE_SET_MAX_REWIND:

            pa_source_set_max_rewind_within_thread(s, (size_t) offset);
            return 0;

        case PA_SOURCE_MESSAGE_GET_LATENCY:

            if (s->monitor_of) {
                *((pa_usec_t*) userdata) = 0;
                return 0;
            }

            /* Implementors need to overwrite this implementation! */
            return -1;

        case PA_SOURCE_MESSAGE_SET_PORT:

            pa_assert(userdata);
            if (s->set_port) {
                struct source_message_set_port *msg_data = userdata;
                msg_data->ret = s->set_port(s, msg_data->port);
            }
            return 0;

        case PA_SOURCE_MESSAGE_UPDATE_VOLUME_AND_MUTE:
            /* This message is sent from IO-thread and handled in main thread. */
            pa_assert_ctl_context();

            /* Make sure we're not messing with main thread when no longer linked */
            if (!PA_SOURCE_IS_LINKED(s->state))
                return 0;

            pa_source_get_volume(s, TRUE);
            pa_source_get_mute(s, TRUE);
            return 0;

        case PA_SOURCE_MESSAGE_SET_LATENCY_OFFSET:
            s->thread_info.latency_offset = offset;
            return 0;

        case PA_SOURCE_MESSAGE_MAX:
            ;
    }

    return -1;
}

/* Called from main thread */
int pa_source_suspend_all(pa_core *c, pa_bool_t suspend, pa_suspend_cause_t cause) {
    pa_source *source;
    uint32_t idx;
    int ret = 0;

    pa_core_assert_ref(c);
    pa_assert_ctl_context();
    pa_assert(cause != 0);

    for (source = PA_SOURCE(pa_idxset_first(c->sources, &idx)); source; source = PA_SOURCE(pa_idxset_next(c->sources, &idx))) {
        int r;

        if (source->monitor_of)
            continue;

        if ((r = pa_source_suspend(source, suspend, cause)) < 0)
            ret = r;
    }

    return ret;
}

/* Called from main thread */
void pa_source_detach(pa_source *s) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_DETACH, NULL, 0, NULL) == 0);
}

/* Called from main thread */
void pa_source_attach(pa_source *s) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_ATTACH, NULL, 0, NULL) == 0);
}

/* Called from IO thread */
void pa_source_detach_within_thread(pa_source *s) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);
    pa_assert(PA_SOURCE_IS_LINKED(s->thread_info.state));

    PA_HASHMAP_FOREACH(o, s->thread_info.outputs, state)
        if (o->detach)
            o->detach(o);
}

/* Called from IO thread */
void pa_source_attach_within_thread(pa_source *s) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);
    pa_assert(PA_SOURCE_IS_LINKED(s->thread_info.state));

    PA_HASHMAP_FOREACH(o, s->thread_info.outputs, state)
        if (o->attach)
            o->attach(o);
}

/* Called from IO thread */
pa_usec_t pa_source_get_requested_latency_within_thread(pa_source *s) {
    pa_usec_t result = (pa_usec_t) -1;
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);

    if (!(s->flags & PA_SOURCE_DYNAMIC_LATENCY))
        return PA_CLAMP(s->thread_info.fixed_latency, s->thread_info.min_latency, s->thread_info.max_latency);

    if (s->thread_info.requested_latency_valid)
        return s->thread_info.requested_latency;

    PA_HASHMAP_FOREACH(o, s->thread_info.outputs, state)
        if (o->thread_info.requested_source_latency != (pa_usec_t) -1 &&
            (result == (pa_usec_t) -1 || result > o->thread_info.requested_source_latency))
            result = o->thread_info.requested_source_latency;

    if (result != (pa_usec_t) -1)
        result = PA_CLAMP(result, s->thread_info.min_latency, s->thread_info.max_latency);

    if (PA_SOURCE_IS_LINKED(s->thread_info.state)) {
        /* Only cache this if we are fully set up */
        s->thread_info.requested_latency = result;
        s->thread_info.requested_latency_valid = TRUE;
    }

    return result;
}

/* Called from main thread */
pa_usec_t pa_source_get_requested_latency(pa_source *s) {
    pa_usec_t usec = 0;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(PA_SOURCE_IS_LINKED(s->state));

    if (s->state == PA_SOURCE_SUSPENDED)
        return 0;

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_REQUESTED_LATENCY, &usec, 0, NULL) == 0);

    return usec;
}

/* Called from IO thread */
void pa_source_set_max_rewind_within_thread(pa_source *s, size_t max_rewind) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);

    if (max_rewind == s->thread_info.max_rewind)
        return;

    s->thread_info.max_rewind = max_rewind;

    if (PA_SOURCE_IS_LINKED(s->thread_info.state))
        PA_HASHMAP_FOREACH(o, s->thread_info.outputs, state)
            pa_source_output_update_max_rewind(o, s->thread_info.max_rewind);
}

/* Called from main thread */
void pa_source_set_max_rewind(pa_source *s, size_t max_rewind) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    if (PA_SOURCE_IS_LINKED(s->state))
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_MAX_REWIND, NULL, max_rewind, NULL) == 0);
    else
        pa_source_set_max_rewind_within_thread(s, max_rewind);
}

/* Called from IO thread */
void pa_source_invalidate_requested_latency(pa_source *s, pa_bool_t dynamic) {
    pa_source_output *o;
    void *state = NULL;

    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);

    if ((s->flags & PA_SOURCE_DYNAMIC_LATENCY))
        s->thread_info.requested_latency_valid = FALSE;
    else if (dynamic)
        return;

    if (PA_SOURCE_IS_LINKED(s->thread_info.state)) {

        if (s->update_requested_latency)
            s->update_requested_latency(s);

        while ((o = pa_hashmap_iterate(s->thread_info.outputs, &state, NULL)))
            if (o->update_source_requested_latency)
                o->update_source_requested_latency(o);
    }

    if (s->monitor_of)
        pa_sink_invalidate_requested_latency(s->monitor_of, dynamic);
}

/* Called from main thread */
void pa_source_set_latency_range(pa_source *s, pa_usec_t min_latency, pa_usec_t max_latency) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    /* min_latency == 0:           no limit
     * min_latency anything else:  specified limit
     *
     * Similar for max_latency */

    if (min_latency < ABSOLUTE_MIN_LATENCY)
        min_latency = ABSOLUTE_MIN_LATENCY;

    if (max_latency <= 0 ||
        max_latency > ABSOLUTE_MAX_LATENCY)
        max_latency = ABSOLUTE_MAX_LATENCY;

    pa_assert(min_latency <= max_latency);

    /* Hmm, let's see if someone forgot to set PA_SOURCE_DYNAMIC_LATENCY here... */
    pa_assert((min_latency == ABSOLUTE_MIN_LATENCY &&
               max_latency == ABSOLUTE_MAX_LATENCY) ||
              (s->flags & PA_SOURCE_DYNAMIC_LATENCY));

    if (PA_SOURCE_IS_LINKED(s->state)) {
        pa_usec_t r[2];

        r[0] = min_latency;
        r[1] = max_latency;

        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_LATENCY_RANGE, r, 0, NULL) == 0);
    } else
        pa_source_set_latency_range_within_thread(s, min_latency, max_latency);
}

/* Called from main thread */
void pa_source_get_latency_range(pa_source *s, pa_usec_t *min_latency, pa_usec_t *max_latency) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();
    pa_assert(min_latency);
    pa_assert(max_latency);

    if (PA_SOURCE_IS_LINKED(s->state)) {
        pa_usec_t r[2] = { 0, 0 };

        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_LATENCY_RANGE, r, 0, NULL) == 0);

        *min_latency = r[0];
        *max_latency = r[1];
    } else {
        *min_latency = s->thread_info.min_latency;
        *max_latency = s->thread_info.max_latency;
    }
}

/* Called from IO thread, and from main thread before pa_source_put() is called */
void pa_source_set_latency_range_within_thread(pa_source *s, pa_usec_t min_latency, pa_usec_t max_latency) {
    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);

    pa_assert(min_latency >= ABSOLUTE_MIN_LATENCY);
    pa_assert(max_latency <= ABSOLUTE_MAX_LATENCY);
    pa_assert(min_latency <= max_latency);

    /* Hmm, let's see if someone forgot to set PA_SOURCE_DYNAMIC_LATENCY here... */
    pa_assert((min_latency == ABSOLUTE_MIN_LATENCY &&
               max_latency == ABSOLUTE_MAX_LATENCY) ||
              (s->flags & PA_SOURCE_DYNAMIC_LATENCY) ||
              s->monitor_of);

    if (s->thread_info.min_latency == min_latency &&
        s->thread_info.max_latency == max_latency)
        return;

    s->thread_info.min_latency = min_latency;
    s->thread_info.max_latency = max_latency;

    if (PA_SOURCE_IS_LINKED(s->thread_info.state)) {
        pa_source_output *o;
        void *state = NULL;

        PA_HASHMAP_FOREACH(o, s->thread_info.outputs, state)
            if (o->update_source_latency_range)
                o->update_source_latency_range(o);
    }

    pa_source_invalidate_requested_latency(s, FALSE);
}

/* Called from main thread, before the source is put */
void pa_source_set_fixed_latency(pa_source *s, pa_usec_t latency) {
    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    if (s->flags & PA_SOURCE_DYNAMIC_LATENCY) {
        pa_assert(latency == 0);
        return;
    }

    if (latency < ABSOLUTE_MIN_LATENCY)
        latency = ABSOLUTE_MIN_LATENCY;

    if (latency > ABSOLUTE_MAX_LATENCY)
        latency = ABSOLUTE_MAX_LATENCY;

    if (PA_SOURCE_IS_LINKED(s->state))
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_FIXED_LATENCY, NULL, (int64_t) latency, NULL) == 0);
    else
        s->thread_info.fixed_latency = latency;
}

/* Called from main thread */
pa_usec_t pa_source_get_fixed_latency(pa_source *s) {
    pa_usec_t latency;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    if (s->flags & PA_SOURCE_DYNAMIC_LATENCY)
        return 0;

    if (PA_SOURCE_IS_LINKED(s->state))
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_FIXED_LATENCY, &latency, 0, NULL) == 0);
    else
        latency = s->thread_info.fixed_latency;

    return latency;
}

/* Called from IO thread */
void pa_source_set_fixed_latency_within_thread(pa_source *s, pa_usec_t latency) {
    pa_source_assert_ref(s);
    pa_source_assert_io_context(s);

    if (s->flags & PA_SOURCE_DYNAMIC_LATENCY) {
        pa_assert(latency == 0);
        s->thread_info.fixed_latency = 0;

        return;
    }

    pa_assert(latency >= ABSOLUTE_MIN_LATENCY);
    pa_assert(latency <= ABSOLUTE_MAX_LATENCY);

    if (s->thread_info.fixed_latency == latency)
        return;

    s->thread_info.fixed_latency = latency;

    if (PA_SOURCE_IS_LINKED(s->thread_info.state)) {
        pa_source_output *o;
        void *state = NULL;

        PA_HASHMAP_FOREACH(o, s->thread_info.outputs, state)
            if (o->update_source_fixed_latency)
                o->update_source_fixed_latency(o);
    }

    pa_source_invalidate_requested_latency(s, FALSE);
}

/* Called from main thread */
void pa_source_set_latency_offset(pa_source *s, int64_t offset) {
    pa_source_assert_ref(s);

    s->latency_offset = offset;

    if (PA_SOURCE_IS_LINKED(s->state))
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_LATENCY_OFFSET, NULL, offset, NULL) == 0);
    else
        s->thread_info.latency_offset = offset;
}

/* Called from main thread */
size_t pa_source_get_max_rewind(pa_source *s) {
    size_t r;
    pa_assert_ctl_context();
    pa_source_assert_ref(s);

    if (!PA_SOURCE_IS_LINKED(s->state))
        return s->thread_info.max_rewind;

    pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_GET_MAX_REWIND, &r, 0, NULL) == 0);

    return r;
}

/* Called from main context */
int pa_source_set_port(pa_source *s, const char *name, pa_bool_t save) {
    pa_device_port *port;
    int ret;

    pa_source_assert_ref(s);
    pa_assert_ctl_context();

    if (!s->set_port) {
        pa_log_debug("set_port() operation not implemented for source %u \"%s\"", s->index, s->name);
        return -PA_ERR_NOTIMPLEMENTED;
    }

    if (!name)
        return -PA_ERR_NOENTITY;

    if (!(port = pa_hashmap_get(s->ports, name)))
        return -PA_ERR_NOENTITY;

    if (s->active_port == port) {
        s->save_port = s->save_port || save;
        return 0;
    }

    if (s->flags & PA_SOURCE_DEFERRED_VOLUME) {
        struct source_message_set_port msg = { .port = port, .ret = 0 };
        pa_assert_se(pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SOURCE_MESSAGE_SET_PORT, &msg, 0, NULL) == 0);
        ret = msg.ret;
    }
    else
        ret = s->set_port(s, port);

    if (ret < 0)
        return -PA_ERR_NOENTITY;

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SOURCE|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);

    pa_log_info("Changed port of source %u \"%s\" to %s", s->index, s->name, port->name);

    s->active_port = port;
    s->save_port = save;

    pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SOURCE_PORT_CHANGED], s);

    return 0;
}

PA_STATIC_FLIST_DECLARE(pa_source_volume_change, 0, pa_xfree);

/* Called from the IO thread. */
static pa_source_volume_change *pa_source_volume_change_new(pa_source *s) {
    pa_source_volume_change *c;
    if (!(c = pa_flist_pop(PA_STATIC_FLIST_GET(pa_source_volume_change))))
        c = pa_xnew(pa_source_volume_change, 1);

    PA_LLIST_INIT(pa_source_volume_change, c);
    c->at = 0;
    pa_cvolume_reset(&c->hw_volume, s->sample_spec.channels);
    return c;
}

/* Called from the IO thread. */
static void pa_source_volume_change_free(pa_source_volume_change *c) {
    pa_assert(c);
    if (pa_flist_push(PA_STATIC_FLIST_GET(pa_source_volume_change), c) < 0)
        pa_xfree(c);
}

/* Called from the IO thread. */
void pa_source_volume_change_push(pa_source *s) {
    pa_source_volume_change *c = NULL;
    pa_source_volume_change *nc = NULL;
    uint32_t safety_margin = s->thread_info.volume_change_safety_margin;

    const char *direction = NULL;

    pa_assert(s);
    nc = pa_source_volume_change_new(s);

    /* NOTE: There is already more different volumes in pa_source that I can remember.
     *       Adding one more volume for HW would get us rid of this, but I am trying
     *       to survive with the ones we already have. */
    pa_sw_cvolume_divide(&nc->hw_volume, &s->real_volume, &s->soft_volume);

    if (!s->thread_info.volume_changes && pa_cvolume_equal(&nc->hw_volume, &s->thread_info.current_hw_volume)) {
        pa_log_debug("Volume not changing");
        pa_source_volume_change_free(nc);
        return;
    }

    nc->at = pa_source_get_latency_within_thread(s);
    nc->at += pa_rtclock_now() + s->thread_info.volume_change_extra_delay;

    if (s->thread_info.volume_changes_tail) {
        for (c = s->thread_info.volume_changes_tail; c; c = c->prev) {
            /* If volume is going up let's do it a bit late. If it is going
             * down let's do it a bit early. */
            if (pa_cvolume_avg(&nc->hw_volume) > pa_cvolume_avg(&c->hw_volume)) {
                if (nc->at + safety_margin > c->at) {
                    nc->at += safety_margin;
                    direction = "up";
                    break;
                }
            }
            else if (nc->at - safety_margin > c->at) {
                    nc->at -= safety_margin;
                    direction = "down";
                    break;
            }
        }
    }

    if (c == NULL) {
        if (pa_cvolume_avg(&nc->hw_volume) > pa_cvolume_avg(&s->thread_info.current_hw_volume)) {
            nc->at += safety_margin;
            direction = "up";
        } else {
            nc->at -= safety_margin;
            direction = "down";
        }
        PA_LLIST_PREPEND(pa_source_volume_change, s->thread_info.volume_changes, nc);
    }
    else {
        PA_LLIST_INSERT_AFTER(pa_source_volume_change, s->thread_info.volume_changes, c, nc);
    }

    pa_log_debug("Volume going %s to %d at %llu", direction, pa_cvolume_avg(&nc->hw_volume), (long long unsigned) nc->at);

    /* We can ignore volume events that came earlier but should happen later than this. */
    PA_LLIST_FOREACH(c, nc->next) {
        pa_log_debug("Volume change to %d at %llu was dropped", pa_cvolume_avg(&c->hw_volume), (long long unsigned) c->at);
        pa_source_volume_change_free(c);
    }
    nc->next = NULL;
    s->thread_info.volume_changes_tail = nc;
}

/* Called from the IO thread. */
static void pa_source_volume_change_flush(pa_source *s) {
    pa_source_volume_change *c = s->thread_info.volume_changes;
    pa_assert(s);
    s->thread_info.volume_changes = NULL;
    s->thread_info.volume_changes_tail = NULL;
    while (c) {
        pa_source_volume_change *next = c->next;
        pa_source_volume_change_free(c);
        c = next;
    }
}

/* Called from the IO thread. */
pa_bool_t pa_source_volume_change_apply(pa_source *s, pa_usec_t *usec_to_next) {
    pa_usec_t now;
    pa_bool_t ret = FALSE;

    pa_assert(s);

    if (!s->thread_info.volume_changes || !PA_SOURCE_IS_LINKED(s->state)) {
        if (usec_to_next)
            *usec_to_next = 0;
        return ret;
    }

    pa_assert(s->write_volume);

    now = pa_rtclock_now();

    while (s->thread_info.volume_changes && now >= s->thread_info.volume_changes->at) {
        pa_source_volume_change *c = s->thread_info.volume_changes;
        PA_LLIST_REMOVE(pa_source_volume_change, s->thread_info.volume_changes, c);
        pa_log_debug("Volume change to %d at %llu was written %llu usec late",
                     pa_cvolume_avg(&c->hw_volume), (long long unsigned) c->at, (long long unsigned) (now - c->at));
        ret = TRUE;
        s->thread_info.current_hw_volume = c->hw_volume;
        pa_source_volume_change_free(c);
    }

    if (ret)
        s->write_volume(s);

    if (s->thread_info.volume_changes) {
        if (usec_to_next)
            *usec_to_next = s->thread_info.volume_changes->at - now;
        if (pa_log_ratelimit(PA_LOG_DEBUG))
            pa_log_debug("Next volume change in %lld usec", (long long) (s->thread_info.volume_changes->at - now));
    }
    else {
        if (usec_to_next)
            *usec_to_next = 0;
        s->thread_info.volume_changes_tail = NULL;
    }
    return ret;
}


/* Called from the main thread */
/* Gets the list of formats supported by the source. The members and idxset must
 * be freed by the caller. */
pa_idxset* pa_source_get_formats(pa_source *s) {
    pa_idxset *ret;

    pa_assert(s);

    if (s->get_formats) {
        /* Source supports format query, all is good */
        ret = s->get_formats(s);
    } else {
        /* Source doesn't support format query, so assume it does PCM */
        pa_format_info *f = pa_format_info_new();
        f->encoding = PA_ENCODING_PCM;

        ret = pa_idxset_new(NULL, NULL);
        pa_idxset_put(ret, f, NULL);
    }

    return ret;
}

/* Called from the main thread */
/* Checks if the source can accept this format */
pa_bool_t pa_source_check_format(pa_source *s, pa_format_info *f)
{
    pa_idxset *formats = NULL;
    pa_bool_t ret = FALSE;

    pa_assert(s);
    pa_assert(f);

    formats = pa_source_get_formats(s);

    if (formats) {
        pa_format_info *finfo_device;
        uint32_t i;

        PA_IDXSET_FOREACH(finfo_device, formats, i) {
            if (pa_format_info_is_compatible(finfo_device, f)) {
                ret = TRUE;
                break;
            }
        }

        pa_idxset_free(formats, (pa_free_cb_t) pa_format_info_free);
    }

    return ret;
}

/* Called from the main thread */
/* Calculates the intersection between formats supported by the source and
 * in_formats, and returns these, in the order of the source's formats. */
pa_idxset* pa_source_check_formats(pa_source *s, pa_idxset *in_formats) {
    pa_idxset *out_formats = pa_idxset_new(NULL, NULL), *source_formats = NULL;
    pa_format_info *f_source, *f_in;
    uint32_t i, j;

    pa_assert(s);

    if (!in_formats || pa_idxset_isempty(in_formats))
        goto done;

    source_formats = pa_source_get_formats(s);

    PA_IDXSET_FOREACH(f_source, source_formats, i) {
        PA_IDXSET_FOREACH(f_in, in_formats, j) {
            if (pa_format_info_is_compatible(f_source, f_in))
                pa_idxset_put(out_formats, pa_format_info_copy(f_in), NULL);
        }
    }

done:
    if (source_formats)
        pa_idxset_free(source_formats, (pa_free_cb_t) pa_format_info_free);

    return out_formats;
}
