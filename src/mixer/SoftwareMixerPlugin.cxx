/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "SoftwareMixerPlugin.hxx"
#include "MixerInternal.hxx"
#include "FilterPlugin.hxx"
#include "FilterRegistry.hxx"
#include "FilterInternal.hxx"
#include "filter/VolumeFilterPlugin.hxx"
#include "pcm/PcmVolume.hxx"

#include <assert.h>
#include <math.h>

struct SoftwareMixer final : public mixer {
	Filter *filter;

	unsigned volume;

	SoftwareMixer()
		:filter(filter_new(&volume_filter_plugin, nullptr, nullptr)),
		 volume(100)
	{
		assert(filter != nullptr);

		mixer_init(this, &software_mixer_plugin);
	}

	~SoftwareMixer() {
		delete filter;
	}
};

static struct mixer *
software_mixer_init(G_GNUC_UNUSED void *ao,
		    G_GNUC_UNUSED const struct config_param *param,
		    G_GNUC_UNUSED GError **error_r)
{
	return new SoftwareMixer();
}

static void
software_mixer_finish(struct mixer *data)
{
	SoftwareMixer *sm = (SoftwareMixer *)data;

	delete sm;
}

static int
software_mixer_get_volume(struct mixer *mixer, G_GNUC_UNUSED GError **error_r)
{
	SoftwareMixer *sm = (SoftwareMixer *)mixer;

	return sm->volume;
}

static bool
software_mixer_set_volume(struct mixer *mixer, unsigned volume,
			  G_GNUC_UNUSED GError **error_r)
{
	SoftwareMixer *sm = (SoftwareMixer *)mixer;

	assert(volume <= 100);

	sm->volume = volume;

	if (volume >= 100)
		volume = PCM_VOLUME_1;
	else if (volume > 0)
		volume = pcm_float_to_volume((exp(volume / 25.0) - 1) /
					     (54.5981500331F - 1));

	volume_filter_set(sm->filter, volume);
	return true;
}

const struct mixer_plugin software_mixer_plugin = {
	software_mixer_init,
	software_mixer_finish,
	nullptr,
	nullptr,
	software_mixer_get_volume,
	software_mixer_set_volume,
	true,
};

Filter *
software_mixer_get_filter(struct mixer *mixer)
{
	SoftwareMixer *sm = (SoftwareMixer *)mixer;

	assert(sm->plugin == &software_mixer_plugin);

	return sm->filter;
}
