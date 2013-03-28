#ifndef fooalsamixerhfoo
#define fooalsamixerhfoo

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

#include <asoundlib.h>

#include <pulse/sample.h>
#include <pulse/mainloop-api.h>
#include <pulse/channelmap.h>
#include <pulse/volume.h>

#include <pulsecore/llist.h>
#include <pulsecore/rtpoll.h>

typedef struct pa_alsa_fdlist pa_alsa_fdlist;
typedef struct pa_alsa_mixer_pdata pa_alsa_mixer_pdata;
typedef struct pa_alsa_setting pa_alsa_setting;
typedef struct pa_alsa_option pa_alsa_option;
typedef struct pa_alsa_element pa_alsa_element;
typedef struct pa_alsa_jack pa_alsa_jack;
typedef struct pa_alsa_path pa_alsa_path;
typedef struct pa_alsa_path_set pa_alsa_path_set;
typedef struct pa_alsa_mapping pa_alsa_mapping;
typedef struct pa_alsa_profile pa_alsa_profile;
typedef struct pa_alsa_decibel_fix pa_alsa_decibel_fix;
typedef struct pa_alsa_profile_set pa_alsa_profile_set;
typedef struct pa_alsa_port_data pa_alsa_port_data;

#include "alsa-util.h"
#include "alsa-ucm.h"

typedef enum pa_alsa_switch_use {
    PA_ALSA_SWITCH_IGNORE,
    PA_ALSA_SWITCH_MUTE,   /* make this switch follow mute status */
    PA_ALSA_SWITCH_OFF,    /* set this switch to 'off' unconditionally */
    PA_ALSA_SWITCH_ON,     /* set this switch to 'on' unconditionally */
    PA_ALSA_SWITCH_SELECT  /* allow the user to select switch status through a setting */
} pa_alsa_switch_use_t;

typedef enum pa_alsa_volume_use {
    PA_ALSA_VOLUME_IGNORE,
    PA_ALSA_VOLUME_MERGE,   /* merge this volume slider into the global volume slider */
    PA_ALSA_VOLUME_OFF,     /* set this volume to minimal unconditionally */
    PA_ALSA_VOLUME_ZERO,    /* set this volume to 0dB unconditionally */
    PA_ALSA_VOLUME_CONSTANT /* set this volume to a constant value unconditionally */
} pa_alsa_volume_use_t;

typedef enum pa_alsa_enumeration_use {
    PA_ALSA_ENUMERATION_IGNORE,
    PA_ALSA_ENUMERATION_SELECT
} pa_alsa_enumeration_use_t;

typedef enum pa_alsa_required {
    PA_ALSA_REQUIRED_IGNORE,
    PA_ALSA_REQUIRED_SWITCH,
    PA_ALSA_REQUIRED_VOLUME,
    PA_ALSA_REQUIRED_ENUMERATION,
    PA_ALSA_REQUIRED_ANY
} pa_alsa_required_t;

typedef enum pa_alsa_direction {
    PA_ALSA_DIRECTION_ANY,
    PA_ALSA_DIRECTION_OUTPUT,
    PA_ALSA_DIRECTION_INPUT
} pa_alsa_direction_t;

/* A setting combines a couple of options into a single entity that
 * may be selected. Only one setting can be active at the same
 * time. */
struct pa_alsa_setting {
    pa_alsa_path *path;
    PA_LLIST_FIELDS(pa_alsa_setting);

    pa_idxset *options;

    char *name;
    char *description;
    unsigned priority;
};

/* An option belongs to an element and refers to one enumeration item
 * of the element is an enumeration item, or a switch status if the
 * element is a switch item. */
struct pa_alsa_option {
    pa_alsa_element *element;
    PA_LLIST_FIELDS(pa_alsa_option);

    char *alsa_name;
    int alsa_idx;

    char *name;
    char *description;
    unsigned priority;

    pa_alsa_required_t required;
    pa_alsa_required_t required_any;
    pa_alsa_required_t required_absent;
};

/* An element wraps one specific ALSA element. A series of elements
 * make up a path (see below). If the element is an enumeration or switch
 * element it may include a list of options. */
struct pa_alsa_element {
    pa_alsa_path *path;
    PA_LLIST_FIELDS(pa_alsa_element);

    char *alsa_name;
    pa_alsa_direction_t direction;

    pa_alsa_switch_use_t switch_use;
    pa_alsa_volume_use_t volume_use;
    pa_alsa_enumeration_use_t enumeration_use;

    pa_alsa_required_t required;
    pa_alsa_required_t required_any;
    pa_alsa_required_t required_absent;

    long constant_volume;

    pa_bool_t override_map:1;
    pa_bool_t direction_try_other:1;

    pa_bool_t has_dB:1;
    long min_volume, max_volume;
    long volume_limit; /* -1 for no configured limit */
    double min_dB, max_dB;

    pa_channel_position_mask_t masks[SND_MIXER_SCHN_LAST + 1][2];
    unsigned n_channels;

    pa_channel_position_mask_t merged_mask;

    PA_LLIST_HEAD(pa_alsa_option, options);

    pa_alsa_decibel_fix *db_fix;
};

struct pa_alsa_jack {
    pa_alsa_path *path;
    PA_LLIST_FIELDS(pa_alsa_jack);

    char *name; /* E g "Headphone" */
    char *alsa_name; /* E g "Headphone Jack" */
    pa_bool_t has_control; /* is the jack itself present? */
    pa_bool_t plugged_in; /* is this jack currently plugged in? */
    snd_hctl_elem_t *hctl_elem; /* Jack detection handle */
    pa_available_t state_unplugged, state_plugged;

    pa_alsa_required_t required;
    pa_alsa_required_t required_any;
    pa_alsa_required_t required_absent;
};

/* A path wraps a series of elements into a single entity which can be
 * used to control it as if it had a single volume slider, a single
 * mute switch and a single list of selectable options. */
struct pa_alsa_path {
    pa_alsa_direction_t direction;
    pa_device_port* port;

    char *name;
    char *description_key;
    char *description;
    unsigned priority;
    int eld_device;
    pa_proplist *proplist;

    pa_bool_t probed:1;
    pa_bool_t supported:1;
    pa_bool_t has_mute:1;
    pa_bool_t has_volume:1;
    pa_bool_t has_dB:1;
    bool mute_during_activation:1;
    /* These two are used during probing only */
    pa_bool_t has_req_any:1;
    pa_bool_t req_any_present:1;

    long min_volume, max_volume;
    double min_dB, max_dB;

    /* This is used during parsing only, as a shortcut so that we
     * don't have to iterate the list all the time */
    pa_alsa_element *last_element;
    pa_alsa_option *last_option;
    pa_alsa_setting *last_setting;
    pa_alsa_jack *last_jack;

    PA_LLIST_HEAD(pa_alsa_element, elements);
    PA_LLIST_HEAD(pa_alsa_setting, settings);
    PA_LLIST_HEAD(pa_alsa_jack, jacks);
};

/* A path set is simply a set of paths that are applicable to a
 * device */
struct pa_alsa_path_set {
    pa_hashmap *paths;
    pa_alsa_direction_t direction;
};

void pa_alsa_setting_dump(pa_alsa_setting *s);

void pa_alsa_option_dump(pa_alsa_option *o);
void pa_alsa_jack_dump(pa_alsa_jack *j);
void pa_alsa_element_dump(pa_alsa_element *e);

pa_alsa_path *pa_alsa_path_new(const char *paths_dir, const char *fname, pa_alsa_direction_t direction);
pa_alsa_path *pa_alsa_path_synthesize(const char *element, pa_alsa_direction_t direction);
int pa_alsa_path_probe(pa_alsa_path *p, snd_mixer_t *m, snd_hctl_t *hctl, pa_bool_t ignore_dB);
void pa_alsa_path_dump(pa_alsa_path *p);
int pa_alsa_path_get_volume(pa_alsa_path *p, snd_mixer_t *m, const pa_channel_map *cm, pa_cvolume *v);
int pa_alsa_path_get_mute(pa_alsa_path *path, snd_mixer_t *m, pa_bool_t *muted);
int pa_alsa_path_set_volume(pa_alsa_path *path, snd_mixer_t *m, const pa_channel_map *cm, pa_cvolume *v, pa_bool_t deferred_volume, pa_bool_t write_to_hw);
int pa_alsa_path_set_mute(pa_alsa_path *path, snd_mixer_t *m, pa_bool_t muted);
int pa_alsa_path_select(pa_alsa_path *p, pa_alsa_setting *s, snd_mixer_t *m, bool device_is_muted);
void pa_alsa_path_set_callback(pa_alsa_path *p, snd_mixer_t *m, snd_mixer_elem_callback_t cb, void *userdata);
void pa_alsa_path_free(pa_alsa_path *p);

pa_alsa_path_set *pa_alsa_path_set_new(pa_alsa_mapping *m, pa_alsa_direction_t direction, const char *paths_dir);
void pa_alsa_path_set_dump(pa_alsa_path_set *s);
void pa_alsa_path_set_set_callback(pa_alsa_path_set *ps, snd_mixer_t *m, snd_mixer_elem_callback_t cb, void *userdata);
void pa_alsa_path_set_free(pa_alsa_path_set *s);

struct pa_alsa_mapping {
    pa_alsa_profile_set *profile_set;

    char *name;
    char *description;
    unsigned priority;
    pa_alsa_direction_t direction;
    /* These are copied over to the resultant sink/source */
    pa_proplist *proplist;

    pa_channel_map channel_map;

    char **device_strings;

    char **input_path_names;
    char **output_path_names;
    char **input_element; /* list of fallbacks */
    char **output_element;
    pa_alsa_path_set *input_path_set;
    pa_alsa_path_set *output_path_set;

    unsigned supported;

    /* Temporarily used during probing */
    snd_pcm_t *input_pcm;
    snd_pcm_t *output_pcm;

    pa_sink *sink;
    pa_source *source;

    /* ucm device context*/
    pa_alsa_ucm_mapping_context ucm_context;
};

struct pa_alsa_profile {
    pa_alsa_profile_set *profile_set;

    char *name;
    char *description;
    unsigned priority;

    pa_bool_t supported:1;

    char **input_mapping_names;
    char **output_mapping_names;

    pa_idxset *input_mappings;
    pa_idxset *output_mappings;
};

struct pa_alsa_decibel_fix {
    pa_alsa_profile_set *profile_set;

    char *name; /* Alsa volume element name. */
    long min_step;
    long max_step;

    /* An array that maps alsa volume element steps to decibels. The steps can
     * be used as indices to this array, after subtracting min_step from the
     * real value.
     *
     * The values are actually stored as integers representing millibels,
     * because that's the format the alsa API uses. */
    long *db_values;
};

struct pa_alsa_profile_set {
    pa_hashmap *mappings;
    pa_hashmap *profiles;
    pa_hashmap *decibel_fixes;
    pa_hashmap *input_paths;
    pa_hashmap *output_paths;

    pa_bool_t auto_profiles;
    pa_bool_t ignore_dB:1;
    pa_bool_t probed:1;
};

void pa_alsa_mapping_dump(pa_alsa_mapping *m);
void pa_alsa_profile_dump(pa_alsa_profile *p);
void pa_alsa_decibel_fix_dump(pa_alsa_decibel_fix *db_fix);
pa_alsa_mapping *pa_alsa_mapping_get(pa_alsa_profile_set *ps, const char *name);

pa_alsa_profile_set* pa_alsa_profile_set_new(const char *fname, const pa_channel_map *bonus);
void pa_alsa_profile_set_probe(pa_alsa_profile_set *ps, const char *dev_id, const pa_sample_spec *ss, unsigned default_n_fragments, unsigned default_fragment_size_msec);
void pa_alsa_profile_set_free(pa_alsa_profile_set *s);
void pa_alsa_profile_set_dump(pa_alsa_profile_set *s);
void pa_alsa_profile_set_drop_unsupported(pa_alsa_profile_set *s);

snd_mixer_t *pa_alsa_open_mixer_for_pcm(snd_pcm_t *pcm, char **ctl_device, snd_hctl_t **hctl);

pa_alsa_fdlist *pa_alsa_fdlist_new(void);
void pa_alsa_fdlist_free(pa_alsa_fdlist *fdl);
int pa_alsa_fdlist_set_handle(pa_alsa_fdlist *fdl, snd_mixer_t *mixer_handle, snd_hctl_t *hctl_handle, pa_mainloop_api* m);

/* Alternative for handling alsa mixer events in io-thread. */

pa_alsa_mixer_pdata *pa_alsa_mixer_pdata_new(void);
void pa_alsa_mixer_pdata_free(pa_alsa_mixer_pdata *pd);
int pa_alsa_set_mixer_rtpoll(struct pa_alsa_mixer_pdata *pd, snd_mixer_t *mixer, pa_rtpoll *rtp);

/* Data structure for inclusion in pa_device_port for alsa
 * sinks/sources. This contains nothing that needs to be freed
 * individually */
struct pa_alsa_port_data {
    pa_alsa_path *path;
    pa_alsa_setting *setting;
};

void pa_alsa_add_ports(void *sink_or_source_new_data, pa_alsa_path_set *ps, pa_card *card);
void pa_alsa_path_set_add_ports(pa_alsa_path_set *ps, pa_card_profile *cp, pa_hashmap *ports, pa_hashmap *extra, pa_core *core);

#endif
