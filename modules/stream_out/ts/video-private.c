/*****************************************************************************
 * video-private.c: proprietary extension to TS for MS codecs
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
 */

#define DEFAULT_DELAY   500 /* ms, non-compliant */

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

#define SOUT_CFG_PREFIX "sout-ts-priv-"

#include "ts_packetizer.h"
#include "ts_input.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

vlc_module_begin()
    set_shortname( _("Private video ES TS"))
    set_description( _("Private video ES TS packetizer") )
    set_capability( "ts packetizer", 50 )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_MUX )

    set_callbacks( Open, Close );

    TS_INPUT_COMMON(0)
vlc_module_end()

/*****************************************************************************
 * Local prototypes and structures
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    TS_INPUT_COMMON_OPTIONS,
    NULL
};

struct ts_packetizer_sys_t
{
    bool b_first;
};

static block_t *Send( ts_input_t *p_input, block_t *p_blocks );

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    ts_input_t *p_input = (ts_input_t *)p_this;
    ts_packetizer_sys_t *p_sys;

    switch ( p_input->fmt.i_codec )
    {
    case VLC_CODEC_H263I:
    case VLC_CODEC_H263:
    case VLC_CODEC_WMV3:
    case VLC_CODEC_WMV2:
    case VLC_CODEC_WMV1:
    case VLC_CODEC_DIV3:
    case VLC_CODEC_DIV2:
    case VLC_CODEC_DIV1:
    case VLC_CODEC_MJPG:
    case VLC_CODEC_FFV1:
    case VLC_CODEC_FFVHUFF:
        p_input->i_stream_type = 0xa0; /* user private */
        break;
    default:
        return VLC_EGENERIC;
    }

    if ( p_input->p_ts_params->i_conformance != CONFORMANCE_NONE )
        msg_Warn( p_input, "MSCODEC encapsulation isn't standard-compliant" );

    p_input->i_es_version = 1;

    p_sys = p_input->p_sys = malloc( sizeof(ts_packetizer_sys_t) );
    memset( p_sys, 0, sizeof(ts_packetizer_sys_t) );

    config_ChainParse( p_input, SOUT_CFG_PREFIX, ppsz_sout_options,
                   p_input->p_cfg );
    tsinput_CommonOptions( p_input );

    if ( p_input->fmt.i_extra < 256 )
    {
        uint8_t *p_dr = p_input->p_descriptors
            = malloc( 12 + p_input->fmt.i_extra );
        p_input->i_descriptors = 12 + p_input->fmt.i_extra;

        p_dr[0] = 0xa0; /* user private */
        p_dr[1] = 10 + p_input->fmt.i_extra;
        memcpy( &p_dr[2], (uint8_t *)&p_input->fmt.i_codec, 4 );
        p_dr[6] = p_input->fmt.video.i_width >> 8;
        p_dr[7] = p_input->fmt.video.i_width & 0xff;
        p_dr[8] = p_input->fmt.video.i_height >> 8;
        p_dr[9] = p_input->fmt.video.i_height & 0xff;
        p_dr[10] = p_input->fmt.i_extra >> 8;
        p_dr[11] = p_input->fmt.i_extra & 0xff;
        if ( p_input->fmt.i_extra )
            memcpy( &p_dr[12], p_input->fmt.p_extra, p_input->fmt.i_extra );
    }
    else
        msg_Warn( p_input, "private descriptor is too large %d",
                  p_input->fmt.i_extra );

    if ( !p_input->i_total_bitrate && p_input->i_bitrate
          && p_input->fmt.video.i_frame_rate_base )
    {
        unsigned int i_rate = p_input->fmt.video.i_frame_rate;
        unsigned int i_base = p_input->fmt.video.i_frame_rate_base;
        p_input->i_total_bitrate = p_input->i_bitrate;
        /* PES overhead */
        p_input->i_total_bitrate += (PES_HEADER_SIZE_PTSDTS * 8
                                      * i_rate + i_base - 1) / i_base;
        /* Alignment */
        p_input->i_total_bitrate += ((TS_SIZE - 1) * 8
                                      * i_rate + i_base - 1) / i_base;
        /* TS overhead */
        p_input->i_total_bitrate += (p_input->i_total_bitrate * TS_HEADER_SIZE
                                      + TS_SIZE - TS_HEADER_SIZE - 1)
                                     / (TS_SIZE - TS_HEADER_SIZE);

        p_input->i_ts_delay = T_STD_TS_BUFFER * INT64_C(8000000)
                               / p_input->i_total_bitrate;
    }

    p_input->pf_send = Send;
    p_sys->b_first = true;

    msg_Dbg( p_input, "setting up %4.4s/%d total %u bitrate %u CPB %d",
             (const char *)&p_input->fmt.i_codec, p_input->fmt.i_id,
             p_input->i_total_bitrate, p_input->i_bitrate,
             p_input->fmt.video.i_cpb_buffer );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    ts_input_t *p_input = (ts_input_t *)p_this;

    free( p_input->p_descriptors );
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
    int i_length = p_frame->i_buffer - PES_HEADER_SIZE;

    p_frame = block_Realloc( p_frame, i_header_size, p_frame->i_buffer );
    pes_init( p_frame->p_buffer );
    pes_set_streamid( p_frame->p_buffer, 0xa0 );
    pes_set_length( p_frame->p_buffer, i_length > 65535 ? 0 : i_length );
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

    p_frame->i_delay = DEFAULT_DELAY * 1000;
    tsinput_CheckMuxing( p_input, p_frame );
    if ( (p_frame->i_flags & BLOCK_FLAG_TYPE_I)
          && p_input->i_pcr_period )
        p_input->i_next_pcr = p_input->i_last_muxing; /* force PCR */
    p_first = p_ts = tsinput_BuildTS( p_input, p_frame );

    if ( p_frame->i_flags & BLOCK_FLAG_TYPE_I )
    {
        if ( ts_has_adaptation( p_first->p_buffer )
              && ts_get_adaptation( p_first->p_buffer ) )
             tsaf_set_randomaccess( p_first->p_buffer );
        else /* shouldn't happen - consider assert() */
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

        p_frame = SetPESHeader( p_input, p_frame );
        *pp_last = OutputFrame( p_input, p_frame );
        while ( *pp_last != NULL )
            pp_last = &(*pp_last)->p_next;
        p_frame = p_next;
    }

    return p_first;
}
