/*****************************************************************************
 * pat.c: Program Assocation Table (ISO/IEC 13818-1)
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

#define SOUT_CFG_PREFIX "sout-ts-pat-"

#include "ts_packetizer.h"
#include "ts_input.h"
#include "ts_table.h"

#define T_STD_PEAK_RATE         1000000 /* bi/s */
#define DEFAULT_PERIOD          300 /* ms */
#define DEFAULT_MAX_PERIOD      700 /* ms */
#define DEFAULT_OFFSET          0

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

#define PROGRAMS_TEXT N_("Programs number/PID")
#define PROGRAMS_LONGTEXT N_( \
    "Set the list of programs number/pid:..." )
#define VERSION_TEXT N_("Version")
#define VERSION_LONGTEXT N_("Define the version number of the first table " \
  "(default random)." )

vlc_module_begin()
    set_shortname( _("PAT TS"))
    set_description( _("PAT TS packetizer") )
    set_capability( "ts packetizer", 0 )
    add_shortcut( "pat" )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_MUX )

    set_callbacks( Open, Close )

    TS_TABLE_COMMON(PAT_PID, DEFAULT_PERIOD, DEFAULT_MAX_PERIOD, DEFAULT_OFFSET)

    add_string( SOUT_CFG_PREFIX "programs", "auto", PROGRAMS_TEXT,
                PROGRAMS_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "version", -1, VERSION_TEXT,
                 VERSION_LONGTEXT, false )
vlc_module_end()

/*****************************************************************************
 * Local prototypes and structures
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    TS_TABLE_COMMON_OPTIONS,
    "programs", "version", NULL
};

typedef struct pat_program_t
{
    uint16_t i_program;
    uint16_t i_pid;
} pat_program_t;

struct ts_packetizer_sys_t
{
    bool b_auto;

    pat_program_t *p_programs;
    int i_nb_programs;

    uint8_t i_version;
};

static block_t *Send( ts_table_t *p_table, mtime_t i_last_muxing );
static bool BuildPrograms( ts_table_t *p_table );
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
    p_sys->i_nb_programs = -1;
    p_sys->p_programs = NULL;

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
        if ( i_max_period > INT64_C(100000) )
            msg_Warn( p_table, "PAT period shouldn't exceed 100 ms in ATSC systems" );
        break;

    case CONFORMANCE_DVB:
        if ( i_max_period > INT64_C(100000) )
            msg_Warn( p_table, "PAT period shouldn't exceed 100 ms in DVB systems" );
        break;
    }

    var_Get( p_table, SOUT_CFG_PREFIX "version", &val );
    if ( val.i_int != -1 )
        p_sys->i_version = val.i_int % 32;
    else
        p_sys->i_version = nrand48(subi) % 32;

    var_Get( p_table, SOUT_CFG_PREFIX "programs", &val );
    psz_parser = val.psz_string;
    if ( psz_parser == NULL || !*psz_parser || !strcmp( psz_parser, "auto" ) )
    {
        p_sys->b_auto = true;
        BuildPrograms( p_table );
    }
    else
    {
        p_sys->b_auto = false;
        p_sys->p_programs = NULL;
        p_sys->i_nb_programs = 0;

        while ( psz_parser != NULL && *psz_parser )
        {
            char *psz_pid = strchr( psz_parser, '/' );
            char *psz_next = strchr( psz_parser, ':' );
            if ( psz_next != NULL )
                *psz_next++ = '\0';

            if ( psz_pid == NULL )
            {
                msg_Warn( p_table, "invalid program %s", psz_parser );
                psz_parser = psz_next;
                continue;
            }
            *psz_pid++ = '\0';

            p_sys->i_nb_programs++;
            p_sys->p_programs = realloc( p_sys->p_programs,
                                p_sys->i_nb_programs * sizeof(pat_program_t) );
            p_sys->p_programs[p_sys->i_nb_programs - 1].i_program
                = strtoul( psz_parser, NULL, 0 );
            p_sys->p_programs[p_sys->i_nb_programs - 1].i_pid
                = strtoul( psz_pid, NULL, 0 );

            psz_parser = psz_next;
        }

        UpdateTable( p_table );
    }
    free( val.psz_string );

    p_table->i_peak_bitrate = T_STD_PEAK_RATE;
    p_table->i_priority = TSPACK_PRIORITY_SI;
    p_table->pf_send = Send;
    tstable_Force( p_table );

    msg_Dbg( p_table, "setting up PAT TSID %u mode %s",
             p_ts_stream->i_tsid, p_sys->b_auto ? "auto" : "manual" );

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
    free( p_sys->p_programs );
    free( p_sys );
}

/*****************************************************************************
 * BuildPrograms: in auto mode, build PAT from PMT and NIT tables and returns
 * whether there is a new PAT or not
 *****************************************************************************/
static int ProgramCompare( const void *_p_program1, const void *_p_program2 )
{
    const pat_program_t *p_program1 = (const pat_program_t *)_p_program1;
    const pat_program_t *p_program2 = (const pat_program_t *)_p_program2;
    return p_program1->i_program - p_program2->i_program;
}

static bool BuildPrograms( ts_table_t *p_table )
{
    ts_packetizer_sys_t *p_sys = p_table->p_sys;
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    pat_program_t *p_programs = NULL;
    int i_nb_programs = 0;
    int i;

    p_table->i_last_stream_version = p_ts_stream->i_stream_version;

    for ( i = 0; i < p_ts_stream->i_nb_tables; i++ )
    {
        sout_stream_id_t *p_table = p_ts_stream->pp_tables[i];
        ts_table_t *p_packetizer = (ts_table_t *)p_table->p_packetizer;

        if ( !p_packetizer->b_defines_program )
            continue;

        i_nb_programs++;
        p_programs = realloc( p_programs,
                              i_nb_programs * sizeof(pat_program_t) );
        memset( &p_programs[i_nb_programs - 1], 0, sizeof(pat_program_t) );
        p_programs[i_nb_programs - 1].i_program = p_packetizer->i_program;
        p_programs[i_nb_programs - 1].i_pid = p_packetizer->i_pid;
    }

    /* Maintain the list in ascending order - this is to get reproduceable
     * behaviour. */
    if ( i_nb_programs )
        qsort( p_programs, i_nb_programs, sizeof(pat_program_t),
               ProgramCompare );

    /* Check if there has been a change. */
    if ( p_sys->i_nb_programs != i_nb_programs
          || memcmp( p_sys->p_programs, p_programs,
                     i_nb_programs * sizeof(pat_program_t) ) )
    {
        free( p_sys->p_programs );
        p_sys->p_programs = p_programs;
        p_sys->i_nb_programs = i_nb_programs;
        p_sys->i_version++;
        p_sys->i_version %= 32;
        UpdateTable( p_table );
        return true;
    }
    else
    {
        free( p_programs );
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
    int i_program_idx = 0;
    int i_nb_sections = 0;

    block_ChainRelease( p_table->p_last_table );
    p_table->p_last_table = NULL;
    pp_section = &p_table->p_last_table;

    if ( !p_sys->i_nb_programs )
    {
        msg_Dbg( p_table, "no program left in PAT, disabling" );
        p_table->i_total_bitrate = 0;
        return;
    }

    do
    {
        uint8_t *p_section;
        uint8_t *p_program;
        uint16_t j = 0;

        *pp_section = block_New( p_table, PSI_MAX_SIZE + PSI_HEADER_SIZE + 1 );
        (*pp_section)->p_buffer[0] = 0; /* pointer_field */
        p_section = (*pp_section)->p_buffer + 1;

        pat_init( p_section );
        /* set length later */
        psi_set_length( p_section, PSI_MAX_SIZE );
        pat_set_tsid( p_section, p_ts_stream->i_tsid );
        psi_set_version( p_section, p_sys->i_version );
        psi_set_current( p_section );
        psi_set_section( p_section, i_nb_sections );
        /* set last section in the end */

        while ( (p_program = pat_get_program( p_section, j )) != NULL
                  && i_program_idx < p_sys->i_nb_programs )
        {
            patn_init( p_program );
            patn_set_program( p_program,
                              p_sys->p_programs[i_program_idx].i_program );
            patn_set_pid( p_program, p_sys->p_programs[i_program_idx].i_pid );
            j++;
            i_program_idx++;
        }

        pat_set_length( p_section, p_program - p_section - PAT_HEADER_SIZE );
        (*pp_section)->i_buffer = psi_get_length( p_section ) + PSI_HEADER_SIZE
                                   + 1;
        pp_section = &(*pp_section)->p_next;
        i_nb_sections++;
    }
    while ( i_program_idx < p_sys->i_nb_programs );

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
             "new PAT version %u with %d programs %d sections, bitrate %u",
             p_sys->i_version, p_sys->i_nb_programs, i_nb_sections,
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
        if ( BuildPrograms( p_table ) )
            tstable_Force( p_table );

    return tstable_Send( p_table, i_last_muxing );
}
