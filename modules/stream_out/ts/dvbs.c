/*****************************************************************************
 * dvbs.c: TS-encapsulation for DVB subtitles
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
 *  - ETSI EN 300 743 V1.2.1 (2002-06) (DVB Subtitling systems)
 *  - ETSI EN 300 468 V1.5.1 (2003-05) (SI in DVB systems)
 */

#define T_STD_BUFFER        24576 /* bytes */
#define T_STD_PEAK_RATE     192000 /* bi/s */
/* This is the theoritical max: */
//#define T_STD_MAX_RETENTION 1000 /* ms */
/* This is what we want: */
#define T_STD_MAX_RETENTION 200 /* ms */
#define DEFAULT_DELAY       200 /* ms, for non-compliant VBR streams */

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
#include <bitstream/dvb/si.h>

#define SOUT_CFG_PREFIX "sout-ts-dvbs-"

#include "ts_packetizer.h"
#include "ts_input.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

#define LANG_TEXT N_("Subtitling languages")
#define LANG_LONGTEXT N_( \
    "Allows you to set subtitling languages (page=lang/type,...)." )

vlc_module_begin()
    set_shortname( _("DVB subtitles TS"))
    set_description( _("DVB subtitles TS packetizer") )
    set_capability( "ts packetizer", 50 )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_MUX )

    set_callbacks( Open, Close )

    TS_INPUT_COMMON(0)

    add_string( SOUT_CFG_PREFIX "lang", "", LANG_TEXT,
                LANG_LONGTEXT, false )
vlc_module_end()

/*****************************************************************************
 * Local prototypes and structures
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    TS_INPUT_COMMON_OPTIONS,
    "lang", NULL
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
    vlc_value_t val;

    if ( p_input->fmt.i_codec != VLC_CODEC_DVBS )
        return VLC_EGENERIC;

    if ( p_input->p_ts_params->i_conformance != CONFORMANCE_DVB )
        msg_Warn( p_input, "DVB sub encapsulation requires DVB conformance" );

    p_input->i_es_version = 1;

    p_sys = p_input->p_sys = malloc( sizeof(ts_packetizer_sys_t) );
    memset( p_sys, 0, sizeof(ts_packetizer_sys_t) );

    config_ChainParse( p_input, SOUT_CFG_PREFIX, ppsz_sout_options,
                   p_input->p_cfg );
    tsinput_CommonOptions( p_input );

    p_input->i_stream_type = 0x6;

    var_Get( p_input, SOUT_CFG_PREFIX "lang", &val );
    if ( (val.psz_string != NULL && *val.psz_string) || !p_input->fmt.i_extra )
    {
        int i_dr_size = DESC59_HEADER_SIZE;
        uint8_t *p_dr = malloc( i_dr_size );
        char *psz_parser = val.psz_string;
        char *psz_next;

        desc59_init( p_dr );

        while ( (psz_next = strchr( psz_parser, '=' )) != NULL )
        {
            uint8_t *p_dr_n = p_dr + i_dr_size;
            uint8_t pi_lang[3];
            uint32_t i_page;

            *psz_next++ = '\0';
            if ( !psz_next[0] || !psz_next[1] || !psz_next[2] )
                break;
            pi_lang[0] = psz_next[0];
            pi_lang[1] = psz_next[1];
            pi_lang[2] = psz_next[2];
            i_page = strtol( psz_parser, NULL, 0 );

            i_dr_size += DESC59_LANGUAGE_SIZE;
            p_dr = realloc( p_dr, i_dr_size );

            desc59n_set_code( p_dr_n, pi_lang );
            desc59n_set_compositionpage( p_dr_n, i_page & 0xffff );
            desc59n_set_ancillarypage( p_dr_n, i_page >> 16 );

            if ( *psz_next == '/' )
            {
                psz_next++;
                desc59n_set_subtitlingtype( p_dr_n,
                                            strtol( psz_next, &psz_next, 0 ) );
            }
            else  /* DVB-subtitles (normal) with no AR criticality */
                desc59n_set_subtitlingtype( p_dr_n, 0x10 );

            if ( *psz_next == ',' )
                psz_next++;
            psz_parser = psz_next;
        }

        desc_set_length( p_dr, i_dr_size - DESC59_HEADER_SIZE );
        p_input->p_descriptors = p_dr;
        p_input->i_descriptors = i_dr_size;
    }
    else if ( p_input->fmt.i_extra )
    {
        p_input->i_descriptors = p_input->fmt.i_extra
                                       + DESC_HEADER_SIZE;
        p_input->p_descriptors = malloc( p_input->i_descriptors );

        desc59_init( p_input->p_descriptors );
        desc_set_length( p_input->p_descriptors, p_input->fmt.i_extra );
        memcpy( p_input->p_descriptors + DESC_HEADER_SIZE,
                p_input->fmt.p_extra, p_input->fmt.i_extra );
    }
    free( val.psz_string );

    p_input->i_peak_bitrate = T_STD_PEAK_RATE;
    if ( !p_input->i_total_bitrate )
        p_input->i_total_bitrate = T_STD_PEAK_RATE;

    /* Do not use T-STD TS buffer for telx because it would be too large
     * and would violate the retention constraint. */
    p_input->i_ts_delay = 0;

    p_input->pf_send = Send;

    msg_Dbg( p_input, "setting up %4.4s/%d total %u bitrate %u",
             (const char *)&p_input->fmt.i_codec, p_input->fmt.i_id,
             p_input->i_total_bitrate, p_input->i_bitrate );

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
    p_frame = block_Realloc( p_frame, PES_HEADER_SIZE_PTS, p_frame->i_buffer );
    pes_init( p_frame->p_buffer );
    pes_set_streamid( p_frame->p_buffer, PES_STREAM_ID_PRIVATE_1 );
    pes_set_length( p_frame->p_buffer, p_frame->i_buffer - PES_HEADER_SIZE );
    pes_set_headerlength( p_frame->p_buffer, 0 );
    pes_set_pts( p_frame->p_buffer, p_frame->i_pts * 9 / 100 );
    pes_set_dataalignment( p_frame->p_buffer );

    return p_frame;
}

/*****************************************************************************
 * OutputFrame:
 *****************************************************************************/
static block_t *OutputFrame( ts_input_t *p_input, block_t *p_frame )
{
    block_t *p_first;

    p_first = tsinput_BuildTS( p_input, p_frame );

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

        if ( p_input->i_bitrate )
            p_frame->i_delay = (T_STD_BUFFER - p_frame->i_buffer)
                                 * INT64_C(8000000) / p_input->i_bitrate;
        else
            p_frame->i_delay = DEFAULT_DELAY * 1000;
        if ( p_frame->i_delay > T_STD_MAX_RETENTION * 1000 )
            p_frame->i_delay = T_STD_MAX_RETENTION * 1000;
        tsinput_CheckMuxing( p_input, p_frame );

        p_frame = SetPESHeader( p_input, p_frame );
        *pp_last = OutputFrame( p_input, p_frame );
        while ( *pp_last != NULL )
            pp_last = &(*pp_last)->p_next;
        p_frame = p_next;
    }

    return p_first;
}
