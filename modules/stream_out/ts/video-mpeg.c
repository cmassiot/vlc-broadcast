/*****************************************************************************
 * video-mpeg.c: TS-encapsulation for MPEG-1/2/4 video
 *****************************************************************************
 * Copyright (C) 2010-2011 VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*
 * Normative references:
 *  - ISO/IEC 13818-1:2007(E) (MPEG-2 systems)
 *  - ETSI TS 101 154 V1.7.1 (2005-06) (DVB video and audio coding)
 */

#define T_STD_BUFFER        p_input->fmt.video.i_cpb_buffer
#define T_STD_PEAK_RATE     p_input->i_peak_bitrate
#define T_STD_MAX_RETENTION 1000 /* ms */
#define DEFAULT_DELAY       500 /* ms, for non-compliant, non-CPB streams */

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_block.h>

#include <bitstream/mpeg/ts.h>
#include <bitstream/mpeg/pes.h>

#define SOUT_CFG_PREFIX "sout-ts-mpgv-"

#include "ts_packetizer.h"
#include "ts_input.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

#define ALIGN_TEXT N_("Always align")
#define ALIGN_LONGTEXT N_("Always align frame header to the start of a PES (suboptimal overhead), default true for MPEG-1/2, false otherwise")

vlc_module_begin()
    set_shortname( _("MPEG video TS"))
    set_description( _("MPEG video TS packetizer") )
    set_capability( "ts packetizer", 50 )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_MUX )

    set_callbacks( Open, Close )

    TS_INPUT_COMMON(0)

    add_integer( SOUT_CFG_PREFIX "align", -1, ALIGN_TEXT,
                 ALIGN_LONGTEXT, false )
vlc_module_end()

/*****************************************************************************
 * Local prototypes and structures
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    TS_INPUT_COMMON_OPTIONS,
    "align", NULL
};

struct ts_packetizer_sys_t
{
    bool b_align, b_first;

    block_t *p_last_frame;
};

static block_t *Send( ts_input_t *p_input, block_t *p_blocks );

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    ts_input_t *p_input = (ts_input_t *)p_this;
    ts_packetizer_sys_t *p_sys;
    vlc_value_t val;

    switch ( p_input->fmt.i_codec )
    {
    case VLC_CODEC_MPGV:
        p_input->i_stream_type = 0x2;
        break;
    case VLC_CODEC_MP4V:
        p_input->i_stream_type = 0x10;
        break;
    case VLC_CODEC_H264:
        p_input->i_stream_type = 0x1b;
        break;
    default:
        return VLC_EGENERIC;
    }

    p_input->i_es_version = 1;

    p_sys = p_input->p_sys = malloc( sizeof(ts_packetizer_sys_t) );
    memset( p_sys, 0, sizeof(ts_packetizer_sys_t) );

    config_ChainParse( p_input, SOUT_CFG_PREFIX, ppsz_sout_options,
                   p_input->p_cfg );
    tsinput_CommonOptions( p_input );

    var_Get( p_input, SOUT_CFG_PREFIX "align", &val );
    if ( val.i_int == -1 )
        p_sys->b_align = (p_input->fmt.i_codec == VLC_CODEC_MPGV);
    else
        p_sys->b_align = val.i_int;

    if ( p_input->fmt.video.i_max_bitrate )
        p_input->i_peak_bitrate = 6 * p_input->fmt.video.i_max_bitrate / 5;
    else
        p_input->i_peak_bitrate = 6 * p_input->i_bitrate / 5;

    if ( !p_input->i_total_bitrate && p_input->i_bitrate
          && p_input->fmt.video.i_frame_rate_base
          && p_input->fmt.video.i_cpb_buffer )
    {
        unsigned int i_rate = p_input->fmt.video.i_frame_rate;
        unsigned int i_base = p_input->fmt.video.i_frame_rate_base;
        p_input->i_total_bitrate = p_input->i_bitrate;
        /* PES overhead */
        p_input->i_total_bitrate += (PES_HEADER_SIZE_PTSDTS * 8
                                      * i_rate + i_base - 1) / i_base;
        /* At worst we'll have 187 bytes wasted per frame, if all frames
         * are I-frames or if we are aligned. */
        p_input->i_total_bitrate += ((TS_SIZE - 1) * 8
                                      * i_rate + i_base - 1) / i_base;
        /* TS overhead */
        p_input->i_total_bitrate += (p_input->i_total_bitrate * TS_HEADER_SIZE
                                      + TS_SIZE - TS_HEADER_SIZE - 1)
                                     / (TS_SIZE - TS_HEADER_SIZE);
    }


    if ( p_input->i_total_bitrate )
        p_input->i_ts_delay = T_STD_TS_BUFFER * INT64_C(8000000)
                               / p_input->i_total_bitrate;
    p_input->pf_send = Send;
    p_sys->p_last_frame = NULL;
    p_sys->b_first = true;

    msg_Dbg( p_input, "setting up %4.4s/%d total %u bitrate %u CPB %d %s",
             (const char *)&p_input->fmt.i_codec, p_input->fmt.i_id,
             p_input->i_total_bitrate, p_input->i_bitrate,
             p_input->fmt.video.i_cpb_buffer,
             p_sys->b_align ? "aligned" : "unaligned" );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    ts_input_t *p_input = (ts_input_t *)p_this;
    ts_packetizer_sys_t *p_sys = p_input->p_sys;

    if ( p_sys->p_last_frame != NULL )
        block_Release( p_sys->p_last_frame );
    free( p_sys );
}

/*****************************************************************************
 * SetPESHeader:
 *****************************************************************************/
static block_t *SetPESHeader( ts_input_t *p_input, block_t *p_frame )
{
    VLC_UNUSED(p_input);
    bool b_has_dts = p_frame->i_dts != p_frame->i_pts;
    int i_header_size = b_has_dts ? PES_HEADER_SIZE_PTSDTS :
                        PES_HEADER_SIZE_PTS;

    p_frame = block_Realloc( p_frame, i_header_size, p_frame->i_buffer );
    pes_init( p_frame->p_buffer );
    pes_set_streamid( p_frame->p_buffer, PES_STREAM_ID_VIDEO_MPEG );
    /* Length will be set later. */
    pes_set_headerlength( p_frame->p_buffer, 0 );
    pes_set_pts( p_frame->p_buffer, p_frame->i_pts * 9 / 100 );
    if ( b_has_dts )
        pes_set_dts( p_frame->p_buffer, p_frame->i_dts * 9 / 100 );

    if ( p_frame->i_flags & BLOCK_FLAG_ALIGNED )
        pes_set_dataalignment( p_frame->p_buffer );

    return p_frame;
}

/*****************************************************************************
 * OutputFrame:
 *****************************************************************************/
static block_t *OutputFrame( ts_input_t *p_input, block_t *p_frame )
{
    block_t *p_first, *p_ts;
    int i_length = p_frame->i_buffer - PES_HEADER_SIZE;
    pes_set_length( p_frame->p_buffer, i_length > 65535 ? 0 : i_length );

    if ( !T_STD_BUFFER )
        p_frame->i_delay = DEFAULT_DELAY * 1000;
    else if ( p_input->fmt.i_codec != VLC_CODEC_H264
               && p_frame->i_delay > T_STD_MAX_RETENTION * 1000 )
        p_frame->i_delay = T_STD_MAX_RETENTION * 1000;
    tsinput_CheckMuxing( p_input, p_frame );

    if ( (p_frame->i_flags & BLOCK_FLAG_TYPE_I)
          && p_input->i_pcr_period )
        p_input->i_next_pcr = p_input->i_last_muxing; /* force PCR insertion */
    p_first = p_ts = tsinput_BuildTS( p_input, p_frame );

    if ( p_frame->i_flags & BLOCK_FLAG_TYPE_I )
    {
        if ( ts_has_adaptation( p_first->p_buffer )
              && ts_get_adaptation( p_first->p_buffer ) )
             tsaf_set_randomaccess( p_first->p_buffer );
        else if ( p_input->i_pcr_period )
            /* shouldn't happen - consider assert() */
            msg_Err( p_input, "internal error #1" );

        while ( p_ts != NULL )
        {
            ts_set_transportpriority( p_ts->p_buffer );
            if ( ts_has_adaptation( p_ts->p_buffer )
                  && ts_get_adaptation( p_ts->p_buffer ) )
                tsaf_set_streampriority( p_ts->p_buffer );
            p_ts = p_ts->p_next;
        }
    }

    block_Release( p_frame );
    return p_first;
}

/*****************************************************************************
 * Send: We consider each p_frame to be a complete frame with optional headers
 *****************************************************************************/
static block_t *Send( ts_input_t *p_input, block_t *p_frame )
{
    ts_packetizer_sys_t *p_sys = p_input->p_sys;
    block_t *p_first = NULL;
    block_t **pp_last = &p_first;

    if ( p_sys->b_first )
    {
        p_frame->i_flags |= BLOCK_FLAG_DISCONTINUITY;
        p_sys->b_first = false;
    }

    while ( p_frame != NULL )
    {
        block_t *p_next = p_frame->p_next;
        p_frame->p_next = NULL;

        if ( (p_frame->i_flags & BLOCK_FLAG_TYPE_I) || p_sys->b_align )
        {
            p_frame->i_flags |= BLOCK_FLAG_ALIGNED;
        }
        else if ( p_sys->p_last_frame != NULL
                   && !(p_sys->p_last_frame->i_flags & BLOCK_FLAG_TYPE_I) )
        {
            int i_overlap_size = tsinput_CheckOverlap( p_input,
                                                       p_sys->p_last_frame );
            if ( i_overlap_size )
                p_frame = tsinput_OverlapFrames( p_frame, p_sys->p_last_frame,
                                                 i_overlap_size );
        }

        if ( p_sys->p_last_frame != NULL )
        {
            *pp_last = OutputFrame( p_input, p_sys->p_last_frame );
            while ( *pp_last != NULL )
                pp_last = &(*pp_last)->p_next;
            p_sys->p_last_frame = NULL;
        }

        p_frame = SetPESHeader( p_input, p_frame );

        p_sys->p_last_frame = p_frame;
        p_frame = p_next;
    }

    /* No need to bufferize one frame if we know we'll be aligned. */
    if ( p_sys->p_last_frame != NULL
          && (p_sys->b_align
               || (p_sys->p_last_frame->i_flags & BLOCK_FLAG_TYPE_I) ) )
    {
        *pp_last = OutputFrame( p_input, p_sys->p_last_frame );
        p_sys->p_last_frame = NULL;
    }

    return p_first;
}
