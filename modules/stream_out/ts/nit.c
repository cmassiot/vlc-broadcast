/*****************************************************************************
 * nit.c: Network Information Table (EN 300 468)
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
 *  - ETSI EN 300 468 V1.11.1 (2010-04) (SI in DVB systems)
 *  - ETSI TR 101 211 V1.9.1 (2009-06) (DVB guidelines on SI)
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
#include <vlc_rand.h>

#include <bitstream/mpeg/ts.h>
#include <bitstream/mpeg/psi.h>
#include <bitstream/dvb/si.h>

#define SOUT_CFG_PREFIX "sout-ts-nit-"

#include "ts_packetizer.h"
#include "ts_input.h"
#include "ts_table.h"

#define T_STD_PEAK_RATE     1000000 /* bi/s */
#define DEFAULT_PERIOD      8000 /* ms */
#define DEFAULT_MAX_PERIOD  8000 /* ms */
#define DEFAULT_OFFSET      0

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

#define NETWORK_NAME_TEXT N_("Network name")
#define NETWORK_NAME_LONGTEXT N_( \
    "Set the network name" )
#define VERSION_TEXT N_("Version")
#define VERSION_LONGTEXT N_("Define the version number of the first table " \
  "(default random)." )

vlc_module_begin()
    set_shortname( _("NIT TS"))
    set_description( _("NIT TS packetizer") )
    set_capability( "ts packetizer", 0 )
    add_shortcut( "nit" )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_MUX )

    set_callbacks( Open, Close )

    TS_TABLE_COMMON(NIT_PID, DEFAULT_PERIOD, DEFAULT_MAX_PERIOD, DEFAULT_OFFSET)

    add_string( SOUT_CFG_PREFIX "network-name", "VLC - http://www.videolan.org",
                NETWORK_NAME_TEXT, NETWORK_NAME_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "version", -1, VERSION_TEXT,
                 VERSION_LONGTEXT, false )
vlc_module_end()

/*****************************************************************************
 * Local prototypes and structures
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    TS_TABLE_COMMON_OPTIONS,
    "network-name", "version", NULL
};

struct ts_packetizer_sys_t
{
    uint8_t *p_network_name;
    size_t i_network_name_size;
    uint8_t i_version;
};

static block_t *Send( ts_table_t *p_table, mtime_t i_last_muxing );
static void UpdateTable( ts_table_t *p_table );

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    ts_table_t *p_table = (ts_table_t *)p_this;
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    ts_packetizer_sys_t *p_sys;
    vlc_value_t val;
    char *psz_network_name;
    mtime_t i_max_period;
    unsigned short subi[3];
    vlc_rand_bytes( subi, sizeof(subi) );

    p_sys = p_table->p_sys = malloc( sizeof(ts_packetizer_sys_t) );
    memset( p_sys, 0, sizeof(ts_packetizer_sys_t) );

    config_ChainParse( p_table, SOUT_CFG_PREFIX, ppsz_sout_options,
                   p_table->p_cfg );
    tstable_CommonOptions( p_table );

    if ( p_table->i_rap_advance == -1 )
        i_max_period = p_table->i_max_period;
    else
        i_max_period = p_table->i_period;

    switch ( p_ts_stream->params.i_conformance )
    {
    default:
    case CONFORMANCE_NONE:
    case CONFORMANCE_ISO:
    case CONFORMANCE_HDMV:
        break;

    case CONFORMANCE_ATSC:
        msg_Warn( p_table, "NIT is not compatible with ATSC conformance" );
        break;

    case CONFORMANCE_DVB:
        if ( i_max_period > INT64_C(10000000) )
            msg_Warn( p_table, "NIT period shouldn't exceed 10 s in DVB systems" );
        break;
    }

    var_Get( p_table, SOUT_CFG_PREFIX "version", &val );
    if ( val.i_int != -1 )
        p_sys->i_version = val.i_int % 32;
    else
        p_sys->i_version = nrand48(subi) % 32;

    var_Get( p_table, SOUT_CFG_PREFIX "network-name", &val );
    psz_network_name = val.psz_string;
    p_sys->p_network_name = p_ts_stream->params.pf_charset(
                            p_ts_stream->params.p_charset, psz_network_name,
                            &p_sys->i_network_name_size );
    if ( p_sys->i_network_name_size > 255 )
    {
        msg_Warn( p_table, "network name is too large: %s", psz_network_name );
        p_sys->i_network_name_size = 255;
    }

    UpdateTable( p_table );

    p_table->b_defines_program = true;
    p_table->i_program = 0;

    p_table->i_peak_bitrate = T_STD_PEAK_RATE;
    p_table->i_priority = TSPACK_PRIORITY_SI;
    p_table->pf_send = Send;
    tstable_Force( p_table );

    msg_Dbg( p_table, "setting up NIT network ID %u name \"%s\"",
             p_ts_stream->i_nid, psz_network_name );
    free( psz_network_name );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    ts_table_t *p_table = (ts_table_t *)p_this;
    ts_packetizer_sys_t *p_sys = p_table->p_sys;

    tstable_Close( p_table );
    free( p_sys->p_network_name );
    free( p_sys );
}

/*****************************************************************************
 * UpdateTable:
 *****************************************************************************/
static void UpdateTable( ts_table_t *p_table )
{
    ts_packetizer_sys_t *p_sys = p_table->p_sys;
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    block_t **pp_section;
    uint8_t *p_section;
    uint8_t *p_ts;
    uint8_t *p_header2;

    block_ChainRelease( p_table->p_last_table );
    p_table->p_last_table = NULL;
    pp_section = &p_table->p_last_table;

    /* please that there can only be one section per tsid, and we declare
     * only one tsid */
    *pp_section = block_New( p_table, PSI_MAX_SIZE + PSI_HEADER_SIZE + 1 );
    (*pp_section)->p_buffer[0] = 0; /* pointer_field */
    p_section = (*pp_section)->p_buffer + 1;

    nit_init( p_section, true );
    /* set length later */
    psi_set_length( p_section, PSI_MAX_SIZE );
    nit_set_nid( p_section, p_ts_stream->i_nid );
    psi_set_version( p_section, p_sys->i_version );
    psi_set_current( p_section );
    psi_set_section( p_section, 0 );
    psi_set_lastsection( p_section, 0 );

    if ( p_sys->p_network_name != NULL )
    {
        uint8_t *p_descs;
        uint8_t *p_desc;
        /* Cannot overflow because the network name is necessarily smaller
         * than a section */
        nit_set_desclength( p_section, DESCS_MAX_SIZE );
        p_descs = nit_get_descs( p_section );
        p_desc = descs_get_desc( p_descs, 0 );
        desc40_init( p_desc );
        desc40_set_networkname( p_desc, p_sys->p_network_name,
                                p_sys->i_network_name_size );
        p_desc = descs_get_desc( p_descs, 1 );
        descs_set_length( p_descs, p_desc - p_descs - DESCS_HEADER_SIZE );
    }
    else
        nit_set_desclength( p_section, 0 );

    p_header2 = nit_get_header2( p_section );
    nith_init( p_header2 );
    nith_set_tslength( p_header2, NIT_TS_SIZE );

    p_ts = nit_get_ts( p_section, 0 );
    nitn_init( p_ts );
    nitn_set_tsid( p_ts, p_ts_stream->i_tsid );
    nitn_set_onid( p_ts, p_ts_stream->i_nid );
    nitn_set_desclength( p_ts, 0 );

    p_ts = nit_get_ts( p_section, 1 );
    if ( p_ts == NULL )
        /* This shouldn't happen */
        nit_set_length( p_section, 0 );
    else
        nit_set_length( p_section, p_ts - p_section - NIT_HEADER_SIZE );
    (*pp_section)->i_buffer = psi_get_length( p_section ) + PSI_HEADER_SIZE
                               + 1;
    psi_set_crc( p_section );

    tstable_UpdateTotalBitrate( p_table );

    msg_Dbg( p_table, "new NIT version %u", p_sys->i_version );
}

/*****************************************************************************
 * Send:
 *****************************************************************************/
static block_t *Send( ts_table_t *p_table, mtime_t i_last_muxing )
{
    return tstable_Send( p_table, i_last_muxing );
}
