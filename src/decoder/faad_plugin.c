/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "../decoder_api.h"
#include "decoder_buffer.h"
#include "config.h"

#define AAC_MAX_CHANNELS	6

#include <assert.h>
#include <unistd.h>
#include <faad.h>
#include <glib.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "faad"

static const unsigned adts_sample_rates[] =
    { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
	16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

/**
 * Check whether the buffer head is an AAC frame, and return the frame
 * length.  Returns 0 if it is not a frame.
 */
static size_t
adts_check_frame(const unsigned char *data)
{
	/* check syncword */
	if (!((data[0] == 0xFF) && ((data[1] & 0xF6) == 0xF0)))
		return 0;

	return (((unsigned int)data[3] & 0x3) << 11) |
		(((unsigned int)data[4]) << 3) |
		(data[5] >> 5);
}

/**
 * Find the next AAC frame in the buffer.  Returns 0 if no frame is
 * found or if not enough data is available.
 */
static size_t
adts_find_frame(struct decoder_buffer *buffer)
{
	const unsigned char *data, *p;
	size_t length, frame_length;
	bool ret;

	while (true) {
		data = decoder_buffer_read(buffer, &length);
		if (data == NULL || length < 8) {
			/* not enough data yet */
			ret = decoder_buffer_fill(buffer);
			if (!ret)
				/* failed */
				return 0;

			continue;
		}

		/* find the 0xff marker */
		p = memchr(data, 0xff, length);
		if (p == NULL) {
			/* no marker - discard the buffer */
			decoder_buffer_consume(buffer, length);
			continue;
		}

		if (p > data) {
			/* discard data before 0xff */
			decoder_buffer_consume(buffer, p - data);
			continue;
		}

		/* is it a frame? */
		frame_length = adts_check_frame(data);
		if (frame_length == 0) {
			/* it's just some random 0xff byte; discard it
			   and continue searching */
			decoder_buffer_consume(buffer, 1);
			continue;
		}

		if (length < frame_length) {
			/* available buffer size is smaller than the
			   frame will be - attempt to read more
			   data */
			ret = decoder_buffer_fill(buffer);
			if (!ret) {
				/* not enough data; discard this frame
				   to prevent a possible buffer
				   overflow */
				data = decoder_buffer_read(buffer, &length);
				if (data != NULL)
					decoder_buffer_consume(buffer, length);
			}

			continue;
		}

		/* found a full frame! */
		return frame_length;
	}
}

static float
adts_song_duration(struct decoder_buffer *buffer)
{
	unsigned int frames, frame_length;
	unsigned sample_rate = 0;
	float frames_per_second;

	/* Read all frames to ensure correct time and bitrate */
	for (frames = 0;; frames++) {
		frame_length = adts_find_frame(buffer);
		if (frame_length == 0)
			break;


		if (frames == 0) {
			const unsigned char *data;
			size_t buffer_length;

			data = decoder_buffer_read(buffer, &buffer_length);
			assert(data != NULL);
			assert(frame_length <= buffer_length);

			sample_rate = adts_sample_rates[(data[2] & 0x3c) >> 2];
		}

		decoder_buffer_consume(buffer, frame_length);
	}

	frames_per_second = (float)sample_rate / 1024.0;
	if (frames_per_second <= 0)
		return -1;

	return (float)frames / frames_per_second;
}

static float
faad_song_duration(struct decoder_buffer *buffer, struct input_stream *is)
{
	size_t fileread;
	size_t tagsize;
	const unsigned char *data;
	size_t length;

	fileread = is->size >= 0 ? is->size : 0;

	decoder_buffer_fill(buffer);
	data = decoder_buffer_read(buffer, &length);
	if (data == NULL)
		return -1;

	tagsize = 0;
	if (length >= 10 && !memcmp(data, "ID3", 3)) {
		tagsize = (data[6] << 21) | (data[7] << 14) |
		    (data[8] << 7) | (data[9] << 0);

		tagsize += 10;

		decoder_buffer_consume(buffer, tagsize);
		decoder_buffer_fill(buffer);
		data = decoder_buffer_read(buffer, &length);
		if (data == NULL)
			return -1;
	}

	if (is->seekable && length >= 2 &&
	    data[0] == 0xFF && ((data[1] & 0xF6) == 0xF0)) {
		float song_length = adts_song_duration(buffer);

		input_stream_seek(is, tagsize, SEEK_SET);

		data = decoder_buffer_read(buffer, &length);
		if (data != NULL)
			decoder_buffer_consume(buffer, length);
		decoder_buffer_fill(buffer);

		return song_length;
	} else if (length >= 5 && memcmp(data, "ADIF", 4) == 0) {
		unsigned bit_rate;
		size_t skip_size = (data[4] & 0x80) ? 9 : 0;

		if (8 + skip_size > length)
			/* not enough data yet; skip parsing this
			   header */
			return -1;

		bit_rate = ((data[4 + skip_size] & 0x0F) << 19) |
			(data[5 + skip_size] << 11) |
			(data[6 + skip_size] << 3) |
			(data[7 + skip_size] & 0xE0);

		if (fileread != 0 && bit_rate != 0)
			return fileread * 8.0 / bit_rate;
		else
			return fileread;
	} else
		return -1;
}

/**
 * Wrapper for faacDecInit() which works around some API
 * inconsistencies in libfaad.
 */
static bool
faad_decoder_init(faacDecHandle decoder, struct decoder_buffer *buffer,
		  struct audio_format *audio_format)
{
	union {
		/* deconst hack for libfaad */
		const void *in;
		void *out;
	} u;
	size_t length;
	int32_t nbytes;
	uint32_t sample_rate;
	uint8_t channels;
#ifdef HAVE_FAAD_LONG
	/* neaacdec.h declares all arguments as "unsigned long", but
	   internally expects uint32_t pointers.  To avoid gcc
	   warnings, use this workaround. */
	unsigned long *sample_rate_r = (unsigned long *)(void *)&sample_rate;
#else
	uint32_t *sample_rate_r = &sample_rate;
#endif

	u.in = decoder_buffer_read(buffer, &length);
	if (u.in == NULL)
		return false;

	nbytes = faacDecInit(decoder, u.out,
#ifdef HAVE_FAAD_BUFLEN_FUNCS
			     length,
#endif
			     sample_rate_r, &channels);
	if (nbytes < 0)
		return false;

	decoder_buffer_consume(buffer, nbytes);

	*audio_format = (struct audio_format){
		.bits = 16,
		.channels = channels,
		.sample_rate = sample_rate,
	};

	return true;
}

/**
 * Wrapper for faacDecDecode() which works around some API
 * inconsistencies in libfaad.
 */
static const void *
faad_decoder_decode(faacDecHandle decoder, struct decoder_buffer *buffer,
		    NeAACDecFrameInfo *frame_info)
{
	union {
		/* deconst hack for libfaad */
		const void *in;
		void *out;
	} u;
	size_t length;
	void *result;

	u.in = decoder_buffer_read(buffer, &length);
	if (u.in == NULL)
		return false;

	result = faacDecDecode(decoder, frame_info,
			       u.out
#ifdef HAVE_FAAD_BUFLEN_FUNCS
			       , length
#endif
			       );

	return result;
}

static float
faad_get_file_time_float(const char *file)
{
	struct decoder_buffer *buffer;
	float length;
	faacDecHandle decoder;
	faacDecConfigurationPtr config;
	struct input_stream is;

	if (!input_stream_open(&is, file))
		return -1;

	buffer = decoder_buffer_new(NULL, &is,
				    FAAD_MIN_STREAMSIZE * AAC_MAX_CHANNELS);
	length = faad_song_duration(buffer, &is);

	if (length < 0) {
		bool ret;
		struct audio_format audio_format;

		decoder = faacDecOpen();

		config = faacDecGetCurrentConfiguration(decoder);
		config->outputFormat = FAAD_FMT_16BIT;
		faacDecSetConfiguration(decoder, config);

		decoder_buffer_fill(buffer);

		ret = faad_decoder_init(decoder, buffer, &audio_format);
		if (ret && audio_format_valid(&audio_format))
			length = 0;

		faacDecClose(decoder);
	}

	decoder_buffer_free(buffer);
	input_stream_close(&is);

	return length;
}

static int
faad_get_file_time(const char *file)
{
	int file_time = -1;
	float length;

	if ((length = faad_get_file_time_float(file)) >= 0)
		file_time = length + 0.5;

	return file_time;
}

static void
faad_stream_decode(struct decoder *mpd_decoder, struct input_stream *is)
{
	float file_time;
	float total_time = 0;
	faacDecHandle decoder;
	struct audio_format audio_format;
	faacDecFrameInfo frame_info;
	faacDecConfigurationPtr config;
	unsigned long sample_count;
	bool ret;
	const void *decoded;
	size_t decoded_length;
	uint16_t bit_rate = 0;
	struct decoder_buffer *buffer;
	enum decoder_command cmd;

	buffer = decoder_buffer_new(mpd_decoder, is,
				    FAAD_MIN_STREAMSIZE * AAC_MAX_CHANNELS);
	total_time = faad_song_duration(buffer, is);

	decoder = faacDecOpen();

	config = faacDecGetCurrentConfiguration(decoder);
	config->outputFormat = FAAD_FMT_16BIT;
#ifdef HAVE_FAACDECCONFIGURATION_DOWNMATRIX
	config->downMatrix = 1;
#endif
#ifdef HAVE_FAACDECCONFIGURATION_DONTUPSAMPLEIMPLICITSBR
	config->dontUpSampleImplicitSBR = 0;
#endif
	faacDecSetConfiguration(decoder, config);

	while (!decoder_buffer_is_full(buffer) &&
	       !input_stream_eof(is) &&
	       decoder_get_command(mpd_decoder) == DECODE_COMMAND_NONE) {
		adts_find_frame(buffer);
		decoder_buffer_fill(buffer);
	}

	ret = faad_decoder_init(decoder, buffer, &audio_format);
	if (!ret) {
		g_warning("Error not a AAC stream.\n");
		faacDecClose(decoder);
		return;
	}

	if (!audio_format_valid(&audio_format)) {
		g_warning("invalid audio format\n");
		faacDecClose(decoder);
		return;
	}

	decoder_initialized(mpd_decoder, &audio_format, false, total_time);

	file_time = 0.0;

	do {
		size_t frame_size = adts_find_frame(buffer);
		if (frame_size == 0)
			break;

		decoded = faad_decoder_decode(decoder, buffer, &frame_info);

		if (frame_info.error > 0) {
			g_warning("error decoding AAC stream: %s\n",
				  faacDecGetErrorMessage(frame_info.error));
			break;
		}

		if (frame_info.channels != audio_format.channels) {
			g_warning("channel count changed from %u to %u",
				  audio_format.channels, frame_info.channels);
			break;
		}

#ifdef HAVE_FAACDECFRAMEINFO_SAMPLERATE
		if (frame_info.samplerate != audio_format.sample_rate) {
			g_warning("sample rate changed from %u to %lu",
				  audio_format.sample_rate,
				  (unsigned long)frame_info.samplerate);
			break;
		}
#endif

		decoder_buffer_consume(buffer, frame_info.bytesconsumed);

		sample_count = (unsigned long)frame_info.samples;
		if (sample_count > 0) {
			bit_rate = frame_info.bytesconsumed * 8.0 *
			    frame_info.channels * audio_format.sample_rate /
			    frame_info.samples / 1000 + 0.5;
			file_time +=
			    (float)(frame_info.samples) / frame_info.channels /
			    audio_format.sample_rate;
		}

		decoded_length = sample_count * 2;

		cmd = decoder_data(mpd_decoder, is, decoded,
				   decoded_length, file_time,
				   bit_rate, NULL);
	} while (cmd != DECODE_COMMAND_STOP);

	faacDecClose(decoder);
}

static struct tag *
faad_tag_dup(const char *file)
{
	int file_time = faad_get_file_time(file);
	struct tag *tag;

	if (file_time < 0) {
		g_debug("Failed to get total song time from: %s", file);
		return NULL;
	}

	tag = tag_new();
	tag->time = file_time;
	return tag;
}

static const char *const faad_suffixes[] = { "aac", NULL };
static const char *const faad_mime_types[] = {
	"audio/aac", "audio/aacp", NULL
};

const struct decoder_plugin faad_decoder_plugin = {
	.name = "faad",
	.stream_decode = faad_stream_decode,
	.tag_dup = faad_tag_dup,
	.suffixes = faad_suffixes,
	.mime_types = faad_mime_types,
};
