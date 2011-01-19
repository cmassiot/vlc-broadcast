/*****************************************************************************
 * cpb.c: try to simulate CPB constraints before streaming (= VBV)
 *****************************************************************************
 * Copyright (C) 2010 VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_block.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define ID_TEXT N_("ID")
#define ID_LONGTEXT N_( \
    "Specify an identifier integer for the elementary stream" )

#define BITRATE_TEXT N_("Bitrate")
#define BITRATE_LONGTEXT N_("Specify the maximum bitrate of the incoming stream.")

#define BUFFER_TEXT N_("Buffer")
#define BUFFER_LONGTEXT N_("Specify the size of the buffer for the CPB operation.")

static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define SOUT_CFG_PREFIX "sout-cpb-"

vlc_module_begin()
    set_shortname( N_("CPB"))
    set_description( N_("cpb stream output"))
    set_capability( "sout stream", 50 )
    add_shortcut( "cpb" )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_STREAM )

    set_callbacks( Open, Close )

    add_integer( SOUT_CFG_PREFIX "id", -1, ID_TEXT, ID_LONGTEXT,
                 false )
    add_integer( SOUT_CFG_PREFIX "bitrate", 0, BITRATE_TEXT,
                 BITRATE_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "buffer", 0, BUFFER_TEXT,
                 BUFFER_LONGTEXT, false )
vlc_module_end()


/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    "id", "bitrate", "buffer", NULL
};

static sout_stream_id_t *Add ( sout_stream_t *, es_format_t * );
static int               Del ( sout_stream_t *, sout_stream_id_t * );
static int               Send( sout_stream_t *, sout_stream_id_t *, block_t * );

struct sout_stream_sys_t
{
    sout_stream_t *p_out;
    int i_id;
    unsigned int i_bitrate;
    unsigned int i_cpb_buffer;
    mtime_t i_cpb_length;
};

struct sout_stream_id_t
{
    sout_stream_id_t *id;
    bool b_cpb, b_inited;
    mtime_t i_cpb_delay, i_cpb_leakage;
};

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    sout_stream_t     *p_stream = (sout_stream_t *)p_this;
    sout_stream_sys_t *p_sys;
    vlc_value_t val;

    p_sys          = malloc( sizeof( sout_stream_sys_t ) );

    if( !p_stream->p_next )
    {
        msg_Err( p_stream, "cannot create chain" );
        free( p_sys );
        return VLC_EGENERIC;
    }

    config_ChainParse( p_stream, SOUT_CFG_PREFIX, ppsz_sout_options,
                   p_stream->p_cfg );

    var_Get( p_stream, SOUT_CFG_PREFIX "id", &val );
    p_sys->i_id = val.i_int;

    var_Get( p_stream, SOUT_CFG_PREFIX "bitrate", &val );
    p_sys->i_bitrate = val.i_int;
    if ( !p_sys->i_bitrate )
    {
        msg_Err( p_stream, "you must specify a bit rate" );
        free( p_sys );
        return VLC_EGENERIC;
    }

    var_Get( p_stream, SOUT_CFG_PREFIX "buffer", &val );
    p_sys->i_cpb_buffer = val.i_int;
    if ( !p_sys->i_cpb_buffer )
    {
        msg_Err( p_stream, "you must specify a CPB buffer" );
        free( p_sys );
        return VLC_EGENERIC;
    }

    p_sys->i_cpb_length = p_sys->i_cpb_buffer * INT64_C(1000000)
                           / p_sys->i_bitrate;

    p_stream->pf_add    = Add;
    p_stream->pf_del    = Del;
    p_stream->pf_send   = Send;

    p_stream->p_sys     = p_sys;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    sout_stream_t     *p_stream = (sout_stream_t*)p_this;
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    free( p_sys );
}

/*****************************************************************************
 * Add:
 *****************************************************************************/
static sout_stream_id_t * Add( sout_stream_t *p_stream, es_format_t *p_fmt )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_t *id = malloc( sizeof(sout_stream_id_t) );

    if ( (p_sys->i_id == -1 && p_fmt->i_cat == VIDEO_ES)
          || p_fmt->i_id == p_sys->i_id )
    {
        if ( !p_fmt->video.i_frame_rate )
        {
            msg_Err( p_stream, "missing frame rate for input codec=%4.4s id=%d",
                 (char*)&p_fmt->i_codec, p_fmt->i_id );
            goto fail;
        }

        msg_Dbg( p_stream, "CPB-ing input codec=%4.4s id=%d",
                 (char*)&p_fmt->i_codec, p_fmt->i_id );
        id->b_cpb = true;
        id->b_inited = false;
        id->i_cpb_leakage = INT64_C(1000000) * p_fmt->video.i_frame_rate_base
                         / p_fmt->video.i_frame_rate;

        p_fmt->i_bitrate = p_sys->i_bitrate;
        p_fmt->video.i_cpb_buffer = p_sys->i_cpb_buffer;
    }
    else
    {
fail:
        id->b_cpb = false;
    }

    id->id = p_sys->p_out->pf_add( p_sys->p_out, p_fmt );
    if ( id->id == NULL )
    {
        free( id );
        id = NULL;
    }

    return id;
}

/*****************************************************************************
 * Del:
 *****************************************************************************/
static int Del( sout_stream_t *p_stream, sout_stream_id_t *id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    p_sys->p_out->pf_del( p_sys->p_out, id->id );
    free( id );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Send:
 *****************************************************************************/
static int Send( sout_stream_t *p_stream, sout_stream_id_t *id,
                 block_t *p_first )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    if ( id->b_cpb )
    {
        block_t *p_buffer = p_first;
        while ( p_buffer != NULL )
        {
            block_t *p_next = p_buffer->p_next;
            mtime_t i_used = p_buffer->i_buffer * INT64_C(8000000)
                              / p_sys->i_bitrate;

            /* Wait for first I frame */
            if ( !id->b_inited && !(p_buffer->i_flags & BLOCK_FLAG_TYPE_I) )
            {
                p_buffer = p_next;
                continue;
            }
            if ( !id->b_inited )
            {
                id->i_cpb_delay = p_sys->i_cpb_length;
                id->b_inited = true;
            }

            id->i_cpb_delay -= i_used;
            id->i_cpb_delay += id->i_cpb_leakage;

            if ( id->i_cpb_delay < 0 )
            {
                msg_Warn( p_stream, "CPB underflow  %"PRIu64, -id->i_cpb_delay );
                id->i_cpb_delay = 0;
            }
            if ( id->i_cpb_delay > p_sys->i_cpb_length )
            {
#if 0
                msg_Warn( p_stream, "CPB overflow "I64Fd,
                          id->i_cpb_delay - p_sys->i_cpb_length );
#endif
                id->i_cpb_delay = p_sys->i_cpb_length;
            }
            p_buffer->i_delay = id->i_cpb_delay;

            p_buffer = p_next;
        }
    }

    return p_sys->p_out->pf_send( p_sys->p_out, id->id, p_first );
}
