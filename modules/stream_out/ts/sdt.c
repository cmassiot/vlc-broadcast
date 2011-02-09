/*****************************************************************************
 * sdt.c: Service Description Table (EN 300 468)
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

#define SOUT_CFG_PREFIX "sout-ts-sdt-"

#include "ts_packetizer.h"
#include "ts_input.h"
#include "ts_table.h"

#define T_STD_PEAK_RATE     1000000 /* bi/s */
#define DEFAULT_PERIOD      1500 /* ms */
#define DEFAULT_MAX_PERIOD  1800 /* ms */
#define DEFAULT_OFFSET      0

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

#define SERVICES_TEXT N_("Services")
#define SERVICES_LONGTEXT N_( \
    "Allows you to set the list of [sid=]name/provider/type[:...]" )
#define VERSION_TEXT N_("Version")
#define VERSION_LONGTEXT N_("Defines the version number of the first table " \
  "(default random)." )

vlc_module_begin()
    set_shortname( _("SDT TS"))
    set_description( _("SDT TS packetizer") )
    set_capability( "ts packetizer", 0 )
    add_shortcut( "sdt" )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_MUX )

    set_callbacks( Open, Close )

    TS_TABLE_COMMON(SDT_PID, DEFAULT_PERIOD, DEFAULT_MAX_PERIOD, DEFAULT_OFFSET)

    add_string( SOUT_CFG_PREFIX "services", "VLC service/videolan.org/1", SERVICES_TEXT,
                SERVICES_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "version", -1, VERSION_TEXT,
                 VERSION_LONGTEXT, false )
vlc_module_end()

/*****************************************************************************
 * Local prototypes and structures
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    TS_TABLE_COMMON_OPTIONS,
    "services", "version", NULL
};

typedef struct sdt_service_t
{
    uint16_t i_sid;
    uint8_t *p_service_name, *p_provider_name;
    size_t i_service_name_size, i_provider_name_size;
    uint8_t i_service_type;
} sdt_service_t;

struct ts_packetizer_sys_t
{
    bool b_auto;
    uint8_t *p_service_name, *p_provider_name;
    size_t i_service_name_size, i_provider_name_size;
    uint8_t i_service_type;

    sdt_service_t *p_services;
    int i_nb_services;

    uint8_t i_version;
};

static block_t *Send( ts_table_t *p_table, mtime_t i_last_muxing );
static bool BuildServices( ts_table_t *p_table );
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
    char *psz_parser;
    mtime_t i_max_period;
    unsigned short subi[3];
    vlc_rand_bytes( subi, sizeof(subi) );

    p_sys = p_table->p_sys = malloc( sizeof(ts_packetizer_sys_t) );
    memset( p_sys, 0, sizeof(ts_packetizer_sys_t) );
    p_sys->i_nb_services = -1;
    p_sys->p_services = NULL;

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
        msg_Warn( p_table, "SDT is not compatible with ATSC conformance" );
        break;

    case CONFORMANCE_DVB:
        if ( i_max_period > INT64_C(2000000) )
            msg_Warn( p_table, "SDT period shouldn't exceed 2 s in DVB systems" );
        break;
    }

    var_Get( p_table, SOUT_CFG_PREFIX "version", &val );
    if ( val.i_int != -1 )
        p_sys->i_version = val.i_int % 32;
    else
        p_sys->i_version = nrand48(subi) % 32;

    var_Get( p_table, SOUT_CFG_PREFIX "services", &val );
    psz_parser = val.psz_string;
    p_sys->p_services = NULL;
    p_sys->i_nb_services = 0;
    p_sys->p_service_name = p_sys->p_provider_name = NULL;
    p_sys->i_service_name_size = p_sys->i_provider_name_size = 0;
    p_sys->i_service_type = 0x1;

    if ( psz_parser == NULL || !*psz_parser )
    {
        p_sys->b_auto = true;
        BuildServices( p_table );
    }
    else
    {
        char *psz_next;
        strtoul( psz_parser, &psz_next, 0 );

        if ( psz_next == psz_parser )
        {
            p_sys->b_auto = true;

            psz_next = strchr( psz_parser, '/' );
            if ( psz_next != NULL )
                *psz_next++ = '\0';

            p_sys->p_service_name = p_ts_stream->params.pf_charset(
                            p_ts_stream->params.p_charset, psz_parser,
                            &p_sys->i_service_name_size );
            if ( p_sys->i_service_name_size > 255 )
            {
                msg_Warn( p_table, "service name is too large: %s",
                          psz_parser );
                p_sys->i_service_name_size = 255;
            }
            psz_parser = psz_next;

            if ( psz_parser != NULL )
            {
                psz_next = strchr( psz_parser, '/' );
                if ( psz_next != NULL )
                    *psz_next++ = '\0';

                p_sys->p_provider_name = p_ts_stream->params.pf_charset(
                                p_ts_stream->params.p_charset, psz_parser,
                                &p_sys->i_provider_name_size );
                if ( p_sys->i_provider_name_size > 255 )
                {
                    msg_Warn( p_table, "provider name is too large: %s",
                              psz_parser );
                    p_sys->i_provider_name_size = 255;
                }
                psz_parser = psz_next;

                if ( psz_parser != NULL )
                    p_sys->i_service_type = strtoul( psz_parser, NULL, 0 );
            }

            BuildServices( p_table );
        }
        else
        {
            p_sys->b_auto = false;

            while ( psz_parser != NULL && *psz_parser )
            {
                uint16_t i_sid = strtoul( psz_parser, &psz_next, 0 );
                uint8_t *p_service_name = NULL, *p_provider_name = NULL;
                size_t i_service_name_size = 0, i_provider_name_size = 0;
                uint8_t i_service_type = 0x1;
                char *psz_next_service = strchr( psz_parser, ':' );
                if ( psz_next_service != NULL )
                    *psz_next_service++ = '\0';

                if ( !i_sid || *psz_next != '=' )
                {
                    msg_Warn( p_table, "invalid service %s", psz_parser );
                    psz_parser = psz_next_service;
                    continue;
                }
                psz_parser = psz_next + 1;

                psz_next = strchr( psz_parser, '/' );
                if ( psz_next != NULL )
                    *psz_next++ = '\0';

                p_service_name = p_ts_stream->params.pf_charset(
                                p_ts_stream->params.p_charset, psz_parser,
                                &i_service_name_size );
                if ( p_sys->i_service_name_size > 255 )
                {
                    msg_Warn( p_table, "service name is too large: %s",
                              psz_parser );
                    p_sys->i_service_name_size = 255;
                }
                psz_parser = psz_next;

                if ( psz_parser != NULL )
                {
                    psz_next = strchr( psz_parser, '/' );
                    if ( psz_next != NULL )
                        *psz_next++ = '\0';

                    p_provider_name = p_ts_stream->params.pf_charset(
                                    p_ts_stream->params.p_charset, psz_parser,
                                    &i_provider_name_size );
                    if ( p_sys->i_provider_name_size > 255 )
                    {
                        msg_Warn( p_table, "provider name is too large: %s",
                                  psz_parser );
                        p_sys->i_provider_name_size = 255;
                    }
                    psz_parser = psz_next;

                    if ( psz_parser != NULL )
                        i_service_type = strtoul( psz_parser, NULL, 0 );
                }

                p_sys->i_nb_services++;
                p_sys->p_services = realloc( p_sys->p_services,
                                p_sys->i_nb_services * sizeof(sdt_service_t) );
                p_sys->p_services[p_sys->i_nb_services - 1].i_sid = i_sid;
                p_sys->p_services[p_sys->i_nb_services - 1].p_service_name
                    = p_service_name;
                p_sys->p_services[p_sys->i_nb_services - 1].p_provider_name
                    = p_provider_name;
                p_sys->p_services[p_sys->i_nb_services - 1].i_service_name_size
                    = i_service_name_size;
                p_sys->p_services[p_sys->i_nb_services - 1].i_provider_name_size
                    = i_provider_name_size;
                p_sys->p_services[p_sys->i_nb_services - 1].i_service_type
                    = i_service_type;

                psz_parser = psz_next_service;
            }

            UpdateTable( p_table );
        }
    }
    free( val.psz_string );

    p_table->i_peak_bitrate = T_STD_PEAK_RATE;
    p_table->i_priority = TSPACK_PRIORITY_SI;
    p_table->pf_send = Send;
    tstable_Force( p_table );

    msg_Dbg( p_table, "setting up SDT TSID %u ONID %u mode %s",
             p_ts_stream->i_tsid, p_ts_stream->i_nid,
             p_sys->b_auto ? "auto" : "manual" );

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
    free( p_sys->p_services );
    free( p_sys );
}

/*****************************************************************************
 * BuildServices: in auto mode, build SDT from PMT and NIT tables and returns
 * whether there is a new SDT or not
 *****************************************************************************/
static int ServiceCompare( const void *_p_service1, const void *_p_service2 )
{
    const sdt_service_t *p_service1 = (const sdt_service_t *)_p_service1;
    const sdt_service_t *p_service2 = (const sdt_service_t *)_p_service2;
    return p_service1->i_sid - p_service2->i_sid;
}

static bool BuildServices( ts_table_t *p_table )
{
    ts_packetizer_sys_t *p_sys = p_table->p_sys;
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    sdt_service_t *p_services = NULL;
    int i_nb_services = 0;
    int i;

    p_table->i_last_stream_version = p_ts_stream->i_stream_version;

    for ( i = 0; i < p_ts_stream->i_nb_tables; i++ )
    {
        sout_stream_id_t *p_table = p_ts_stream->pp_tables[i];
        ts_table_t *p_packetizer = (ts_table_t *)p_table->p_packetizer;

        if ( !p_packetizer->b_defines_program || !p_packetizer->i_program )
            continue;

        i_nb_services++;
        p_services = realloc( p_services,
                              i_nb_services * sizeof(sdt_service_t) );
        memset( &p_services[i_nb_services - 1], 0, sizeof(sdt_service_t) );
        p_services[i_nb_services - 1].i_sid = p_packetizer->i_program;
        p_services[i_nb_services - 1].p_service_name = p_sys->p_service_name;
        p_services[i_nb_services - 1].p_provider_name = p_sys->p_provider_name;
        p_services[i_nb_services - 1].i_service_name_size
            = p_sys->i_service_name_size;
        p_services[i_nb_services - 1].i_provider_name_size
            = p_sys->i_provider_name_size;
        p_services[i_nb_services - 1].i_service_type = p_sys->i_service_type;
    }

    /* Maintain the list in ascending order - this is to get reproduceable
     * behaviour. */
    if ( i_nb_services )
        qsort( p_services, i_nb_services, sizeof(sdt_service_t),
               ServiceCompare );

    /* Check if there has been a change. */
    if ( p_sys->i_nb_services != i_nb_services
          || memcmp( p_sys->p_services, p_services,
                     i_nb_services * sizeof(sdt_service_t) ) )
    {
        free( p_sys->p_services );
        p_sys->p_services = p_services;
        p_sys->i_nb_services = i_nb_services;
        p_sys->i_version++;
        p_sys->i_version %= 32;
        UpdateTable( p_table );
        return true;
    }
    else
    {
        free( p_services );
        return false;
    }
}

/*****************************************************************************
 * UpdateTable:
 *****************************************************************************/
static void UpdateTable( ts_table_t *p_table )
{
    ts_packetizer_sys_t *p_sys = p_table->p_sys;
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    block_t **pp_section;
    int i_service_idx = 0;
    int i_nb_sections = 0;

    block_ChainRelease( p_table->p_last_table );
    p_table->p_last_table = NULL;
    pp_section = &p_table->p_last_table;

    do
    {
        uint8_t *p_section;
        uint8_t *p_service;
        uint16_t j = 0;

        *pp_section = block_New( p_table, PSI_MAX_SIZE + PSI_HEADER_SIZE + 1 );
        (*pp_section)->p_buffer[0] = 0; /* pointer_field */
        p_section = (*pp_section)->p_buffer + 1;

        sdt_init( p_section, true );
        /* set length later */
        psi_set_length( p_section, PSI_MAX_SIZE );
        sdt_set_tsid( p_section, p_ts_stream->i_tsid );
        psi_set_version( p_section, p_sys->i_version );
        psi_set_current( p_section );
        psi_set_section( p_section, i_nb_sections );
        /* set last section in the end */
        sdt_set_onid( p_section, p_ts_stream->i_nid );

        while ( (p_service = sdt_get_service( p_section, j )) != NULL
                  && i_service_idx < p_sys->i_nb_services )
        {
            uint8_t *p_descs;
            uint8_t *p_desc;
            uint16_t i_desclength;

            if ( p_sys->p_services[i_service_idx].i_service_name_size ||
                 p_sys->p_services[i_service_idx].i_provider_name_size )
                i_desclength = DESC48_HEADER_SIZE + 1
                     + p_sys->p_services[i_service_idx].i_service_name_size + 1
                     + p_sys->p_services[i_service_idx].i_provider_name_size;
            else
                i_desclength = 0;

            if ( !sdt_validate_service( p_section, p_service, i_desclength ) )
                /* It can't loop because a service descriptor is necessary
                 * smaller than a section. */
                break;

            sdtn_init( p_service );
            sdtn_set_sid( p_service, p_sys->p_services[i_service_idx].i_sid );
            /* TODO: EIT */
            sdtn_set_running( p_service, 4 ); /* running */
            /* TODO: free_ca */
            sdtn_set_desclength( p_service, i_desclength );
            if ( i_desclength )
            {
                p_descs = sdtn_get_descs( p_service );
                p_desc = descs_get_desc( p_descs, 0 );
                desc48_init( p_desc );
                desc_set_length( p_desc, i_desclength - DESC_HEADER_SIZE );
                desc48_set_type( p_desc,
                    p_sys->p_services[i_service_idx].i_service_type );
                desc48_set_provider( p_desc,
                    p_sys->p_services[i_service_idx].p_provider_name,
                    p_sys->p_services[i_service_idx].i_provider_name_size );
                desc48_set_service( p_desc,
                    p_sys->p_services[i_service_idx].p_service_name,
                    p_sys->p_services[i_service_idx].i_service_name_size );
            }

            j++;
            i_service_idx++;
        }

        sdt_set_length( p_section, p_service - p_section - SDT_HEADER_SIZE );
        (*pp_section)->i_buffer = psi_get_length( p_section ) + PSI_HEADER_SIZE
                                   + 1;
        pp_section = &(*pp_section)->p_next;
        i_nb_sections++;
    }
    while ( i_service_idx < p_sys->i_nb_services );

    *pp_section = NULL;
    pp_section = &p_table->p_last_table;

    while ( *pp_section != NULL )
    {
        psi_set_lastsection( (*pp_section)->p_buffer + 1, i_nb_sections - 1 );
        psi_set_crc( (*pp_section)->p_buffer + 1 );
        pp_section = &(*pp_section)->p_next;
    }

    tstable_UpdateTotalBitrate( p_table );

    msg_Dbg( p_table,
             "new SDT version %u with %d services %d sections, bitrate %u",
             p_sys->i_version, p_sys->i_nb_services, i_nb_sections,
             p_table->i_total_bitrate );
}

/*****************************************************************************
 * Send:
 *****************************************************************************/
static block_t *Send( ts_table_t *p_table, mtime_t i_last_muxing )
{
    ts_packetizer_sys_t *p_sys = p_table->p_sys;
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;

    if ( p_sys->b_auto
          && p_ts_stream->i_stream_version > p_table->i_last_stream_version )
        if ( BuildServices( p_table ) )
            tstable_Force( p_table );

    return tstable_Send( p_table, i_last_muxing );
}
