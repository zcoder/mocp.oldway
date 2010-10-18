/*
 * MOC - music on console
 * Copyright (C) 2005, 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Based on FFplay Copyright (c) 2003 Fabrice Bellard
 *
 * Yuri Dyachenko edition
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif
#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#endif
#if HAVE_LIBAVFORMAT_AVFORMAT_H
#include <libavformat/avformat.h>
#else
#include <ffmpeg/avformat.h>
#endif

/* FFmpeg also likes common names, without that, our common.h and log.h would
 * not be included. */
#undef COMMON_H
#undef LOG_H

/* same for logging... */
#undef LOG_H

#define DEBUG

#include "common.h"
#include "decoder.h"
#include "log.h"
#include "files.h"

struct format;

struct format
{
char* m_ext;
char  m_name[4];
};

struct format formats[] = {
//	{ "wma", "WMA" }, //bad
	{ "mp3", "MP3" },
	{ "ogg", "OGG" },
	{ "aac", "AAC" },
	{ "ac3", "AC3" },
	{ "m4a", "M4A" },
	{ "wav", "WAV" },
	{ "wv" , "WV"  },
	{ "ape", "APE" },
	{ "flac", "FLA" },
};

const size_t FORMATS_COUNT = sizeof(formats)/sizeof(struct format);

struct ffmpeg_data
{
AVFormatContext* pfc;
AVCodecContext* pcc;
AVCodec* pcodec;

char* remain_buf;
int remain_buf_len;

int open; /* was this stream successfully opened? */
struct decoder_error error;
int bitrate;
int avg_bitrate;
int audio_index;
};

static void ffmpeg_init()
{
avcodec_register_all();
av_register_all ();
}

/* Fill info structure with data from ffmpeg comments */
static void ffmpeg_info(const char *file, struct file_tags *info, const int tags_sel)
{
AVFormatContext *pfc;

int err = av_open_input_file( &pfc, file, NULL, 0, NULL );
if ( err < 0 )
	{
	logit ( "av_open_input_file() failed (%d)", err );
	return;
	}

err = av_find_stream_info( pfc );
if ( err < 0 )
	{
	logit ( "av_find_stream_info() failed (%d)", err );
	av_close_input_file( pfc );
	return;
	}

if ( tags_sel & TAGS_COMMENTS )
	{
	//TODO: pfc->metadata + av_metadata_get() + AV_METADATA_MATCH_CASE - AV_METADATA_IGNORE_SUFFIX;
	if ( pfc->track != 0 )
		info->track = pfc->track;
	if ( pfc->title[0] != 0 )
		info->title = xstrdup( pfc->title );
	if ( pfc->author[0] != 0 )
		info->artist = xstrdup( pfc->author );
	if ( pfc->album[0] != 0 )
		info->album = xstrdup( pfc->album );
	}

	if ( tags_sel & TAGS_TIME )
		info->time = (pfc->duration >= 0)? (pfc->duration/AV_TIME_BASE) : -1;

av_close_input_file( pfc );
}

static void *ffmpeg_open (const char *file)
{
struct ffmpeg_data* data = (struct ffmpeg_data*) xmalloc(sizeof(struct ffmpeg_data));

data->open = 0;
decoder_error_init( &data->error );

int err = av_open_input_file(&data->pfc, file, NULL, 0, NULL);
if ( err < 0 )
	{
	decoder_error( &data->error, ERROR_FATAL, 0, "Can't open file" );
	return data;
	}

err = av_find_stream_info( data->pfc );
if ( err < 0 )
	{
	decoder_error ( &data->error, ERROR_FATAL, 0, "Could not find codec parameters (err %d)", err );
	av_close_input_file ( data->pfc );
	return data;
	}

av_read_play( data->pfc );
unsigned int i;
for (i = 0; i < data->pfc->nb_streams; i++)
	{
	data->pcc = data->pfc->streams[i]->codec;
	if (data->pcc->codec_type == CODEC_TYPE_AUDIO)
		{
		data->audio_index = i;
		break;
		}
	}
if ( data->audio_index == -1 )
	{
	decoder_error( &data->error, ERROR_FATAL, 0, "No audio stream in file" );
	av_close_input_file( data->pfc );
	return data;
	}

data->pcodec = avcodec_find_decoder( data->pcc->codec_id );
if ( !data->pcodec )
	{
	decoder_error( &data->error, ERROR_FATAL, 0, "No codec for this file." );
	av_close_input_file( data->pfc );
	return data;
	}

if ( data->pcodec->capabilities & CODEC_CAP_TRUNCATED )
	{
	data->pcc->flags |= CODEC_FLAG_TRUNCATED;
	}

err = avcodec_open( data->pcc, data->pcodec );
if ( err < 0 )
	{
	decoder_error( &data->error, ERROR_FATAL, 0, "No codec for this file." );
	av_close_input_file( data->pfc );
	return data;
	}

data->remain_buf = NULL;
data->remain_buf_len = 0;

data->open = 1;
data->avg_bitrate = (int) (data->pfc->file_size / (data->pfc->duration / 1000) * 8);
data->bitrate = data->pfc->bit_rate / 1000;

return data;
}

static void ffmpeg_close(void *prv_data)
{
struct ffmpeg_data *data = (struct ffmpeg_data*)prv_data;

if ( data->open )
	{
	avcodec_close( data->pcc );
	av_close_input_file (data->pfc);

	if ( data->remain_buf )
		free( data->remain_buf );
	}

decoder_error_clear( &data->error );
free( data );
}

static int ffmpeg_seek(void *prv_data, int sec)
{
struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;

int audio_index = data->audio_index;
AVFormatContext* pfc = data->pfc;
AVStream* ps = pfc->streams[audio_index];

int64_t ts;
int flags = AVSEEK_FLAG_ANY;
if ( data->pcc->codec_id == CODEC_ID_FLAC ) //TODO: fix: flac crutch
	{
	ts = (double)pfc->file_size * (double)sec * (double)ps->time_base.den / (double)ps->time_base.num  / (double)ps->duration;
	flags |= AVSEEK_FLAG_BYTE;
	}
else
	{
	ts = av_rescale( sec, ps->time_base.den, ps->time_base.num );
	}

int err = av_seek_frame( pfc, audio_index, ts, flags );
if ( err < 0)
	logit ("Seek error %d", err);
else if ( data->remain_buf )
	{
	free( data->remain_buf );
	data->remain_buf = NULL;
	data->remain_buf_len = 0;
	}

return (err >= 0)? sec : -1;
}

static void put_in_remain_buf(struct ffmpeg_data *data, const char *buf, const int len)
{
debug ( "Remain: %dB", len );

data->remain_buf_len = len;
data->remain_buf = (char*) xmalloc( len );
memcpy( data->remain_buf, buf, len );
}

static void add_to_remain_buf(struct ffmpeg_data *data, const char *buf, const int len)
{
debug( "Adding %dB to remain_buf", len );

data->remain_buf = (char*) xrealloc( data->remain_buf, data->remain_buf_len + len );
memcpy( data->remain_buf + data->remain_buf_len, buf, len );
data->remain_buf_len += len;

debug( "remain_buf is %dB long", data->remain_buf_len );
}

static int ffmpeg_decode (void *prv_data, char *buf, int buf_len, struct sound_params *sound_params)
{
struct ffmpeg_data* data = (struct ffmpeg_data*)prv_data;
decoder_error_clear (&data->error);

int ret;
AVPacket pkt, pkt_tmp;
int data_size;
int filled = 0;

/* The sample buffer should be 16 byte aligned (because SSE), a segmentation
 * fault may occur otherwise.
 * See: avcodec.h in ffmpeg
 */
char avbuf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2] __attribute__((aligned(16)));

sound_params->channels = data->pcc->channels;
sound_params->rate = data->pcc->sample_rate;
sound_params->fmt = SFMT_S16 | SFMT_NE;//data->pcc->sample_fmt;

if (data->remain_buf)
	{
	int to_copy = MIN( buf_len, data->remain_buf_len );
	debug ("Copying %d bytes from the remain buf", to_copy);
	memcpy (buf, data->remain_buf, to_copy);

	if (to_copy < data->remain_buf_len)
		{
		memmove( data->remain_buf, data->remain_buf + to_copy, data->remain_buf_len - to_copy);
		data->remain_buf_len -= to_copy;
		}
	else
		{
		debug ("Remain buf is now empty");
		free (data->remain_buf);
		data->remain_buf = NULL;
		data->remain_buf_len = 0;
		}
	return to_copy;
	}

size_t decode_size = 0;
do
	{
	ret = av_read_frame( data->pfc, &pkt );
	if ( ret < 0 )
		return 0;

	memcpy( &pkt_tmp, &pkt, sizeof(pkt) );
	debug ( "Got %dB packet", pkt.size );

	while ( pkt.size )
		{
		int len;
		data_size = sizeof(avbuf);
#if LIBAVCODEC_VERSION_INT >= ((52<<16)+(26<<8)+0)
		len = avcodec_decode_audio3( data->pcc, (int16_t*)avbuf, &data_size, &pkt );
#elif LIBAVCODEC_VERSION_INT >= ((51<<16)+(50<<8)+0)
		len = avcodec_decode_audio2( data->pcc, (int16_t*)avbuf, &data_size, pkt.data, pkt.size );
#else
		len = avcodec_decode_audio( data->pcc, (int16_t*)avbuf, &data_size, pkt.data, pkt.size );
#endif
		debug ("Decoded %dB", len);

		if (len < 0)
			{
			/* skip frame */
			decoder_error( &data->error, ERROR_STREAM, 0, "Error in the stream!" );
			break;
			}

		decode_size += len;
		pkt.data += len;
		pkt.size -= len;

		if ( buf_len )
			{
			int to_copy = MIN( data_size, buf_len );
			memcpy( buf, avbuf, to_copy );

			buf += to_copy;
			filled += to_copy;
			buf_len -= to_copy;

			debug ( "Copying %dB (%dB filled)", to_copy, filled );

			if ( to_copy < data_size )
				put_in_remain_buf( data, avbuf + to_copy, data_size - to_copy );
			}
		else if ( data_size )
			add_to_remain_buf( data, avbuf, data_size );
		}
	av_free_packet( &pkt_tmp );
	}
while( !filled );

//8.0 bit per byte, 16.0 bit per sample
double time = (filled+data->remain_buf_len) * 8.0 / 16.0 / sound_params->rate / sound_params->channels;
data->bitrate = round( decode_size * 8.0 / time / 1000 );

return filled;
}

static int ffmpeg_get_bitrate(void *prv_data)
{
struct ffmpeg_data *data = (struct ffmpeg_data*)prv_data;
return data->bitrate;
}

static int ffmpeg_get_avg_bitrate(void *prv_data)
{
struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;
return data->avg_bitrate;
}

static int ffmpeg_get_duration(void *prv_data)
{
struct ffmpeg_data *data = (struct ffmpeg_data *)prv_data;
return (data->pfc->duration >= 0)? (data->pfc->duration/AV_TIME_BASE) : -1;
}

static void ffmpeg_get_name(const char *file, char buf[4])
{
const char UNK[] = "UNK";
char* ext = ext_pos(file);
unsigned int i;
for (i = 0; i < FORMATS_COUNT; ++i)
	if ( !strcasecmp( ext, formats[i].m_ext ) )
		{
		strncpy( buf, formats[i].m_name, 3 );
		return;
		}

AVFormatContext* pfc;
int ret = av_open_input_file( &pfc, file, NULL, 0, NULL );
if ( ret )
	{
	strcpy( buf, UNK );
	return;
	}
	ret = av_find_stream_info( pfc );
if ( ret < 0 )
	{
	strcpy( buf, UNK );
	return;
	}

strncpy( buf, pfc->iformat->name, 3 );

//to upper case
buf[0] = toupper(buf[0]);
buf[1] = toupper(buf[1]);
buf[2] = toupper(buf[2]);

av_close_input_file( pfc );
}

static int ffmpeg_our_format_ext(const char *ext)
{
unsigned int i;
for (i = 0; i < FORMATS_COUNT; ++i)
	if ( !strcasecmp( ext, formats[i].m_ext ) )
		return 1;
return 0;
}

static int ffmpeg_our_format_file(const char *file)
{
int video = 0;
int audio = 0;

AVFormatContext* pfc;
int ret = av_open_input_file( &pfc, file, NULL, 0, NULL );
if ( ret )
	return 0;
ret = av_find_stream_info( pfc );
if ( ret < 0 )
	return 0;

unsigned int i;
for (i = 0; i < pfc->nb_streams; ++i)
	{
	if ( pfc->streams[i]->codec->codec_type==CODEC_TYPE_VIDEO )
		{
		video = 1;
		break;
		}
	else if ( pfc->streams[i]->codec->codec_type==CODEC_TYPE_AUDIO )
		audio = 1;
        }

av_close_input_file( pfc );

return audio && !video;
}

static void ffmpeg_get_error(void *prv_data, struct decoder_error *error)
{
struct ffmpeg_data *data = (struct ffmpeg_data*)prv_data;
decoder_error_copy(error, &data->error);
}

static struct decoder ffmpeg_decoder = {
	DECODER_API_VERSION,
	ffmpeg_init,
	NULL,
	ffmpeg_open,
	NULL,
	NULL,
	ffmpeg_close,
	ffmpeg_decode,
	ffmpeg_seek,
	ffmpeg_info,
	ffmpeg_get_bitrate,
	ffmpeg_get_duration,
	ffmpeg_get_error,
	ffmpeg_our_format_ext,
	ffmpeg_our_format_file,
	NULL,
	ffmpeg_get_name,
	NULL,
	NULL,
	ffmpeg_get_avg_bitrate
};

struct decoder *plugin_init()
{
return &ffmpeg_decoder;
}
