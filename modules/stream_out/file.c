/*****************************************************************************
 * file.c: Output incoming packets to a file
 *****************************************************************************
 * Copyright (C) 2001, 2002, 2010-2011 the VideoLAN team
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Laurent Aimar <fenrir@via.ecp.fr>
 *          Eric Petit <titer@videolan.org>
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

#include <sys/types.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_block.h>
#include <vlc_fs.h>
#include <vlc_strings.h>

#if defined( WIN32 ) && !defined( UNDER_CE )
#   include <io.h>
#   define lseek _lseeki64
#else
#   include <unistd.h>
#endif

#ifndef O_LARGEFILE
#   define O_LARGEFILE 0
#endif

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define SOUT_CFG_PREFIX "sout-file-"

#define DST_TEXT N_("Output destination")
#define DST_LONGTEXT N_( \
    "Allows you to specify the output destination used for the streaming output." )
#define APPEND_TEXT N_("Append to file")
#define APPEND_LONGTEXT N_( "Append to file if it exists instead " \
                            "of replacing it.")

#define SYNC_TEXT N_("Synchronous writing")
#define SYNC_LONGTEXT N_( "Open the file with synchronous writing.")

vlc_module_begin()
    set_description( _("File stream output") )
    set_shortname( N_( "file" ) )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_STREAM )
    add_string( SOUT_CFG_PREFIX "dst", 0, DST_TEXT, DST_LONGTEXT,
                                 true )
    add_bool( SOUT_CFG_PREFIX "append", 0, APPEND_TEXT, APPEND_LONGTEXT,
              true )
#ifdef O_SYNC
    add_bool( SOUT_CFG_PREFIX "sync", false, SYNC_TEXT, SYNC_LONGTEXT,
              false )
#endif

    set_capability( "sout stream", 100 )
    add_shortcut( "file", "stream", "fd" )
    set_callbacks( Open, Close )
vlc_module_end()

/*****************************************************************************
 * Exported prototypes
 *****************************************************************************/

static const char *ppsz_sout_options[] = {
    "dst", "append",
#ifdef O_SYNC
    "sync",
#endif
    NULL
};

struct sout_stream_sys_t
{
    int i_handle;
};

static sout_stream_id_t *Add ( sout_stream_t *, es_format_t * );
static int Del ( sout_stream_t *, sout_stream_id_t * );
static int Send( sout_stream_t *, sout_stream_id_t *, block_t * );

/*****************************************************************************
 * Open: open the file
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    sout_stream_t *p_stream = (sout_stream_t *)p_this;
    vlc_value_t val;
    int                 fd;

    config_ChainParse( p_stream, SOUT_CFG_PREFIX, ppsz_sout_options,
                   p_stream->p_cfg );

    bool append = var_GetBool( p_stream, SOUT_CFG_PREFIX "append" );

    var_Get( p_stream, SOUT_CFG_PREFIX "dst", &val );
    if ( val.psz_string == NULL || !*val.psz_string )
    {
        msg_Err( p_stream, "missing dst file name" );
        free( val.psz_string );
        return VLC_EGENERIC;
    }

    if (!strcmp (p_stream->psz_name, "fd"))
    {
        char *end;

        fd = strtol (val.psz_string, &end, 0);
        if (!*val.psz_string || *end)
        {
            msg_Err (p_stream, "invalid file descriptor: %s",
                     val.psz_string);
            return VLC_EGENERIC;
        }
        fd = vlc_dup (fd);
        if (fd == -1)
        {
            msg_Err (p_stream, "cannot use file descriptor: %m");
            return VLC_EGENERIC;
        }
    }
#ifndef UNDER_CE
    else
    if( !strcmp( val.psz_string, "-" ) )
    {
#ifdef WIN32
        setmode (fileno (stdout), O_BINARY);
#endif
        fd = vlc_dup (fileno (stdout));
        if (fd == -1)
        {
            msg_Err (p_stream, "cannot use standard output: %m");
            return VLC_EGENERIC;
        }
        msg_Dbg( p_stream, "using stdout" );
    }
#endif
    else
    {
        char *psz_tmp = str_format( p_stream, val.psz_string );
        path_sanitize( psz_tmp );

        fd = vlc_open( psz_tmp, O_RDWR | O_CREAT | O_LARGEFILE |
#ifdef O_SYNC
                (var_GetBool( p_stream, SOUT_CFG_PREFIX "sync" ) ? O_SYNC : 0) |
#endif
                (append ? 0 : O_TRUNC), 0666 );
        free( psz_tmp );
        if (fd == -1)
        {
            msg_Err (p_stream, "cannot create %s: %m", val.psz_string);
            return VLC_EGENERIC;
        }
    }

    msg_Dbg( p_stream, "file stream output opened (%s)", val.psz_string );
    free( val.psz_string );

    p_stream->pf_add    = Add;
    p_stream->pf_del    = Del;
    p_stream->pf_send   = Send;
    p_stream->p_sys    = (void *)(intptr_t)fd;

    if( p_stream->psz_name != NULL
         && (!strcmp( p_stream->psz_name, "stream" ) ||
             !strcmp(p_stream->psz_name, "fd")) )
        p_stream->p_sout->i_out_pace_nocontrol++;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: close the target
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    sout_stream_t *p_stream = (sout_stream_t *)p_this;

    close( (intptr_t)p_stream->p_sys );

    if( p_stream->psz_name != NULL
         && (!strcmp( p_stream->psz_name, "stream" ) ||
             !strcmp(p_stream->psz_name, "fd")) )
        p_stream->p_sout->i_out_pace_nocontrol--;

    msg_Dbg( p_stream, "file access output closed" );
}

/*****************************************************************************
 * Add: new input
 *****************************************************************************/
static sout_stream_id_t *Add( sout_stream_t *p_stream, es_format_t *p_fmt )
{
    if ( p_fmt->i_codec != VLC_CODEC_M2TS )
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
 * Send: write a packet to the file
 *****************************************************************************/
static int Send( sout_stream_t *p_stream, sout_stream_id_t *_junk,
                 block_t *p_in )
{
    VLC_UNUSED(_junk);
    while ( p_in != NULL )
    {
        block_t *p_next = p_in->p_next;

        if ( write( (intptr_t)p_stream->p_sys, p_in->p_buffer, p_in->i_buffer ) == -1 )
            msg_Warn( p_stream, "send error: %m" );

        block_Release( p_in );
        p_in = p_next;
    }

    return VLC_SUCCESS;
}
