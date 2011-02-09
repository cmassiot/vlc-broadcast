/*****************************************************************************
 * udp.c: Output incoming packet to UDP datagrams
 *****************************************************************************
 * Copyright (C) 2001-2005, 2010-2011 the VideoLAN team
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Eric Petit <titer@videolan.org>
 *          Christophe Massiot <massiot@via.ecp.fr>
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

#include <vlc_common.h>
#include <vlc_plugin.h>

#include <sys/types.h>

#include <vlc_sout.h>
#include <vlc_block.h>

#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif

#ifdef WIN32
#   include <winsock2.h>
#   include <ws2tcpip.h>
#else
#   include <sys/socket.h>
#endif

#include <vlc_network.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define SOUT_CFG_PREFIX "sout-udp-"

#define DST_TEXT N_("Output destination")
#define DST_LONGTEXT N_( \
    "Allows you to specify the output destination used for the streaming output." )
#define TTL_TEXT N_("Time-To-Live (TTL)")
#define TTL_LONGTEXT N_("Allows you to define the Time-To-Live of the " \
                        "outgoing stream.")
#define TOS_TEXT N_("Type of service (TOS)")
#define TOS_LONGTEXT N_("Allows you to set the TOS parameter of the IP " \
                        "header of the outgoing stream.")

vlc_module_begin()
    set_description( _("UDP stream output") )
    set_shortname( N_( "UDP" ) )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_STREAM )
    add_string( SOUT_CFG_PREFIX "dst", 0, DST_TEXT, DST_LONGTEXT,
                                 true )
    add_integer( SOUT_CFG_PREFIX "ttl", 0, TTL_TEXT, TTL_LONGTEXT,
                                 true )
    add_integer( SOUT_CFG_PREFIX "tos", 0, TOS_TEXT, TOS_LONGTEXT,
                                 true )

    set_capability( "sout stream", 100 )
    add_shortcut( "udp" )
    set_callbacks( Open, Close )
vlc_module_end()

/*****************************************************************************
 * Exported prototypes
 *****************************************************************************/

static const char *ppsz_sout_options[] = {
    "dst", "ttl", "tos", NULL
};

struct sout_stream_sys_t
{
    int i_handle;
};

static sout_stream_id_t *Add ( sout_stream_t *, es_format_t * );
static int Del ( sout_stream_t *, sout_stream_id_t * );
static int Send( sout_stream_t *, sout_stream_id_t *, block_t * );

#define DEFAULT_PORT 1234

/*****************************************************************************
 * Open: open the file
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    sout_stream_t *p_stream = (sout_stream_t *)p_this;
    sout_stream_sys_t *p_sys;
    vlc_value_t val;
    int i_ttl;
    int i_tos;
    char *psz_parser;
    int i_port = DEFAULT_PORT;

    p_sys = malloc( sizeof(sout_stream_sys_t) );
    memset( p_sys, 0, sizeof(sout_stream_sys_t) );

    config_ChainParse( p_stream, SOUT_CFG_PREFIX, ppsz_sout_options,
                   p_stream->p_cfg );

    var_Get( p_stream, SOUT_CFG_PREFIX "ttl", &val );
    i_ttl = val.i_int;

    var_Get( p_stream, SOUT_CFG_PREFIX "tos", &val );
    i_tos = val.i_int;

    var_Get( p_stream, SOUT_CFG_PREFIX "dst", &val );
    psz_parser = val.psz_string;
    if ( *psz_parser == '[' )
    {
        char *psz_port = strrchr( psz_parser, ']' );
        if ( psz_port == NULL || (psz_port[1] && psz_port[1] != ':') )
        {
            msg_Err( p_stream, "invalid IPv6 address %s", psz_parser );
            free( p_sys );
            free( val.psz_string );
            return VLC_EGENERIC;
        }
        psz_parser = psz_port + 1;
    }
    else
    {
        psz_parser = strrchr( psz_parser, ':' );
    }

    if ( psz_parser != NULL && *psz_parser )
    {
        psz_parser[0] = '\0';
        i_port = atoi(&psz_parser[1]);
    }

    p_sys->i_handle = net_ConnectDgram( p_this, val.psz_string, i_port, i_ttl,
                                        IPPROTO_UDP );
    free( val.psz_string );
    if( p_sys->i_handle == -1 )
    {
         msg_Err( p_stream, "failed to open a connection (udp)" );
         free( p_sys );
         return VLC_EGENERIC;
    }
    msg_Dbg( p_stream, "udp stream output opened" );

    shutdown( p_sys->i_handle, SHUT_RD );
    if( i_tos )
        net_SetTOS( p_stream, p_sys->i_handle, i_tos );

    p_stream->pf_add    = Add;
    p_stream->pf_del    = Del;
    p_stream->pf_send   = Send;
    p_stream->p_sys     = p_sys;

    p_stream->p_sout->i_out_pace_nocontrol++;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: close the target
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    sout_stream_t *p_stream = (sout_stream_t *)p_this;
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    net_Close( p_sys->i_handle );

    p_stream->p_sout->i_out_pace_nocontrol--;

    msg_Dbg( p_stream, "udp stream output closed" );
    free( p_sys );
}

/*****************************************************************************
 * Add: new input
 *****************************************************************************/
static sout_stream_id_t *Add( sout_stream_t *p_stream, es_format_t *p_fmt )
{
    if ( p_fmt->i_codec != VLC_CODEC_RTP && p_fmt->i_codec != VLC_CODEC_M2TS )
        msg_Warn( p_stream, "trying to handle unknown datagram source %4.4s",
                  (char *)&p_fmt->i_codec );

    /* Just return non-NULL */
    return (sout_stream_id_t *)1;
}

/*****************************************************************************
 * Del: stub
 *****************************************************************************/
static int Del( sout_stream_t *p_stream, sout_stream_id_t *p_input )
{
    VLC_UNUSED(p_stream);
    VLC_UNUSED(p_input);
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Send: write a packet on the network
 *****************************************************************************/
static int Send( sout_stream_t *p_stream, sout_stream_id_t *_junk,
                 block_t *p_in )
{
    VLC_UNUSED(_junk);
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    while ( p_in != NULL )
    {
        block_t *p_next = p_in->p_next;

        if ( send( p_sys->i_handle, p_in->p_buffer, p_in->i_buffer, 0 ) == -1 )
            msg_Warn( p_stream, "send error: %m" );

        block_Release( p_in );
        p_in = p_next;
    }

    return VLC_SUCCESS;
}
