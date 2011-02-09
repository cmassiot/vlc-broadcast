/*****************************************************************************
 * pmt.c: Program Map Table (ISO/IEC 13818-1)
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
 *  - ISO/IEC 13818-1:2000(E) (MPEG-2 systems)
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

#define SOUT_CFG_PREFIX "sout-ts-pmt-"

#include "ts_packetizer.h"
#include "ts_input.h"
#include "ts_table.h"

#define T_STD_PEAK_RATE         1000000 /* bi/s */
#define DEFAULT_PERIOD          300 /* ms */
#define DEFAULT_MAX_PERIOD      700 /* ms */
#define DEFAULT_OFFSET          150 /* ms */
#define DEFAULT_AUTODELETE      5000 /* ms */

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

#define ES_TEXT N_("Elementary streams PID")
#define ES_LONGTEXT N_( \
    "Set the list of ES pid1:pid2..." )
#define PROGRAM_TEXT N_("Program number")
#define PROGRAM_LONGTEXT N_("Assign a fixed program number (Service ID).")
#define VERSION_TEXT N_("Version")
#define VERSION_LONGTEXT N_("Define the version number of the first table " \
  "(default random)." )
#define AUTODELETE_TEXT N_("Auto-delete delay")
#define AUTODELETE_LONGTEXT N_("Define the delay of inactivity after which an ES is removed from the PMT (in ms, 0 to disable)." )

vlc_module_begin()
    set_shortname( _("PMT TS"))
    set_description( _("PMT TS packetizer") )
    set_capability( "ts packetizer", 0 )
    add_shortcut( "pmt" )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_MUX )

    set_callbacks( Open, Close )

    TS_TABLE_COMMON(0x1fff, DEFAULT_PERIOD, DEFAULT_MAX_PERIOD, DEFAULT_OFFSET)

    add_string( SOUT_CFG_PREFIX "es", "auto", ES_TEXT,
                ES_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "program", -1, PROGRAM_TEXT,
                 PROGRAM_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "version", -1, VERSION_TEXT,
                 VERSION_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "autodelete-delay", DEFAULT_AUTODELETE,
                 AUTODELETE_TEXT, AUTODELETE_LONGTEXT, false )
vlc_module_end()

/*****************************************************************************
 * Local prototypes and structures
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    TS_TABLE_COMMON_OPTIONS,
    "es", "program", "version", "autodelete-delay", NULL
};

typedef struct pmt_es_t
{
    uint16_t i_pid;
    int i_es_version;
} pmt_es_t;

struct ts_packetizer_sys_t
{
    bool b_auto;
    mtime_t i_autodelete, i_last_check;

    pmt_es_t *p_es;
    int i_nb_es;
    uint16_t i_pcr_pid;

    uint8_t i_version;
};

static block_t *Send( ts_table_t *p_table, mtime_t i_last_muxing );
static bool ValidateProgram( ts_table_t *p_table, uint16_t i_program );
static bool BuildES( ts_table_t *p_table, mtime_t i_last_muxing );
static bool CheckES( ts_table_t *p_table, mtime_t i_last_muxing );
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

    p_table->b_defines_program = true;
    p_ts_stream->i_stream_version++;

    p_sys = p_table->p_sys = malloc( sizeof(ts_packetizer_sys_t) );
    memset( p_sys, 0, sizeof(ts_packetizer_sys_t) );
    p_sys->i_nb_es = -1;
    p_sys->p_es = NULL;
    p_sys->i_pcr_pid = 0x1fff;

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
        if ( i_max_period > INT64_C(400000) )
            msg_Warn( p_table, "PMT period shouldn't exceed 400 ms in ATSC systems" );
        break;

    case CONFORMANCE_DVB:
        if ( i_max_period > INT64_C(100000) )
            msg_Warn( p_table, "PMT period shouldn't exceed 100 ms in DVB systems" );
        break;
    }

    var_Get( p_table, SOUT_CFG_PREFIX "program", &val );
    if ( val.i_int != -1 )
    {
        p_table->i_program = val.i_int % 65536;
    }
    else
    {
dynamic_program:
        p_table->i_program = nrand48(subi) % 65536;
    }
    if ( !ValidateProgram( p_table, p_table->i_program ) )
    {
        if ( p_table->i_program == val.i_int )
            msg_Warn( p_table, "invalid program %u", p_table->i_program );
        goto dynamic_program;
    }

    var_Get( p_table, SOUT_CFG_PREFIX "version", &val );
    if ( val.i_int != -1 )
        p_sys->i_version = val.i_int % 32;
    else
        p_sys->i_version = nrand48(subi) % 32;

    var_Get( p_table, SOUT_CFG_PREFIX "es", &val );
    psz_parser = val.psz_string;
    if ( psz_parser == NULL || !*psz_parser || !strcmp( psz_parser, "auto" ) )
    {
        p_sys->b_auto = true;
        BuildES( p_table, 0 );
    }
    else
    {
        p_sys->b_auto = false;
        p_sys->p_es = NULL;
        p_sys->i_nb_es = 0;

        while ( psz_parser != NULL && *psz_parser )
        {
            char *psz_next = strchr( psz_parser, ':' );
            if ( psz_next != NULL )
                *psz_next++ = '\0';

            p_sys->i_nb_es++;
            p_sys->p_es = realloc( p_sys->p_es,
                                   p_sys->i_nb_es * sizeof(pmt_es_t) );
            memset( &p_sys->p_es[p_sys->i_nb_es - 1], 0, sizeof(pmt_es_t) );
            p_sys->p_es[p_sys->i_nb_es - 1].i_pid
                = strtoul( psz_parser, NULL, 0 );
            p_sys->p_es[p_sys->i_nb_es - 1].i_es_version = -1;
            psz_parser = psz_next;
        }

        CheckES( p_table, 0 );
    }
    free( val.psz_string );

    var_Get( p_table, SOUT_CFG_PREFIX "autodelete-delay", &val );
    p_sys->i_autodelete = val.i_int * 1000;
    p_sys->i_last_check = 0;

    p_table->i_peak_bitrate = T_STD_PEAK_RATE;
    p_table->i_priority = TSPACK_PRIORITY_SI;
    p_table->pf_send = Send;
    tstable_Force( p_table );

    msg_Dbg( p_table, "setting up PMT program %u mode %s",
             p_table->i_program, p_sys->b_auto ? "auto" : "manual" );

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
    free( p_sys->p_es );
    free( p_sys );
}

/*****************************************************************************
 * ValidateProgram: check that a program number is not already in use
 *****************************************************************************/
static bool ValidateProgram( ts_table_t *p_table, uint16_t i_program )
{
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    int i;

    if ( i_program == 0 )
        return false; /* reserved for NIT */

    for ( i = 0; i < p_ts_stream->i_nb_tables; i++ )
    {
        ts_table_t *p_packetizer
            = (ts_table_t *)p_ts_stream->pp_tables[i]->p_packetizer;
        if ( p_packetizer->b_defines_program
              && p_packetizer->i_program == i_program )
            return false;
    }
    return true;
}

/*****************************************************************************
 * BuildES: in auto mode, build PMT from inputs and returns whether there is
 * a new PMT or not
 *****************************************************************************/
static int ESCompare( const void *_p_es1, const void *_p_es2 )
{
    const pmt_es_t *p_es1 = (const pmt_es_t *)_p_es1;
    const pmt_es_t *p_es2 = (const pmt_es_t *)_p_es2;
    return p_es1->i_pid - p_es2->i_pid;
}

static bool BuildES( ts_table_t *p_table, mtime_t i_last_muxing )
{
    ts_packetizer_sys_t *p_sys = p_table->p_sys;
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    pmt_es_t *p_es = NULL;
    int i_nb_es = 0;
    uint16_t i_pcr_pid = 0x1fff;
    int i;

    for ( i = 0; i < p_ts_stream->i_nb_inputs; i++ )
    {
        sout_stream_id_t *p_input = p_ts_stream->pp_inputs[i];
        ts_input_t *p_packetizer = (ts_input_t *)p_input->p_packetizer;

        if ( p_sys->i_autodelete
              && p_packetizer->i_last_muxing + p_sys->i_autodelete
                  < i_last_muxing )
            continue;

        i_nb_es++;
        p_es = realloc( p_es, i_nb_es * sizeof(pmt_es_t) );
        memset( &p_es[i_nb_es - 1], 0, sizeof(pmt_es_t) );
        p_es[i_nb_es - 1].i_pid = p_packetizer->i_pid;
        p_es[i_nb_es - 1].i_es_version = p_packetizer->i_es_version;
        if ( p_packetizer->i_pcr_period )
            i_pcr_pid = p_packetizer->i_pid;
    }

    /* Maintain the list in ascending order - this is to get reproduceable
     * behaviour. */
    if ( i_nb_es )
        qsort( p_es, i_nb_es, sizeof(pmt_es_t), ESCompare );

    /* Check if there has been a change. */
    if ( p_sys->i_pcr_pid != i_pcr_pid || p_sys->i_nb_es != i_nb_es
          || memcmp( p_sys->p_es, p_es, i_nb_es * sizeof(pmt_es_t) ) )
    {
        if ( !p_sys->i_nb_es || !i_nb_es )
            p_ts_stream->i_stream_version++;
        free( p_sys->p_es );
        p_sys->p_es = p_es;
        p_sys->i_nb_es = i_nb_es;
        p_sys->i_pcr_pid = i_pcr_pid;
        p_sys->i_version++;
        p_sys->i_version %= 32;
        UpdateTable( p_table );
        p_table->i_last_stream_version = p_ts_stream->i_stream_version;
        return true;
    }
    else
    {
        free( p_es );
        p_table->i_last_stream_version = p_ts_stream->i_stream_version;
        return false;
    }
}

/*****************************************************************************
 * CheckES: in non-auto mode, check whether PIDs are missing or back
 *****************************************************************************/
static bool CheckES( ts_table_t *p_table, mtime_t i_last_muxing )
{
    ts_packetizer_sys_t *p_sys = p_table->p_sys;
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    bool b_changed = p_table->p_last_table == NULL;
    uint16_t i_pcr_pid = 0x1fff;
    int j;

    for ( j = 0; j < p_sys->i_nb_es; j++ )
    {
        sout_stream_id_t *p_input;
        ts_input_t *p_packetizer = NULL;
        int i_es_version;
        int i;

        for ( i = 0; i < p_ts_stream->i_nb_inputs; i++ )
        {
            p_input = p_ts_stream->pp_inputs[i];
            p_packetizer = (ts_input_t *)p_input->p_packetizer;

            if ( p_sys->i_autodelete
                  && p_packetizer->i_last_muxing + p_sys->i_autodelete
                      < i_last_muxing )
                continue;

            if ( p_sys->p_es[j].i_pid == p_packetizer->i_pid )
                break;
        }

        if ( i == p_ts_stream->i_nb_inputs )
        {
            i_es_version = -1;
        }
        else
        {
            i_es_version = p_packetizer->i_es_version;
            if ( p_packetizer->i_pcr_period )
                i_pcr_pid = p_packetizer->i_pid;
        }

        if ( p_sys->p_es[j].i_es_version != i_es_version )
            b_changed = true;
        p_sys->p_es[j].i_es_version = i_es_version;
    }

    if ( b_changed || p_sys->i_pcr_pid != i_pcr_pid )
    {
        p_sys->i_pcr_pid = i_pcr_pid;
        p_sys->i_version++;
        p_sys->i_version %= 32;
        UpdateTable( p_table );
    }
    return b_changed;
}

/*****************************************************************************
 * UpdateTable:
 *****************************************************************************/
static const ts_input_t *FindInput( ts_table_t *p_table, uint16_t i_pid )
{
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    int i;

    for ( i = 0; i < p_ts_stream->i_nb_inputs; i++ )
        if ( p_ts_stream->pp_inputs[i]->p_packetizer->i_pid == i_pid )
            return (ts_input_t *)p_ts_stream->pp_inputs[i]->p_packetizer;
    return NULL;
}

static void UpdateTable( ts_table_t *p_table )
{
    ts_packetizer_sys_t *p_sys = p_table->p_sys;
    block_t **pp_section;
    int i_es_idx = 0;
    uint8_t *p_section;
    uint8_t *p_es;
    int j = 0;

    block_ChainRelease( p_table->p_last_table );
    p_table->p_last_table = NULL;
    pp_section = &p_table->p_last_table;

    if ( !p_sys->i_nb_es )
    {
        msg_Dbg( p_table, "no ES left in PMT PID %d, disabling",
                 p_table->i_pid );
        p_table->b_defines_program = false;
        p_table->i_total_bitrate = 0;
        return;
    }
    p_table->b_defines_program = true;

    /* please note that there can be only one section per program (normative) */
    *pp_section = block_New( p_table, PSI_MAX_SIZE + PSI_HEADER_SIZE + 1 );
    (*pp_section)->p_buffer[0] = 0; /* pointer_field */
    p_section = (*pp_section)->p_buffer + 1;

    pmt_init( p_section );
    /* set length later */
    psi_set_length( p_section, PSI_MAX_SIZE );
    pmt_set_program( p_section, p_table->i_program );
    psi_set_version( p_section, p_sys->i_version );
    psi_set_current( p_section );
    pmt_set_pcrpid( p_section, p_sys->i_pcr_pid );
    pmt_set_desclength( p_section, 0 ); /* TODO: scrambling */

    while ( (p_es = pmt_get_es( p_section, j )) != NULL
              && i_es_idx < p_sys->i_nb_es )
    {
        const ts_input_t *p_input = FindInput( p_table,
                                          p_sys->p_es[i_es_idx].i_pid );
        if ( p_input == NULL ) goto next_es;

        if ( !pmt_validate_es( p_section, p_es, p_input->i_descriptors ) )
        {
            msg_Warn( p_table, "PMT is too big and can't be split" );
            break;
        }

        pmtn_init( p_es );
        pmtn_set_streamtype( p_es, p_input->i_stream_type );
        pmtn_set_pid( p_es, p_input->i_pid );
        pmtn_set_desclength( p_es, p_input->i_descriptors );
        memcpy( p_es + PMT_ES_SIZE, p_input->p_descriptors,
                p_input->i_descriptors );
        j++;
next_es:
        i_es_idx++;
    }

    pmt_set_length( p_section, p_es - p_section - PMT_HEADER_SIZE );
    psi_set_crc( p_section );
    (*pp_section)->i_buffer = psi_get_length( p_section ) + PSI_HEADER_SIZE + 1;

    tstable_UpdateTotalBitrate( p_table );

    msg_Dbg( p_table, "new PMT PID %u version %u with %d ES, bitrate %u",
             p_table->i_pid, p_sys->i_version, p_sys->i_nb_es,
             p_table->i_total_bitrate );
}

/*****************************************************************************
 * Send:
 *****************************************************************************/
static block_t *Send( ts_table_t *p_table, mtime_t i_last_muxing )
{
    ts_packetizer_sys_t *p_sys = p_table->p_sys;
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;

    if ( p_ts_stream->i_stream_version > p_table->i_last_stream_version
          || (p_sys->i_autodelete
               && p_sys->i_last_check + p_sys->i_autodelete < i_last_muxing) )
    {
        p_sys->i_last_check = i_last_muxing;
        if ( (p_sys->b_auto && BuildES( p_table, i_last_muxing ))
              || (!p_sys->b_auto && CheckES( p_table, i_last_muxing )) )
            tstable_Force( p_table );
    }

    return tstable_Send( p_table, i_last_muxing );
}
