/*****************************************************************************
 * dca.c: TS-encapsulation for DTS Coherent Acoustic (DVB-style)
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
#include <vlc_iso_lang.h>

#include <bitstream/mpeg/ts.h>
#include <bitstream/mpeg/pes.h>
#include <bitstream/mpeg/psi.h>

#define SOUT_CFG_PREFIX "sout-ts-dca-"

#include "ts_packetizer.h"
#include "ts_input.h"
#include "ts_audio.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

vlc_module_begin()
    set_shortname( _("DCA TS") )
    set_description( _("DTS Coherent Acoustic TS packetizer") )
    set_capability( "ts packetizer", 50 )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_MUX )

    set_callbacks( Open, Close )

    TS_AUDIO_COMMON(0)
vlc_module_end()

/*****************************************************************************
 * Local prototypes and structures
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    TS_AUDIO_COMMON_OPTIONS,
    NULL
};

static block_t *Send( ts_input_t *p_input, block_t *p_blocks );

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    ts_input_t *p_input = (ts_input_t *)p_this;
    ts_packetizer_sys_t *p_sys;
    uint8_t *p_descriptor;
    uint8_t p_id[4] = { 'D', 'T', 'S', '2' };

    if ( p_input->fmt.i_codec != VLC_CODEC_DTS )
        return VLC_EGENERIC;


    p_input->i_es_version = 1;

    p_sys = p_input->p_sys = malloc( sizeof(ts_packetizer_sys_t) );
    memset( p_sys, 0, sizeof(ts_packetizer_sys_t) );

    config_ChainParse( p_input, SOUT_CFG_PREFIX, ppsz_sout_options,
                   p_input->p_cfg );
    tsaudio_CommonOptions( p_input, PES_STREAM_ID_PRIVATE_1 );

    switch ( p_input->p_ts_params->i_conformance )
    {
    default:
        msg_Warn( p_input, "DTS encapsulation requires DVB conformance" );
        /* intended pass-through */
    case CONFORMANCE_DVB:
        p_input->i_stream_type = 0x6;
        break;

    case CONFORMANCE_HDMV:
        p_input->i_stream_type = 0x82;
        /* FIXME : should we still add the registration descriptor ?
         * Find normative reference. */
        break;
    }

    switch ( p_input->fmt.audio.i_frame_length )
    {
    case 512:
        p_id[3] = '1';
        break;
    case 2048:
        p_id[3] = '3';
        break;
    default:
        msg_Warn( p_input, "unknown frame length %d, signalling 1024",
                  p_input->fmt.audio.i_frame_length );
    case 1024:
        break;
    }

    /* DTS registration descriptor */
    p_input->p_descriptors = realloc( p_input->p_descriptors,
                                p_input->i_descriptors + DESC05_HEADER_SIZE );
    p_descriptor = p_input->p_descriptors + p_input->i_descriptors;
    p_input->i_descriptors += DESC05_HEADER_SIZE;
    desc05_init( p_descriptor );
    desc05_set_identifier( p_descriptor, p_id );

    if ( !p_input->i_total_bitrate && p_input->fmt.i_bitrate
          && p_input->fmt.audio.i_frame_length )
    {
        unsigned int i_rate = p_input->fmt.audio.i_rate;
        unsigned int i_base = p_input->fmt.audio.i_frame_length
                               * p_sys->i_nb_frames;
        p_input->i_total_bitrate = p_input->fmt.i_bitrate;
        /* PES overhead */
        p_input->i_total_bitrate += (PES_HEADER_SIZE_PTS * 8
                                      * i_rate + i_base - 1) / i_base;
        if ( p_sys->b_align )
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

    msg_Dbg( p_input, "setting up %4.4s/%d total %u bitrate %u lang %3.3s/%u frame %d %s",
             (const char *)&p_input->fmt.i_codec, p_input->fmt.i_id,
             p_input->i_total_bitrate, p_input->fmt.i_bitrate,
             p_sys->pi_language, p_sys->i_audio_type, p_sys->i_nb_frames,
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

    tsaudio_Close( p_input );
    free( p_sys );
}

/*****************************************************************************
 * Send: We consider each p_frame to be a complete frame with optional headers
 *****************************************************************************/
static block_t *Send( ts_input_t *p_input, block_t *p_frame )
{
    block_t *p_first = NULL;
    block_t **pp_last = &p_first;

    if ( tsaudio_LanguageChanged( p_input ) )
    {
        tsaudio_GetLanguage( p_input );
        tsaudio_SetLanguageDescr( p_input );
        p_input->i_es_version++;
    }

    while ( p_frame != NULL )
    {
        block_t *p_next = p_frame->p_next;
        p_frame->p_next = NULL;

        *pp_last = tsaudio_HandleFrame( p_input, p_frame );
        while ( *pp_last != NULL )
            pp_last = &(*pp_last)->p_next;

        p_frame = p_next;
    }

    return p_first;
}
