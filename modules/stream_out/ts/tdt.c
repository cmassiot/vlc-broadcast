/*****************************************************************************
 * tdt.c: Time and Date Table (EN 300 468)
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

#include <bitstream/mpeg/ts.h>
#include <bitstream/mpeg/psi.h>
#include <bitstream/dvb/si.h>

#define SOUT_CFG_PREFIX "sout-ts-tdt-"

#include "ts_packetizer.h"
#include "ts_input.h"
#include "ts_table.h"

#define T_STD_PEAK_RATE     1000000 /* bi/s */
#define DEFAULT_PERIOD      25000 /* ms */
#define DEFAULT_MAX_PERIOD  29000 /* ms */
#define DEFAULT_OFFSET      0

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

vlc_module_begin()
    set_shortname( _("TDT TS"))
    set_description( _("TDT TS packetizer") )
    set_capability( "ts packetizer", 0 )
    add_shortcut( "tdt" )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_MUX )

    set_callbacks( Open, Close )

    TS_TABLE_COMMON(TDT_PID, DEFAULT_PERIOD, DEFAULT_MAX_PERIOD, DEFAULT_OFFSET)
vlc_module_end()

/*****************************************************************************
 * Local prototypes and structures
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    TS_TABLE_COMMON_OPTIONS,
    NULL
};

static block_t *Send( ts_table_t *p_table, mtime_t i_last_muxing );

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    ts_table_t *p_table = (ts_table_t *)p_this;
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    mtime_t i_max_period;

    config_ChainParse( p_table, SOUT_CFG_PREFIX, ppsz_sout_options,
                   p_table->p_cfg );
    tstable_CommonOptions( p_table );

    /* RAP advance mode is not adequate here. */
    p_table->i_rap_advance = -1;
    i_max_period = p_table->i_period;

    switch ( p_ts_stream->params.i_conformance )
    {
    default:
    case CONFORMANCE_NONE:
    case CONFORMANCE_ISO:
    case CONFORMANCE_HDMV:
        break;

    case CONFORMANCE_ATSC:
        msg_Warn( p_table, "TDT is not compatible with ATSC conformance" );
        break;

    case CONFORMANCE_DVB:
        if ( i_max_period > INT64_C(30000000) )
            msg_Warn( p_table, "TDT period shouldn't exceed 30 s in DVB systems" );
        break;
    }

    p_table->i_peak_bitrate = T_STD_PEAK_RATE;
    p_table->i_priority = TSPACK_PRIORITY_SI;
    p_table->pf_send = Send;
    tstable_Force( p_table );

    p_table->i_total_bitrate = (TS_SIZE * 8 * INT64_C(1000000)
                                 + p_table->i_period - 1) / p_table->i_period;

    msg_Dbg( p_table, "setting up TDT" );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    VLC_UNUSED(p_this);
}

/*****************************************************************************
 * BuildTDT:
 *****************************************************************************/
static block_t *BuildTDT( ts_table_t *p_table )
{
    block_t *p_block, *p_ts;
    uint8_t *p_section;
    mtime_t i_packet_interval = p_table->p_ts_stream->params.i_packet_interval;
    mtime_t i_offset = p_table->i_last_muxing - mdate();
    time_t i_output_time = time(NULL) + i_offset / 1000000;
    struct tm broken_time;
    uint64_t i_utc;

    gmtime_r( &i_output_time, &broken_time );
    i_utc = ((uint64_t)dvb_mjd_set( broken_time.tm_year, broken_time.tm_mon,
                                    broken_time.tm_mday ) << 24)
              | (dvb_bcd_set8(broken_time.tm_hour) << 16)
              | (dvb_bcd_set8(broken_time.tm_min) << 8)
              | dvb_bcd_set8(broken_time.tm_sec);

    p_block = block_New( p_table, TDT_HEADER_SIZE + 1 );
    p_block->p_buffer[0] = 0; /* pointer_field */
    p_section = p_block->p_buffer + 1;

    tdt_init( p_section );
    tdt_set_utc( p_section, i_utc );

    msg_Dbg( p_table, "new TDT date %"PRIx64, i_utc );

    p_ts = tstable_BuildTS( p_table, p_block );
    p_ts->i_dts = p_table->i_last_muxing + i_packet_interval;
    p_ts->i_delay = i_packet_interval * 2;
    block_Release( p_block );
    return p_ts;
}

/*****************************************************************************
 * Send:
 *****************************************************************************/
static block_t *Send( ts_table_t *p_table, mtime_t i_last_muxing )
{
    mtime_t i_packet_interval = p_table->p_ts_stream->params.i_packet_interval;
    mtime_t i_next_muxing = tstable_NextMuxing( p_table, i_last_muxing );

    if ( i_next_muxing == -1
          || i_next_muxing > i_last_muxing
                              + p_table->p_ts_stream->params.i_max_prepare
                              + 3 * i_packet_interval )
        return NULL;
    p_table->i_last_muxing = i_next_muxing;

    return BuildTDT( p_table );
}
