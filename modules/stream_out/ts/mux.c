/*****************************************************************************
 * mux.c: muxing code for TS
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
 *  - IETF RFC 3550 (Real-Time Protocol)
 *  - IETF RFC 2038 (MPEG video over Real-Time Protocol)
 */

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_block.h>
#include <vlc_iso_lang.h>
#include <vlc_charset.h>
#include <vlc_modules.h>
#include <vlc_rand.h>

#include <bitstream/mpeg/ts.h>
#include <bitstream/dvb/si.h>
#include <bitstream/ietf/rtp.h>

#define SOUT_CFG_PREFIX "sout-ts-"

#include "ts_packetizer.h"
#include "ts_input.h"
#include "ts_table.h"

#define VBR_DEFAULT_INTERVAL    5 /* ms */
#define MAX_PREPARE_PKT         2 /* packets */
#define MAX_PREPARE_TIME        20 /* ms, must meet both conditions */
#define MAX_DELAYING            200 /* ms */
/* This is dimensioned so that we have time to create all elementary streams
 * before starting. */
#define DEFAULT_ASYNC_DELAY     1000 /* ms */

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

/* common */
#define CONFORMANCE_TEXT N_("Conformance")
#define CONFORMANCE_LONGTEXT N_("Force conformance to a specific standard (required for some inputs)")
#define CHARSET_TEXT N_("Charset")
#define CHARSET_LONGTEXT N_("Set character set to be used in text fields (default ISO_8859-1)")

/* inputs */
#define ID_PID_TEXT N_("Keep PIDs")
#define ID_PID_LONGTEXT N_("Assume PIDs are correct in the input, otherwise assign dynamic PIDs")
#define DYNAMIC_PID_TEXT N_("Start of dynamic PIDs")
#define DYNAMIC_PID_LONGTEXT N_("If dynamic PIDs are enabled, define the first assigned PID, then increment")
#define AUTO_PCR_TEXT N_("Automatic PCR")
#define AUTO_PCR_LONGTEXT N_("Automatically elect a PCR PID from the most suitable inputs; otherwise all user-selected PIDs will carry a PCR (by default video PIDs)")
#define PCR_PERIOD_TEXT N_("PCR period")
#define PCR_PERIOD_LONGTEXT N_("Set default PCR period on the elected PID (can be overriden by input option)")
#define INPUTS_TEXT N_("Input options")
#define INPUTS_LONGTEXT N_("Assign packetizer options to specific IDs or streams: 68{lang=fra}:video{pid=68}:mp2a{data-alignment} (first match)")

/* PSI */
#define TABLES_TEXT N_("PSI tables options")
#define TABLES_LONGTEXT N_("Assign packetizer options to specific PSI tables : pat{period=200}:pmt{rap-advance=6}, or add optional tables")
#define CONFORMANCE_TABLES_TEXT N_("Conformance")
#define CONFORMANCE_TABLES_LONGTEXT N_("In automatic tables mode, force the insertion of mandatory but mostly unused conformance tables")
#define TSID_TEXT N_("TS ID")
#define TSID_LONGTEXT N_("Assign a fixed Transport Stream ID")
#define NID_TEXT N_("Network ID")
#define NID_LONGTEXT N_("Assign a fixed Network ID and Original Network ID (default 0xffff)")

/* mux */
#define MUXMODE_TEXT N_( "Mux mode" )
#define MUXMODE_LONGTEXT N_( "Choose between auto, vbr, cbr, capped-vbr (default capped-vbr or vbr)" )
#define MUXRATE_TEXT N_( "Mux rate" )
#define MUXRATE_LONGTEXT N_( "Define the constant bitrate (CBR) or max bitrate (Capped VBR), in bi/s (default automatic)" )
#define PADDING_TEXT N_( "Padding bitrate" )
#define PADDING_LONGTEXT N_( "Define an amount of padding packets to provision in auto-bitrate mode, just in case, in bi/s" )
#define DROP_TEXT N_( "Drop late packets" )
#define DROP_LONGTEXT N_( "Drop packets that are late compared to the output time (!VBR)" )
#define BURST_TEXT N_( "Burst late packets" )
#define BURST_LONGTEXT N_( "Temporarily burst when there are late packets (!VBR)" )
#define GRANULARITY_TEXT N_( "Granularity" )
#define GRANULARITY_LONGTEXT N_( "Define the number of TS output at once (default 7 in synchronous mode, 1 in asynchronous (file)" )
#define ASYNC_DELAY_TEXT N_( "Asynchronous buffer" )
#define ASYNC_DELAY_LONGTEXT N_( "Define the delay (in ms) that's waited for between the input and the output of frames (useful for PSI rap-advance mode)" )

/* output */
#define RTP_TEXT N_( "RTP" )
#define RTP_LONGTEXT N_( "Prepend an RTP header" )
#define SSRC_TEXT N_( "RTP SSRC" )
#define SSRC_LONGTEXT N_( "Define the synchronization source (eg. 12.42.12.42)" )

static const char * ppsz_conformance[] =
{
    "none", "iso", "atsc", "dvb", "hdmv"
};

static const char * ppsz_muxmode[] =
{
    "auto", "vbr", "cbr", "capped-vbr"
};

vlc_module_begin()
    set_shortname( _("TS mux"))
    set_description( _("MPEG-2 Transport Stream mux") )
    set_capability( "sout stream", 50 )
    add_shortcut( "ts" )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_MUX )

    set_callbacks( Open, Close )

    /* common */
    add_string( SOUT_CFG_PREFIX "conformance", "none", CONFORMANCE_TEXT,
                CONFORMANCE_LONGTEXT, false )
        change_string_list( ppsz_conformance, 0, 0 )
    add_string( SOUT_CFG_PREFIX "charset", "ISO_8859-1", CHARSET_TEXT,
                CHARSET_LONGTEXT, false )

    /* inputs */
    add_bool( SOUT_CFG_PREFIX "es-id-pid", false, ID_PID_TEXT,
              ID_PID_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "dynamic-pid", 66, DYNAMIC_PID_TEXT,
                 DYNAMIC_PID_LONGTEXT, false )
    add_bool( SOUT_CFG_PREFIX "auto-pcr", true, AUTO_PCR_TEXT,
              AUTO_PCR_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "pcr", DEFAULT_PCR_PERIOD,
                 PCR_PERIOD_TEXT, PCR_PERIOD_LONGTEXT, false )
    add_string( SOUT_CFG_PREFIX "inputs", "", INPUTS_TEXT,
                INPUTS_LONGTEXT, false )

    /* tables */
    add_string( SOUT_CFG_PREFIX "tables", "auto", TABLES_TEXT,
                TABLES_LONGTEXT, false )
    add_bool( SOUT_CFG_PREFIX "conformance-tables", false,
              CONFORMANCE_TABLES_TEXT, CONFORMANCE_TABLES_LONGTEXT, false );
    add_integer( SOUT_CFG_PREFIX "tsid", -1, TSID_TEXT,
                 TSID_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "nid", 0xffff, NID_TEXT,
                 NID_LONGTEXT, false )

    /* mux */
    add_string( SOUT_CFG_PREFIX "muxmode", "auto", MUXMODE_TEXT,
                MUXMODE_LONGTEXT, false )
        change_string_list( ppsz_muxmode, 0, 0 )
    add_integer( SOUT_CFG_PREFIX "muxrate", 0, MUXRATE_TEXT,
                 MUXRATE_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "padding", 0, PADDING_TEXT,
                 PADDING_LONGTEXT, false )
    add_bool( SOUT_CFG_PREFIX "drop", false, DROP_TEXT,
              DROP_LONGTEXT, false )
    add_bool( SOUT_CFG_PREFIX "burst", false, BURST_TEXT,
              BURST_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "granularity", 0, GRANULARITY_TEXT,
                 GRANULARITY_LONGTEXT, false )
    add_integer( SOUT_CFG_PREFIX "async-delay", DEFAULT_ASYNC_DELAY,
                 ASYNC_DELAY_TEXT, ASYNC_DELAY_LONGTEXT, false )

    /* output */
    add_bool( SOUT_CFG_PREFIX "rtp", false, RTP_TEXT,
              RTP_LONGTEXT, false )
    add_string( SOUT_CFG_PREFIX "ssrc", "", SSRC_TEXT,
                SSRC_LONGTEXT, false )
vlc_module_end()


/*****************************************************************************
 * Local prototypes and structures
 *****************************************************************************/
static const char *ppsz_sout_options[] = {
    "conformance", "charset",
    "es-id-pid", "dynamic-pid", "auto-pcr", "pcr", "inputs",
    "tables", "conformance-tables", "tsid", "nid",
    "muxmode", "muxrate", "padding", "drop", "burst", "granularity", "async-delay",
    "rtp", "ssrc",
    NULL
};

static void CharsetInit( ts_parameters_t *p_ts_params,
                         const char *psz_charset );
static void CharsetDestroy( ts_parameters_t *p_ts_params );
static uint8_t *CharsetToStream( ts_charset_t *p_charset,
                                 char *psz_string, size_t *pi_out_string );
static void InputParseConfig( sout_stream_t *p_stream, char *psz_inputs );
static void InputDelete( sout_stream_t *p_stream, sout_stream_id_t *p_input );
static sout_stream_id_t *Add ( sout_stream_t *, es_format_t * );
static int Del ( sout_stream_t *, sout_stream_id_t * );
static int Send( sout_stream_t *, sout_stream_id_t *, block_t* );
static void TableParseConfig( sout_stream_t *p_stream, char *psz_tables );
static void TableAdd( sout_stream_t *p_stream, char *psz_name,
                      config_chain_t *p_cfg );
static void TableDel( sout_stream_t *p_stream, sout_stream_id_t *p_table );
static void MuxValidateParams( sout_stream_t *p_stream );
static void *MuxThread( vlc_object_t * );
static void MuxAsync( sout_stream_t *p_stream, bool b_flush );

#define MODE_AUTO   0
#define MODE_VBR    1
#define MODE_CBR    2
#define MODE_CAPPED 3

typedef struct ts_input_cfg_t
{
    char *psz_name;
    config_chain_t *p_cfg;
} ts_input_cfg_t;

struct sout_stream_sys_t
{
    /* For threading stuff */
    VLC_COMMON_MEMBERS

    /* output */
    sout_stream_t *p_stream;
    sout_stream_id_t *id; /* unique output */
    bool b_rtp;
    uint16_t i_rtp_cc;
    uint8_t pi_ssrc[4];

    /* PIDs management */
    uint16_t i_next_dynamic_pid;
    bool b_es_id_pid;

    /* inputs */
    ts_input_cfg_t *p_inputs_cfg;
    int i_nb_inputs_cfg;
    bool b_auto_pcr;
    mtime_t i_auto_pcr_period;
    sout_stream_id_t *p_pcr_input;

    /* stream definition / PSI */
    vlc_mutex_t stream_lock;
    vlc_cond_t stream_wait;
    ts_stream_t ts;

    /* muxing */
    unsigned int i_muxrate, i_muxmode; /* muxrate in byte/s */
    bool b_auto_muxrate, b_auto_muxmode;
    int i_padding_bitrate;
    int i_last_stream_version;
    bool b_drop, b_burst;
    int i_granularity;
    mtime_t i_granularity_size; /* in 1000000 bits for conveniency */
    mtime_t i_async_delay;
    mtime_t i_last_muxing, i_last_muxing_remainder;
    bool b_sync;
    /* temporary buffers for delayed packets */
    block_t *p_tmp_blocks;
    block_t **pp_tmp_last;
    int i_tmp_nb_packets;
};


/*
 * Inits
 */

/*****************************************************************************
 * Open: start the thread
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    sout_stream_t *p_stream = (sout_stream_t *)p_this;
    sout_stream_sys_t *p_sys;
    es_format_t fmt;
    vlc_value_t val;
    unsigned short subi[3];

    p_stream->p_sys = p_sys = vlc_object_create( p_stream, sizeof(sout_stream_sys_t) );
    vlc_object_attach( p_sys, p_stream );
    vlc_rand_bytes( subi, sizeof(subi) );

    /* output */
    p_sys->p_stream = p_stream;
    if( !p_stream->p_next )
    {
        msg_Err( p_stream, "cannot create chain" );
        vlc_object_release( p_sys );
        return VLC_EGENERIC;
    }
    p_sys->b_sync = !!p_stream->p_sout->i_out_pace_nocontrol;

    config_ChainParse( p_stream, SOUT_CFG_PREFIX, ppsz_sout_options,
                   p_stream->p_cfg );

    var_Get( p_stream, SOUT_CFG_PREFIX "rtp", &val );
    p_sys->b_rtp = val.b_bool;

    memset( &fmt, 0, sizeof(es_format_t) );
    if ( p_sys->b_rtp )
    {
        fmt.i_codec = VLC_CODEC_RTP;
        p_sys->i_rtp_cc = nrand48(subi) & 0xffff;
        p_sys->pi_ssrc[0] = nrand48(subi) & 0xff;
        p_sys->pi_ssrc[1] = nrand48(subi) & 0xff;
        p_sys->pi_ssrc[2] = nrand48(subi) & 0xff;
        p_sys->pi_ssrc[3] = nrand48(subi) & 0xff;

        var_Get( p_stream, SOUT_CFG_PREFIX "ssrc", &val );
        if ( val.psz_string != NULL && *val.psz_string )
        {
            struct in_addr maddr;
            if ( !inet_aton( val.psz_string, &maddr ) )
                msg_Warn( p_stream, "invalid RTP SSRC %s", val.psz_string );
            else
                memcpy( p_sys->pi_ssrc, &maddr.s_addr, 4 );
        }
        free( val.psz_string );
    }
    else
    {
        fmt.i_codec = VLC_CODEC_M2TS;
    }

    if ( (p_sys->id = p_stream->p_next->pf_add( p_stream->p_next, &fmt )) == NULL )
    {
        msg_Err( p_stream, "cannot create chain" );
        vlc_object_release( p_sys );
        return VLC_EGENERIC;
    }

    /* inputs */
    vlc_mutex_init( &p_sys->stream_lock );
    vlc_cond_init( &p_sys->stream_wait );

    var_Get( p_stream, SOUT_CFG_PREFIX "conformance", &val );
    if ( val.psz_string == NULL || !*val.psz_string
          || !strcmp( val.psz_string, "none" ) )
        p_sys->ts.params.i_conformance = CONFORMANCE_NONE;
    else if ( !strcmp( val.psz_string, "iso" ) )
        p_sys->ts.params.i_conformance = CONFORMANCE_ISO;
    else if ( !strcmp( val.psz_string, "atsc" ) )
        p_sys->ts.params.i_conformance = CONFORMANCE_ATSC;
    else if ( !strcmp( val.psz_string, "dvb" ) )
        p_sys->ts.params.i_conformance = CONFORMANCE_DVB;
    else if ( !strcmp( val.psz_string, "hdmv" ) )
        p_sys->ts.params.i_conformance = CONFORMANCE_HDMV;
    else
    {
        msg_Warn( p_stream, "invalid conformance %s", val.psz_string );
        p_sys->ts.params.i_conformance = CONFORMANCE_NONE;
    }
    free( val.psz_string );

    var_Get( p_stream, SOUT_CFG_PREFIX "charset", &val );
    CharsetInit( &p_sys->ts.params, val.psz_string );
    free( val.psz_string );

    var_Get( p_stream, SOUT_CFG_PREFIX "es-id-pid", &val );
    p_sys->b_es_id_pid = val.b_bool;

    var_Get( p_stream, SOUT_CFG_PREFIX "dynamic-pid", &val );
    p_sys->i_next_dynamic_pid = val.i_int;

    var_Get( p_stream, SOUT_CFG_PREFIX "auto-pcr", &val );
    p_sys->b_auto_pcr = val.b_bool;
    p_sys->p_pcr_input = NULL;

    var_Get( p_stream, SOUT_CFG_PREFIX "pcr", &val );
    p_sys->i_auto_pcr_period = val.i_int * 1000;

    var_Get( p_stream, SOUT_CFG_PREFIX "inputs", &val );
    InputParseConfig( p_stream, val.psz_string );

    /* muxing */
    var_Get( p_stream, SOUT_CFG_PREFIX "drop", &val );
    p_sys->b_drop = val.b_bool;

    var_Get( p_stream, SOUT_CFG_PREFIX "burst", &val );
    p_sys->b_burst = val.b_bool;

    var_Get( p_stream, SOUT_CFG_PREFIX "granularity", &val );
    if ( val.i_int )
        p_sys->i_granularity = val.i_int;
    else if ( p_sys->b_sync )
        p_sys->i_granularity = 7;
    else
        p_sys->i_granularity = 1;
    p_sys->i_granularity_size = p_sys->i_granularity * TS_SIZE
                                   * INT64_C(1000000);

    var_Get( p_stream, SOUT_CFG_PREFIX "padding", &val );
    p_sys->i_padding_bitrate = val.i_int;

    var_Get( p_stream, SOUT_CFG_PREFIX "muxrate", &val );
    p_sys->i_muxrate = (val.i_int + 7) / 8;

    var_Get( p_stream, SOUT_CFG_PREFIX "muxmode", &val );
    if ( val.psz_string == NULL || !*val.psz_string
          || !strcmp( val.psz_string, "auto" ) )
    {
        p_sys->b_auto_muxmode = true;
        p_sys->b_auto_muxrate = true;
    }
    else if ( !strcmp( val.psz_string, "vbr" ) )
    {
        p_sys->b_auto_muxmode = false;

        p_sys->i_muxmode = MODE_VBR;
        MuxValidateParams( p_stream );
    }
    else if ( !strcmp( val.psz_string, "capped-vbr" )
               || !strcmp( val.psz_string, "cbr" ) )
    {
        p_sys->b_auto_muxmode = false;

        if ( !strcmp( val.psz_string, "cbr" ) )
            p_sys->i_muxmode = MODE_CBR;
        else
            p_sys->i_muxmode = MODE_CAPPED;

        p_sys->b_auto_muxrate = (p_sys->i_muxrate == 0);

        if ( !p_sys->b_auto_muxrate )
            MuxValidateParams( p_stream );
    }
    else
    {
        msg_Warn( p_stream, "invalid muxmode %s", val.psz_string );
        p_sys->b_auto_muxmode = true;
        p_sys->b_auto_muxrate = true;
    }
    free( val.psz_string );

    var_Get( p_stream, SOUT_CFG_PREFIX "async-delay", &val );
    p_sys->i_async_delay = val.i_int * 1000;

    /* TS stream / PSI - in the end because the operating mode must be
     * known first */
    p_sys->ts.i_stream_version = 0;
    p_sys->ts.pi_raps = NULL;
    p_sys->ts.i_nb_raps = 0;

    p_sys->ts.pp_inputs = NULL;
    p_sys->ts.i_nb_inputs = 0;

    p_sys->ts.pp_tables = NULL;
    p_sys->ts.i_nb_tables = 0;

    var_Get( p_stream, SOUT_CFG_PREFIX "tsid", &val );
    if ( val.i_int != -1 )
        p_sys->ts.i_tsid = val.i_int % 65536;
    else
        p_sys->ts.i_tsid = nrand48(subi) % 65536;
    var_Get( p_stream, SOUT_CFG_PREFIX "nid", &val );
    p_sys->ts.i_nid = val.i_int % 65536;

    var_Get( p_stream, SOUT_CFG_PREFIX "tables", &val );
    if ( val.psz_string != NULL && !strcmp( val.psz_string, "auto" ) )
    {
        vlc_value_t val2;
        var_Get( p_stream, SOUT_CFG_PREFIX "conformance-tables", &val2 );

        switch ( p_sys->ts.params.i_conformance )
        {
        default:
        case CONFORMANCE_NONE:
        case CONFORMANCE_ISO:
        case CONFORMANCE_HDMV:
            TableParseConfig( p_stream, strdup("pat:pmt") );
            break;

        case CONFORMANCE_DVB:
            if ( val2.b_bool )
            {
                if ( p_sys->b_sync )
                    TableParseConfig( p_stream, strdup("pat:pmt:nit:sdt:tdt") );
                else
                    TableParseConfig( p_stream, strdup("pat:pmt:nit:sdt") );
            }
            else
                TableParseConfig( p_stream, strdup("pat:pmt") );
            break;

        case CONFORMANCE_ATSC:
            if ( val2.b_bool )
#if 0
                TableParseConfig( p_stream, strdup("pat:pmt:nit:mgt:eit:rrt:stt") );
#else
                msg_Warn( p_stream, "ATSC conformance tables are currently unimplemented" );
#endif
#if 0
            else
#endif
                TableParseConfig( p_stream, strdup("pat:pmt") );
            break;
        }
        free( val.psz_string );
    }
    else
    {
        TableParseConfig( p_stream, val.psz_string );
        /* No need to free val.psz_string because TableParseConfig frees
         * its argument (ugly). */
    }

    /* Start of operations */
    p_sys->i_last_muxing = -1;
    p_sys->i_last_muxing_remainder = 0;
    p_sys->p_tmp_blocks = NULL;
    p_sys->pp_tmp_last = NULL;
    p_sys->i_tmp_nb_packets = 0;

    if( p_sys->b_sync && vlc_thread_create( p_sys, "sout mux thread", MuxThread,
                                            VLC_THREAD_PRIORITY_OUTPUT ) )
    {
        msg_Err( p_sys, "cannot spawn sout mux thread" );
        vlc_object_release( p_sys );
        return VLC_EGENERIC;
    }
    else
        msg_Dbg( p_stream, "starting TS mux with %s conformance",
                 ppsz_conformance[p_sys->ts.params.i_conformance] );


    p_stream->pf_add    = Add;
    p_stream->pf_del    = Del;
    p_stream->pf_send   = Send;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: shut it down
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    sout_stream_t *p_stream = (sout_stream_t *)p_this;
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    int i;

    if ( p_sys->b_sync )
    {
        vlc_mutex_lock( &p_sys->stream_lock );
        vlc_object_kill( p_sys );
        vlc_cond_signal( &p_sys->stream_wait );
        vlc_mutex_unlock( &p_sys->stream_lock );
        vlc_thread_join( p_sys );
    }
    else
    {
        MuxAsync( p_stream, true );
    }

    p_stream->p_next->pf_del( p_stream->p_next, p_sys->id );

    for ( i = p_sys->ts.i_nb_inputs - 1; i >= 0; i-- )
        InputDelete( p_stream, p_sys->ts.pp_inputs[i] );

    for ( i = p_sys->ts.i_nb_tables - 1; i >= 0; i-- )
        TableDel( p_stream, p_sys->ts.pp_tables[i] );

    for ( i = p_sys->i_nb_inputs_cfg - 1; i >= 0; i-- )
    {
        free( p_sys->p_inputs_cfg[i].p_cfg );
        free( p_sys->p_inputs_cfg[i].psz_name );
    }
    free( p_sys->p_inputs_cfg );
    free( p_sys->ts.pi_raps );

    CharsetDestroy( &p_sys->ts.params );

    vlc_mutex_destroy( &p_sys->stream_lock );
    vlc_cond_destroy( &p_sys->stream_wait );

    vlc_object_release( p_sys );
    p_stream->p_sout->i_out_pace_nocontrol--;
}


/*
 * Charset conversions
 */

struct ts_charset_t
{
    char *psz_charset;
    vlc_iconv_t iconv_handle;
};

/*****************************************************************************
 * CharsetInit: allocate data structures
 *****************************************************************************/
static void CharsetInit( ts_parameters_t *p_ts_params, const char *psz_charset )
{
    ts_charset_t *p_charset = p_ts_params->p_charset
        = malloc(sizeof(ts_charset_t));
    p_charset->iconv_handle = (vlc_iconv_t)-1;
    if ( strcasecmp( psz_charset, "UTF-8" ) )
        p_charset->iconv_handle = vlc_iconv_open( psz_charset, "UTF-8" );
    p_charset->psz_charset = strdup( psz_charset );
    p_ts_params->pf_charset = CharsetToStream;
}

/*****************************************************************************
 * CharsetDestroy: deallocate data structures
 *****************************************************************************/
static void CharsetDestroy( ts_parameters_t *p_ts_params )
{
    ts_charset_t *p_charset = p_ts_params->p_charset;

    if ( p_charset->iconv_handle != (vlc_iconv_t)-1 )
        vlc_iconv_close( p_charset->iconv_handle );
    free( p_charset->psz_charset );
    free( p_charset );
}

/*****************************************************************************
 * CharsetToStream: convert a UTF-8 string to stream encoding
 *****************************************************************************/
static uint8_t *CharsetToStream( ts_charset_t *p_charset,
                                 char *psz_string, size_t *pi_out_string )
{
    uint8_t *p_ret;
    char *p_tmp = NULL;
    size_t i_tmp;

    if ( p_charset->iconv_handle != (vlc_iconv_t)-1 )
    {
        size_t i = strlen( psz_string );
        char *p;

        i_tmp = i * 6;
        p_tmp = p = malloc(i_tmp);

        if ( vlc_iconv( p_charset->iconv_handle, (const char **)&psz_string, &i, &p, &i_tmp )
              == (size_t)-1 )
        {
            free( p_tmp );
            p_tmp = NULL;
        }
        else
            i_tmp = p - p_tmp;
    }

    if ( p_tmp == NULL )
    {
        p_tmp = strdup(psz_string);
        i_tmp = strlen(psz_string);
    }

    p_ret = dvb_string_set( (uint8_t *)p_tmp, i_tmp, p_charset->psz_charset,
                            pi_out_string );
    free( p_tmp );
    return p_ret;
}


/*
 * Generic PID management (for inputs and tables)
 */

/*****************************************************************************
 * PIDValidate: check that the PID is valid and unused
 *****************************************************************************/
static bool PIDValidate( sout_stream_t *p_stream, uint16_t i_pid )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    int i;

    if ( i_pid >= 0x1fff ) /* reserved */
        return false;

    vlc_mutex_lock( &p_sys->stream_lock );
    for ( i = 0; i < p_sys->ts.i_nb_inputs; i++ )
    {
        sout_stream_id_t *p_input = p_sys->ts.pp_inputs[i];
        if ( p_input->p_packetizer->i_pid == i_pid )
        {
            vlc_mutex_unlock( &p_sys->stream_lock );
            return false;
        }
    }
    for ( i = 0; i < p_sys->ts.i_nb_tables; i++ )
    {
        sout_stream_id_t *p_table = p_sys->ts.pp_tables[i];
        if ( p_table->p_packetizer->i_pid == i_pid )
        {
            vlc_mutex_unlock( &p_sys->stream_lock );
            return false;
        }
    }
    vlc_mutex_unlock( &p_sys->stream_lock );

    return true;
}

/*****************************************************************************
 * PIDAllocate: find the appropriate PID
 *****************************************************************************/
static uint16_t PIDAllocate( sout_stream_t *p_stream,
                             sout_stream_id_t *p_input, int i_es_id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    uint16_t i_pid;

    if ( p_input->p_packetizer->i_cfg_pid != 0x1fff )
        i_pid = p_input->p_packetizer->i_cfg_pid;

    else if ( p_sys->b_es_id_pid && i_es_id != -1 )
        i_pid = i_es_id & 0x1fff;

    else
    {
dynamic_pid:
        do
        {
            i_pid = p_sys->i_next_dynamic_pid++;
            if ( p_sys->i_next_dynamic_pid == 0x1fff )
                p_sys->i_next_dynamic_pid = 0x10;
        }
        while ( !PIDValidate( p_stream, i_pid ) );
        return i_pid;
    }

    if ( !PIDValidate( p_stream, i_pid ) )
    {
        msg_Warn( p_stream, "invalid PID %u", i_pid );
        goto dynamic_pid;
    }
    return i_pid;
}


/*
 * Inputs
 */

/*****************************************************************************
 * InputParseConfig: parse the "inputs" option
 *****************************************************************************/
static void InputParseConfig( sout_stream_t *p_stream, char *psz_inputs )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    while ( psz_inputs != NULL )
    {
        config_chain_t *p_cfg;
        char *psz_name, *psz_next;

        psz_next = config_ChainCreate( &psz_name, &p_cfg, psz_inputs );
        free( psz_inputs );
        psz_inputs = psz_next;

        if ( p_cfg != NULL )
        {
            p_sys->p_inputs_cfg = realloc( p_sys->p_inputs_cfg,
                         ++p_sys->i_nb_inputs_cfg * sizeof(ts_input_cfg_t) );
            p_sys->p_inputs_cfg[p_sys->i_nb_inputs_cfg - 1].p_cfg = p_cfg;
            p_sys->p_inputs_cfg[p_sys->i_nb_inputs_cfg - 1].psz_name = psz_name;
        }
        else
            free(psz_name);
    }
}

/*****************************************************************************
 * InputUndelete: try to match a deleted input and undelete it
 *****************************************************************************/
static sout_stream_id_t *InputUndelete( sout_stream_t *p_stream,
                                        const es_format_t *p_fmt )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    int i;

    vlc_mutex_lock( &p_sys->stream_lock );
    for ( i = 0; i < p_sys->ts.i_nb_inputs; i++ )
    {
        sout_stream_id_t *p_input = p_sys->ts.pp_inputs[i];
        ts_input_t *p_packetizer = (ts_input_t *)p_input->p_packetizer;
        if ( p_input->b_deleted
              && !memcmp( &p_packetizer->fmt, p_fmt, sizeof(es_format_t) ) )
        {
            p_input->b_deleted = false;
            vlc_mutex_unlock( &p_sys->stream_lock );
            return p_input;
        }
    }
    vlc_mutex_unlock( &p_sys->stream_lock );
    return NULL;
}

/*****************************************************************************
 * InputMatches: decide whether an option is for an input or not
 *****************************************************************************/
static bool InputMatches( sout_stream_t *p_stream,
                                const es_format_t *p_fmt, const char *psz_name )
{
    VLC_UNUSED(p_stream);
    char *psz_error;
    int i_id = strtol( psz_name, &psz_error, 0 );
    if ( !*psz_error )
        return (i_id == p_fmt->i_id);

    if ( strlen(psz_name) == 3
          && (p_fmt->i_codec ==
              VLC_FOURCC(psz_name[0], psz_name[1], psz_name[2], ' ')) )
        return true;
    else if ( strlen(psz_name) == 4
          && (p_fmt->i_codec ==
              VLC_FOURCC(psz_name[0], psz_name[1], psz_name[2], psz_name[3])) )
        return true;

    if ( !strcmp( psz_name, "video" ) && p_fmt->i_cat == VIDEO_ES )
        return true;
    if ( !strcmp( psz_name, "audio" ) && p_fmt->i_cat == AUDIO_ES )
        return true;
    if ( !strcmp( psz_name, "spu" ) && p_fmt->i_cat == SPU_ES )
        return true;

    return false;
}

/*****************************************************************************
 * InputConfig: look at the input options and see if there is something for us
 *****************************************************************************/
static config_chain_t *InputConfig( sout_stream_t *p_stream,
                                sout_stream_id_t *p_input )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    ts_input_t *p_packetizer = (ts_input_t *)p_input->p_packetizer;
    int i;

    for ( i = 0; i < p_sys->i_nb_inputs_cfg; i++ )
        if ( InputMatches( p_stream, &p_packetizer->fmt,
                           p_sys->p_inputs_cfg[i].psz_name ) )
            return p_sys->p_inputs_cfg[i].p_cfg;

    return NULL;
}

/*****************************************************************************
 * InputValidatePCR: warn if PCR period isn't compliant
 *****************************************************************************/
static bool InputValidatePCR( ts_input_t *p_input )
{
    if ( p_input->i_bitrate )
        p_input->i_pcr_tolerance = TS_SIZE * INT64_C(8000000) / p_input->i_bitrate;
    else
        p_input->i_pcr_tolerance = DEFAULT_PCR_TOLERANCE * 1000;

    switch ( p_input->p_ts_params->i_conformance )
    {
    default:
    case CONFORMANCE_NONE:
        return true;

    case CONFORMANCE_ISO:
    case CONFORMANCE_ATSC:
    case CONFORMANCE_DVB: /* legend says 40 but really 100 */
    case CONFORMANCE_HDMV:
        if ( p_input->i_pcr_period + p_input->i_pcr_tolerance > INT64_C(100000) )
        {
            msg_Warn( p_input, "PCR period shouldn't exceed 100 ms (%"PRId64" + %"PRId64")",
                      p_input->i_pcr_period, p_input->i_pcr_tolerance );
            return false;
        }
        return true;
    }
}

/*****************************************************************************
 * InputElectPCR: determine the input for PCR / called with the stream_lock
 *****************************************************************************/
static void InputElectPCR( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_t *p_pcr_input = p_sys->p_pcr_input;
    int i;

    if ( !p_sys->ts.i_nb_inputs )
        return;

    for ( i = 0; i < p_sys->ts.i_nb_inputs; i++ )
    {
        sout_stream_id_t *p_input = p_sys->ts.pp_inputs[i];
        ts_input_t *p_packetizer = (ts_input_t *)p_input->p_packetizer;

        if ( (p_pcr_input == NULL && p_packetizer->fmt.i_cat == AUDIO_ES)
               || p_packetizer->fmt.i_cat == VIDEO_ES )
        {
            p_pcr_input = p_input;
        }
        else if ( p_packetizer->i_cfg_pcr_period )
        {
            p_pcr_input = p_input;
            break;
        }
    }

    if ( p_pcr_input != p_sys->p_pcr_input )
    {
        if ( p_sys->p_pcr_input != NULL )
        {
            ts_input_t *p_old_packetizer
                = (ts_input_t *)p_sys->p_pcr_input->p_packetizer;
            p_old_packetizer->i_pcr_period = 0;
            p_old_packetizer->i_priority = TSPACK_PRIORITY_NONE;
        }

        p_sys->p_pcr_input = p_pcr_input;
        p_sys->ts.i_stream_version++;

        if ( p_pcr_input != NULL )
        {
            ts_input_t *p_packetizer = (ts_input_t *)p_pcr_input->p_packetizer;
            p_packetizer->i_pcr_period = p_packetizer->i_cfg_pcr_period;
            if ( !p_packetizer->i_pcr_period )
                p_packetizer->i_pcr_period = p_sys->i_auto_pcr_period;
            p_packetizer->i_priority = TSPACK_PRIORITY_PCR;
            InputValidatePCR( p_packetizer );

            msg_Dbg( p_stream, "new PCR PID is %d period=%"PRId64,
                     p_packetizer->i_pid, p_packetizer->i_pcr_period );
        }
        else
            msg_Dbg( p_stream, "new PCR PID is 8191" );
    }
}

/*****************************************************************************
 * InputDelete: remove input / called with stream_lock
 *****************************************************************************/
static void InputDelete( sout_stream_t *p_stream, sout_stream_id_t *p_input )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    ts_input_t *p_packetizer = (ts_input_t *)p_input->p_packetizer;

    msg_Dbg( p_stream, "removing PID %u (%4.4s/%d)",
             p_packetizer->i_pid,
             (const char *)&p_packetizer->fmt.i_codec,
             p_packetizer->fmt.i_id );

    TAB_REMOVE( p_sys->ts.i_nb_inputs, p_sys->ts.pp_inputs, p_input );
    p_sys->ts.i_stream_version++;
    if ( p_sys->b_auto_pcr && p_sys->p_pcr_input == p_input )
    {
        p_sys->p_pcr_input = NULL;
        InputElectPCR( p_stream );
    }

    module_unneed( p_packetizer, p_packetizer->p_module );
    /* free( p_packetizer->p_cfg ); - not now, only at the end of mux */
    vlc_object_release( p_packetizer );

    block_FifoRelease( p_input->p_fifo );
    free( p_input );
}

/*****************************************************************************
 * InputCheckRAP: check TS header for a random access point
 *****************************************************************************/
static void InputCheckRAP( sout_stream_t *p_stream, block_t *p_block )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    while ( p_block != NULL )
    {
        if ( ts_has_adaptation( p_block->p_buffer )
              && ts_get_adaptation( p_block->p_buffer )
              && tsaf_has_randomaccess( p_block->p_buffer ) )
        {
            vlc_mutex_lock( &p_sys->stream_lock );
            p_sys->ts.pi_raps = realloc( p_sys->ts.pi_raps,
                                ++p_sys->ts.i_nb_raps * sizeof(mtime_t) );
            p_sys->ts.pi_raps[p_sys->ts.i_nb_raps - 1] = p_block->i_dts
                                                          - p_block->i_delay;
            vlc_mutex_unlock( &p_sys->stream_lock );
        }

        p_block = p_block->p_next;
    }
}

/*****************************************************************************
 * Add: new input
 *****************************************************************************/
static sout_stream_id_t *Add( sout_stream_t *p_stream, es_format_t *p_fmt )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_t *p_input;
    ts_input_t *p_packetizer;

    p_input = InputUndelete( p_stream, p_fmt );
    if ( p_input != NULL )
        return p_input;

    p_input = malloc( sizeof( sout_stream_id_t ) );
    memset( p_input, 0, sizeof( sout_stream_id_t ) );
    p_input->p_fifo = block_FifoNew();
    p_input->b_deleted = false;
    p_input->i_min_muxing = 0;

    p_packetizer = (ts_input_t *)vlc_object_create( p_stream,
                                                    sizeof(ts_input_t) );
    p_input->p_packetizer = (ts_packetizer_t *)p_packetizer;
    vlc_object_attach( p_packetizer, p_stream );

    p_packetizer->fmt = *p_fmt;
    p_packetizer->p_ts_params = &p_sys->ts.params;
    p_packetizer->p_cfg = InputConfig( p_stream, p_input );
    p_packetizer->p_module =
        module_need( p_packetizer, "ts packetizer", NULL, false );
    if ( p_packetizer->p_module == NULL )
    {
        vlc_object_release( p_packetizer );

        block_FifoRelease( p_input->p_fifo );
        free( p_input );
        return NULL;
    }

    p_packetizer->i_pid = PIDAllocate( p_stream, p_input,
                                       p_packetizer->fmt.i_id );

    vlc_mutex_lock( &p_sys->stream_lock );
    TAB_APPEND( p_sys->ts.i_nb_inputs, p_sys->ts.pp_inputs, p_input );
    p_sys->ts.i_stream_version++;
    if ( p_sys->b_auto_pcr )
    {
        InputElectPCR( p_stream );
    }
    else
    {
        p_packetizer->i_pcr_period = p_packetizer->i_cfg_pcr_period;
        if ( p_packetizer->i_pcr_period )
        {
            InputValidatePCR( p_packetizer );
            p_packetizer->i_priority = TSPACK_PRIORITY_PCR;
        }
    }
    vlc_mutex_unlock( &p_sys->stream_lock );

    msg_Dbg( p_stream, "adding PID %u (%4.4s/%d)", p_packetizer->i_pid,
             (const char *)&p_packetizer->fmt.i_codec, p_packetizer->fmt.i_id );

    return p_input;
}

/*****************************************************************************
 * Del: signal in-band that the input is to be removed
 *****************************************************************************/
static int Del( sout_stream_t *p_stream, sout_stream_id_t *p_input )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    int i_depth;

    vlc_mutex_lock( &p_sys->stream_lock );

    vlc_mutex_lock( &p_input->p_fifo->lock );
    i_depth = p_input->p_fifo->i_depth;
    vlc_mutex_unlock( &p_input->p_fifo->lock );

    p_input->b_deleted = true;
    if ( i_depth )
    {
        ts_input_t *p_packetizer = (ts_input_t *)p_input->p_packetizer;
        msg_Dbg( p_stream, "scheduled removal of PID %u (%4.4s/%d)",
                 p_packetizer->i_pid, (const char *)&p_packetizer->fmt.i_codec,
                 p_packetizer->fmt.i_id );
    }
    else
    {
        InputDelete( p_stream, p_input );
    }

    vlc_mutex_unlock( &p_sys->stream_lock );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Send: new packet for a PID
 *****************************************************************************/
static int Send( sout_stream_t *p_stream, sout_stream_id_t *p_input,
                 block_t *p_in )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    ts_input_t *p_packetizer = (ts_input_t *)p_input->p_packetizer;
    block_t *p_out;

    for ( block_t *p_block = p_in; p_block != NULL; p_block = p_block->p_next )
    {
        if ( p_block->i_dts == VLC_TS_INVALID ||
             p_block->i_pts == VLC_TS_INVALID )
        {
            msg_Warn( p_stream, "packet with invalid timestamp on PID %hu",
                      p_packetizer->i_pid );
            block_ChainRelease( p_in );
            return VLC_SUCCESS;
        }
    }

    p_out = p_packetizer->pf_send( p_packetizer, p_in );

    if ( p_out != NULL )
    {
        mtime_t i_last_muxing;
        vlc_mutex_lock( &p_sys->stream_lock );
        i_last_muxing = p_sys->i_last_muxing;
        vlc_mutex_unlock( &p_sys->stream_lock );

        if ( p_packetizer->fmt.i_cat == VIDEO_ES )
            InputCheckRAP( p_stream, p_out );
        if ( p_out->i_dts - p_out->i_delay
              < p_sys->i_last_muxing + p_sys->ts.params.i_max_prepare )
            msg_Warn( p_stream, "received late buffer PID %u (%"PRId64")",
                      p_packetizer->i_pid,
                      p_sys->i_last_muxing
                       + p_sys->ts.params.i_max_prepare
                       - p_out->i_dts + p_out->i_delay );

        block_FifoPut( p_input->p_fifo, p_out );

        if ( p_sys->b_sync )
        {
            vlc_mutex_lock( &p_sys->stream_lock );
            vlc_cond_signal( &p_sys->stream_wait );
            vlc_mutex_unlock( &p_sys->stream_lock );
        }
        else
            MuxAsync( p_stream, false );
    }

    return VLC_SUCCESS;
}


/*
 * Tables
 */

/*****************************************************************************
 * TableParseConfig: parse the "tables" option, and start PSI tables
 *****************************************************************************/
static void TableParseConfig( sout_stream_t *p_stream, char *psz_tables )
{
    while ( psz_tables != NULL )
    {
        config_chain_t *p_cfg;
        char *psz_name, *psz_next;

        psz_next = config_ChainCreate( &psz_name, &p_cfg, psz_tables );
        free( psz_tables );
        psz_tables = psz_next;

        if ( psz_name != NULL )
            TableAdd( p_stream, psz_name, p_cfg );
    }
}

/*****************************************************************************
 * TableAdd: new PSI PID
 *****************************************************************************/
static void TableAdd( sout_stream_t *p_stream, char *psz_name,
                      config_chain_t *p_cfg )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_t  *p_table;
    ts_table_t *p_packetizer;

    p_table = malloc( sizeof( sout_stream_id_t ) );
    memset( p_table, 0, sizeof( sout_stream_id_t ) );
    p_table->p_fifo = block_FifoNew();
    p_table->b_deleted = false;

    p_packetizer = (ts_table_t *)vlc_object_create( p_stream,
                                                    sizeof(ts_table_t) );
    p_table->p_packetizer = (ts_packetizer_t *)p_packetizer;
    vlc_object_attach( p_packetizer, p_stream );

    p_packetizer->p_cfg = p_cfg;
    p_packetizer->psz_name = psz_name;
    p_packetizer->p_ts_stream = &p_sys->ts;
    p_packetizer->p_module =
        module_need( p_packetizer, "ts packetizer", psz_name, true );
    if ( p_packetizer->p_module == NULL )
    {
        free( psz_name );
        free( p_cfg );
        vlc_object_release( p_packetizer );

        block_FifoRelease( p_table->p_fifo );
        free( p_table );
        return;
    }

    p_packetizer->i_pid = PIDAllocate( p_stream, p_table, -1 );

    vlc_mutex_lock( &p_sys->stream_lock );
    TAB_APPEND( p_sys->ts.i_nb_tables, p_sys->ts.pp_tables, p_table );
    vlc_mutex_unlock( &p_sys->stream_lock );

    msg_Dbg( p_stream, "adding PID %u (%s)", p_packetizer->i_pid,
             psz_name );
}

/*****************************************************************************
 * TableDel: remove PSI PID / called with stream_lock
 *****************************************************************************/
static void TableDel( sout_stream_t *p_stream, sout_stream_id_t *p_table )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    ts_table_t *p_packetizer = (ts_table_t *)p_table->p_packetizer;

    msg_Dbg( p_stream, "removing PID %u (%s)",
             p_packetizer->i_pid, p_packetizer->psz_name );

    TAB_REMOVE( p_sys->ts.i_nb_tables, p_sys->ts.pp_tables, p_table );

    module_unneed( p_packetizer, p_packetizer->p_module );
    free( p_packetizer->p_cfg );
    free( p_packetizer->psz_name );
    vlc_object_release( p_packetizer );

    block_FifoRelease( p_table->p_fifo );
    free( p_table );
}

/*****************************************************************************
 * TableSend: check tables for new buffers / called with stream_lock
 *****************************************************************************/
static void TableSend( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    int i;

    for ( i = 0; i < p_sys->ts.i_nb_tables; i++ )
    {
        sout_stream_id_t *p_table = p_sys->ts.pp_tables[i];
        ts_table_t *p_packetizer = (ts_table_t *)p_table->p_packetizer;
        block_t *p_out = p_packetizer->pf_send( p_packetizer,
                                                p_sys->i_last_muxing );

        if ( p_out != NULL )
        {
            if ( p_out->i_dts - p_out->i_delay
                  < p_sys->i_last_muxing + p_sys->ts.params.i_max_prepare )
                msg_Warn( p_stream, "received late buffer PID %u (%"PRId64")",
                          p_packetizer->i_pid,
                          p_sys->i_last_muxing
                           + p_sys->ts.params.i_max_prepare
                           - p_out->i_dts + p_out->i_delay );
            block_FifoPut( p_table->p_fifo, p_out );
        }
    }
}


/*
 * Muxing
 */

/*****************************************************************************
 * MuxValidateParams: calculate new TS parameters from muxmode/muxrate / called
 * with stream_lock
 *****************************************************************************/
static void MuxValidateParams( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    if ( p_sys->i_muxmode == MODE_VBR )
        p_sys->ts.params.i_packet_interval = VBR_DEFAULT_INTERVAL * 1000;
    else
        p_sys->ts.params.i_packet_interval = p_sys->i_granularity_size
                                              / p_sys->i_muxrate;

    p_sys->ts.params.i_max_prepare = p_sys->ts.params.i_packet_interval
                                      * MAX_PREPARE_PKT;
    if ( p_sys->ts.params.i_max_prepare > MAX_PREPARE_TIME * 1000 )
        p_sys->ts.params.i_max_prepare = MAX_PREPARE_TIME * 1000;
}

/*****************************************************************************
 * MuxCheckMode: automatically choose appropriate operating mode / called with
 * stream_lock
 *****************************************************************************/
static void MuxCheckMode( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    bool b_mode_vbr = false;
    unsigned int i_total_bitrate = 0;
    int i;

    for ( i = 0; i < p_sys->ts.i_nb_tables; i++ )
    {
        sout_stream_id_t *p_table = p_sys->ts.pp_tables[i];
        ts_table_t *p_packetizer = (ts_table_t *)p_table->p_packetizer;
        i_total_bitrate += p_packetizer->i_total_bitrate;
    }

    for ( i = p_sys->ts.i_nb_inputs - 1; i >= 0; i-- )
    {
        sout_stream_id_t *p_input = p_sys->ts.pp_inputs[i];
        ts_input_t *p_packetizer = (ts_input_t *)p_input->p_packetizer;
        if ( !p_packetizer->i_total_bitrate )
            b_mode_vbr = true;
        else
            i_total_bitrate += p_packetizer->i_total_bitrate;
        if ( p_packetizer->i_pcr_period )
            i_total_bitrate += ((TS_HEADER_SIZE_PCR - TS_HEADER_SIZE)
                 * INT64_C(8000000) + p_packetizer->i_pcr_period - 1)
                / p_packetizer->i_pcr_period;
    }

    i_total_bitrate += p_sys->i_padding_bitrate;

    if ( p_sys->b_auto_muxmode )
        p_sys->i_muxmode = b_mode_vbr ? MODE_VBR : MODE_CAPPED;
    else if ( b_mode_vbr && p_sys->i_muxmode != MODE_VBR )
        msg_Warn( p_stream, "%s mode requested but only vbr is possible",
                  p_sys->i_muxmode == MODE_CAPPED ? "capped-vbr" : "cbr" );

    if ( p_sys->b_auto_muxrate )
    {
        p_sys->i_muxrate = (i_total_bitrate + 7) / 8;
        if ( !p_sys->i_muxrate )
            p_sys->i_muxrate = 1; /* shouldn't happen */
    }
    else if ( p_sys->i_muxmode != MODE_VBR
               && p_sys->i_muxrate < (i_total_bitrate + 7) / 8 )
        msg_Warn( p_stream, "%u bitrate requested is too low (should be %u)",
                  p_sys->i_muxrate * 8, i_total_bitrate );

    MuxValidateParams( p_stream );

    if ( p_sys->b_auto_muxrate || p_sys->b_auto_muxmode )
    {
        if ( p_sys->i_muxmode == MODE_VBR )
        {
            msg_Dbg( p_stream, "now operating in vbr mode" );
        }
        else
        {
            msg_Dbg( p_stream, "now operating in %s mode at bitrate %u, packet interval %"PRId64" us",
                     ppsz_muxmode[p_sys->i_muxmode], p_sys->i_muxrate * 8,
                     p_sys->ts.params.i_packet_interval );
            p_sys->i_last_muxing_remainder = 0;
        }
    }

    p_sys->i_last_stream_version = p_sys->ts.i_stream_version;
}

/*****************************************************************************
 * MuxCheckAsync: calculate latest muxing timestamp of every stream and
 * return the earliest / called with stream_lock
 *****************************************************************************/
static mtime_t MuxCheckAsync( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    mtime_t i_max_muxing = -1;
    int i;

    /* Do not check the tables because they have plenty of time. */
    for ( i = p_sys->ts.i_nb_inputs - 1; i >= 0; i-- )
    {
        sout_stream_id_t *p_queue = p_sys->ts.pp_inputs[i];
        block_t *p_block;

        if ( p_queue->b_deleted )
            continue;

        vlc_mutex_lock( &p_queue->p_fifo->lock );
        p_block = p_queue->p_fifo->p_first;
        while ( p_block != NULL && p_block->p_next != NULL )
            p_block = p_block->p_next;
        vlc_mutex_unlock( &p_queue->p_fifo->lock );

        if ( p_block == NULL )
            return -1; /* wait for at least one packet in every stream */
        if ( i_max_muxing == -1
              || p_block->i_dts - p_block->i_delay < i_max_muxing )
            i_max_muxing = p_block->i_dts - p_block->i_delay;
    }

    return i_max_muxing;
}

#define FOREACH_QUEUE( CMD )                                                \
    int i;                                                                  \
    /* Tables are in ascending order so that we send PAT before PMT;        \
     * inputs are in descending order so that we can delete an input without\
     * changing the iterator. */                                            \
                                                                            \
    for ( i = 0; i < p_sys->ts.i_nb_tables; i++ )                           \
    {                                                                       \
        sout_stream_id_t *p_queue = p_sys->ts.pp_tables[i];                 \
        block_t *p_block;                                                   \
                                                                            \
        vlc_mutex_lock( &p_queue->p_fifo->lock );                           \
        p_block = p_queue->p_fifo->p_first;                                 \
        vlc_mutex_unlock( &p_queue->p_fifo->lock );                         \
                                                                            \
        CMD                                                                 \
    }                                                                       \
                                                                            \
    for ( i = p_sys->ts.i_nb_inputs - 1; i >= 0; i-- )                      \
    {                                                                       \
        sout_stream_id_t *p_queue = p_sys->ts.pp_inputs[i];                 \
        block_t *p_block;                                                   \
                                                                            \
        vlc_mutex_lock( &p_queue->p_fifo->lock );                           \
        p_block = p_queue->p_fifo->p_first;                                 \
        vlc_mutex_unlock( &p_queue->p_fifo->lock );                         \
                                                                            \
        CMD                                                                 \
    }

/*****************************************************************************
 * MuxShow: return muxing date of the next available TS / called with
 * stream_lock
 *****************************************************************************/
static mtime_t MuxShow( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    mtime_t i_min_muxing = -1;

#define CMD                                                                 \
    if ( p_block != NULL )                                                  \
    {                                                                       \
        mtime_t i_muxing = __MAX(p_block->i_dts - p_block->i_delay,         \
                                 p_queue->i_min_muxing);                    \
        if ( i_min_muxing == -1 || i_muxing < i_min_muxing )                \
            i_min_muxing = i_muxing;                                        \
    }
    FOREACH_QUEUE( CMD )
#undef CMD

    return i_min_muxing;
}

/*****************************************************************************
 * MuxGet: return next queue to be muxed / called with stream_lock
 *****************************************************************************/
static sout_stream_id_t *MuxGet( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    mtime_t i_min_muxing = -1;
    mtime_t i_emergency_muxing = p_sys->i_last_muxing
                                  + p_sys->ts.params.i_packet_interval;
    unsigned int i_priority = TSPACK_PRIORITY_NONE;
    sout_stream_id_t *p_next_queue = NULL;

#define CMD                                                                 \
    if ( p_block != NULL )                                                  \
    {                                                                       \
        mtime_t i_muxing = __MAX(p_block->i_dts - p_block->i_delay,         \
                                 p_queue->i_min_muxing);                    \
        if ( (i_min_muxing == -1 || i_muxing < i_min_muxing                 \
               || p_queue->p_packetizer->i_priority > i_priority)           \
              && i_muxing <= p_sys->i_last_muxing )                         \
        {                                                                   \
            i_min_muxing = i_muxing;                                        \
            i_priority = p_queue->p_packetizer->i_priority;                 \
            p_next_queue = p_queue;                                         \
        }                                                                   \
                                                                            \
        if ( p_block->i_dts <= i_emergency_muxing )                         \
            return p_queue;                                                 \
    }                                                                       \
    else if ( p_queue->b_deleted )                                          \
        InputDelete( p_stream, p_queue );
    FOREACH_QUEUE( CMD )
#undef CMD

    return p_next_queue;
}

/*****************************************************************************
 * MuxFixQueues: update min muxing timestamp of each queue wrt. to the peak
 * bitrate, to be T-STD-compliant / called with stream_lock
 *****************************************************************************/
static void MuxFixQueues( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

#define CMD                                                                 \
    if ( p_queue->p_packetizer->i_peak_bitrate && p_queue->i_muxed_size )   \
    {                                                                       \
        p_queue->i_min_muxing = p_sys->i_last_muxing                        \
            + p_queue->i_muxed_size * INT64_C(8000000)                         \
            / p_queue->p_packetizer->i_peak_bitrate;                        \
        p_queue->i_muxed_size = 0;                                          \
    }
    FOREACH_QUEUE( CMD )
#undef CMD
}

#undef FOREACH_QUEUE

/*****************************************************************************
 * MuxShowMuxing: return the date of the next <granularity> ensemble / called
 * with stream_lock
 *****************************************************************************/
static mtime_t MuxShowMuxing( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    if ( p_sys->i_muxmode != MODE_VBR && p_sys->i_last_muxing != -1 )
        return p_sys->i_last_muxing
                + (p_sys->i_last_muxing_remainder + p_sys->i_granularity_size)
                    / p_sys->i_muxrate;
    return MuxShow( p_stream );
}

/*****************************************************************************
 * MuxIncrementMuxing: set the date of the next muxing buffer / called with
 * stream_lock
 *****************************************************************************/
static void MuxIncrementMuxing( sout_stream_t *p_stream, mtime_t i_next_muxing )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    /* i_next_muxing is used as optimisation in cases where it is possible
     * (and expensive). */
    if ( p_sys->i_muxmode == MODE_VBR || p_sys->i_last_muxing == -1 )
        p_sys->i_last_muxing = i_next_muxing;
    else
    {
        p_sys->i_last_muxing += (p_sys->i_last_muxing_remainder
                                  + p_sys->i_granularity_size)
                                 / p_sys->i_muxrate;
        p_sys->i_last_muxing_remainder = (p_sys->i_last_muxing_remainder
                                           + p_sys->i_granularity_size)
                                          % p_sys->i_muxrate;
    }
}

/*****************************************************************************
 * MuxClearRAP: clear the RAPs which are in the past / called with stream_lock
 *****************************************************************************/
static void MuxClearRAP( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    while ( p_sys->ts.i_nb_raps && *p_sys->ts.pi_raps <= p_sys->i_last_muxing )
    {
        memmove( p_sys->ts.pi_raps, p_sys->ts.pi_raps + 1,
                 --p_sys->ts.i_nb_raps * sizeof(mtime_t) );
        p_sys->ts.pi_raps = realloc( p_sys->ts.pi_raps,
                                     p_sys->ts.i_nb_raps * sizeof(mtime_t) );
    }
}

/*****************************************************************************
 * MuxCheckLate: check for late packets and return first one / called with
 * stream_lock
 *****************************************************************************/
static block_t *MuxCheckLate( sout_stream_t *p_stream,
                              sout_stream_id_t **pp_queue )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    block_t *p_block;

next_packet:
    *pp_queue = MuxGet( p_stream );
    if ( *pp_queue == NULL )
        return NULL;

    p_block = block_FifoGet( (*pp_queue)->p_fifo );

    if ( p_block->i_dts < p_sys->i_last_muxing )
    {
        if ( p_block->i_dts < p_sys->i_last_muxing - MAX_DELAYING * 1000
              || p_sys->b_drop )
        {
            msg_Warn( p_stream, "dropping late packet pid=%u priority=%u lateness=%"PRId64" delay=%"PRId64,
                      (*pp_queue)->p_packetizer->i_pid,
                      (*pp_queue)->p_packetizer->i_priority,
                      p_sys->i_last_muxing - p_block->i_dts,
                      p_block->i_delay );
            block_Release( p_block );
            goto next_packet;
        }
        else if ( p_sys->b_burst )
        {
            msg_Warn( p_stream, "bursting late packet pid=%u priority=%u lateness=%"PRId64" delay=%"PRId64,
                      (*pp_queue)->p_packetizer->i_pid,
                      (*pp_queue)->p_packetizer->i_priority,
                      p_sys->i_last_muxing - p_block->i_dts,
                      p_block->i_delay );
            p_sys->i_last_muxing = p_block->i_dts;
            p_sys->i_last_muxing_remainder = 0;
        }
        else
            msg_Warn( p_stream, "delaying late packet pid=%u priority=%u lateness=%"PRId64" delay=%"PRId64,
                      (*pp_queue)->p_packetizer->i_pid,
                      (*pp_queue)->p_packetizer->i_priority,
                      p_sys->i_last_muxing - p_block->i_dts,
                      p_block->i_delay );
    }

    return p_block;
}

/*****************************************************************************
 * MuxCheckIncrement: check how much current buffer could bear being delayed /
 * called with stream_lock
 *****************************************************************************/
static mtime_t MuxCheckIncrement( sout_stream_t *p_stream, block_t *p_blocks )
{
    VLC_UNUSED(p_stream);
    mtime_t i_max_muxing = -1;

    while ( p_blocks != NULL )
    {
        if ( i_max_muxing == -1 || p_blocks->i_dts < i_max_muxing )
            i_max_muxing = p_blocks->i_dts;
        p_blocks = p_blocks->p_next;
    }

    return i_max_muxing;
}

/*****************************************************************************
 * Mux: prepare <granularity> packets to be output / called with stream_lock
 *****************************************************************************/
static block_t *Mux( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_t *p_queue;
    int i_nb_packets = p_sys->i_granularity;
    mtime_t i_last_packet_muxing = p_sys->i_last_muxing;
    block_t *p_blocks = NULL;
    block_t **pp_last = &p_blocks;
    block_t *p_block = NULL;

    if ( p_sys->i_tmp_nb_packets )
    {
        i_nb_packets = p_sys->i_tmp_nb_packets;
        p_blocks = p_sys->p_tmp_blocks;
        pp_last = p_sys->pp_tmp_last;

        p_sys->i_tmp_nb_packets = 0;
        p_sys->p_tmp_blocks = NULL;
        p_sys->pp_tmp_last = NULL;
    }

    if ( p_sys->i_muxmode == MODE_VBR )
    {
        /* Small hack to avoid calling Mux() too often. */
        mtime_t i_max_muxing = MuxCheckIncrement( p_stream, p_blocks );
        if ( i_max_muxing == -1
              || i_max_muxing > p_sys->i_last_muxing
                                 + p_sys->ts.params.i_packet_interval )
            p_sys->i_last_muxing += p_sys->ts.params.i_packet_interval;
        else
            p_sys->i_last_muxing = i_max_muxing;
    }

    p_block = MuxCheckLate( p_stream, &p_queue );

    do
    {
        if ( p_queue == NULL )
        {
            if ( p_sys->i_muxmode != MODE_CBR )
            {
                mtime_t i_max_muxing = MuxCheckIncrement( p_stream, p_blocks );
                if ( i_max_muxing == -1
                      || i_max_muxing >= MuxShowMuxing( p_stream ) )
                {
                    p_sys->i_tmp_nb_packets = i_nb_packets;
                    p_sys->p_tmp_blocks = p_blocks;
                    p_sys->pp_tmp_last = pp_last;
                    return NULL;
                }
            }

            *pp_last = block_New( p_stream, TS_SIZE );
            ts_pad( (*pp_last)->p_buffer );
        }
        else
        {
            uint8_t *p_ts = p_block->p_buffer;
            unsigned int i_payload_size = TS_SIZE - (ts_payload(p_ts) - p_ts);
            *pp_last = p_block;
            p_queue->i_muxed_size += i_payload_size;
            i_last_packet_muxing = p_block->i_dts - p_block->i_delay;
        }
        pp_last = &(*pp_last)->p_next;

        i_nb_packets--;
        if ( i_nb_packets )
        {
            p_queue = MuxGet( p_stream );

            if ( p_queue != NULL )
                p_block = block_FifoGet( p_queue->p_fifo );
        }
    }
    while ( i_nb_packets );
    *pp_last = NULL;

    if ( p_sys->i_muxmode == MODE_VBR )
        /* Fix the small hack. */
        p_sys->i_last_muxing = i_last_packet_muxing;

    MuxClearRAP( p_stream );
    MuxFixQueues( p_stream );

    return p_blocks;
}

/*****************************************************************************
 * MuxGather: packetize <granularity> packets for the output plug-in
 *****************************************************************************/
static block_t *MuxGather( sout_stream_t *p_stream, block_t *p_blocks,
                           mtime_t i_pcr_date )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    block_t *p_block = p_blocks;

    /* First write the PCRs. */
    while ( p_block != NULL )
    {
        if ( ts_has_adaptation( p_block->p_buffer )
              && ts_get_adaptation( p_block->p_buffer )
              && tsaf_has_pcr( p_block->p_buffer ) )
        {
            tsaf_set_pcr( p_block->p_buffer, i_pcr_date / 300 );
            tsaf_set_pcrext( p_block->p_buffer, i_pcr_date % 300 );
        }

        p_block = p_block->p_next;
    }

    if ( p_sys->b_rtp )
    {
        block_t *p_rtp = block_New( p_stream, RTP_HEADER_SIZE );
        rtp_set_hdr( p_rtp->p_buffer );
        rtp_set_type( p_rtp->p_buffer, RTP_TYPE_TS );
        rtp_set_cc( p_rtp->p_buffer, p_sys->i_rtp_cc++ );
        rtp_set_timestamp( p_rtp->p_buffer, i_pcr_date / 300 );
        rtp_set_ssrc( p_rtp->p_buffer, p_sys->pi_ssrc );

        p_rtp->p_next = p_blocks;
        p_blocks = p_rtp;
    }

    return block_ChainGather( p_blocks );
}

/*****************************************************************************
 * MuxAsync: run in asynchronous mode (eg. reading from and writing to file)
 *****************************************************************************/
static void MuxAsync( sout_stream_t *p_stream, bool b_flush )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    for ( ; ; )
    {
        mtime_t i_next_muxing;
        block_t *p_blocks;

        vlc_mutex_lock( &p_sys->stream_lock );
        if ( p_sys->i_last_stream_version != p_sys->ts.i_stream_version )
            MuxCheckMode( p_stream );

        i_next_muxing = MuxShowMuxing( p_stream );
        if ( i_next_muxing == -1 )
        {
            vlc_mutex_unlock( &p_sys->stream_lock );
            return;
        }
        else if ( p_sys->i_last_muxing == -1 )
            /* Allow for an early start. */
            i_next_muxing -= 2 * p_sys->ts.params.i_max_prepare;

        if ( !b_flush )
        {
            mtime_t i_max_muxing = MuxCheckAsync( p_stream );
            if ( i_max_muxing == -1
                  || i_max_muxing < i_next_muxing
                                     + p_sys->ts.params.i_max_prepare
                                     + p_sys->ts.params.i_packet_interval
                                     + p_sys->i_async_delay )
            {
                vlc_mutex_unlock( &p_sys->stream_lock );
                return;
            }
        }
        else
        {
            if ( MuxShow( p_stream ) == -1 )
            {
                vlc_mutex_unlock( &p_sys->stream_lock );
                return;
            }
        }

        MuxIncrementMuxing( p_stream, i_next_muxing );
        TableSend( p_stream );
        p_blocks = Mux( p_stream );
        vlc_mutex_unlock( &p_sys->stream_lock );

        if ( p_blocks != NULL )
        {
            mtime_t i_pcr_clock = p_sys->i_last_muxing * 27;
            /* We need that for sub-microsecond precision PCR (spec says
             * 500 ns). */
            if ( p_sys->i_muxrate )
                i_pcr_clock += p_sys->i_last_muxing_remainder * 27
                                / p_sys->i_muxrate;
            p_stream->p_next->pf_send( p_stream->p_next, p_sys->id,
                MuxGather( p_stream, p_blocks, i_pcr_clock ) );
        }
    }
}

/*****************************************************************************
 * MuxThread: run in synchronous mode
 *****************************************************************************/
static void *MuxThread( vlc_object_t *p_this )
{
    sout_stream_sys_t *p_sys = (sout_stream_sys_t *)p_this;
    sout_stream_t *p_stream = p_sys->p_stream;

    msg_Dbg( p_stream, "starting TS mux thread with %s conformance",
             ppsz_conformance[p_sys->ts.params.i_conformance] );

    while( vlc_object_alive( p_sys ) )
    {
        bool b_init = (p_sys->i_last_muxing == -1);
        mtime_t i_current_date, i_next_muxing;

        vlc_mutex_lock( &p_sys->stream_lock );
        if ( p_sys->i_last_stream_version != p_sys->ts.i_stream_version )
            MuxCheckMode( p_stream );

        i_next_muxing = MuxShowMuxing( p_stream );
        i_current_date = mdate();

        if ( p_sys->i_last_muxing == -1 && i_next_muxing != -1 )
            /* Allow for an early start. */
            i_next_muxing -= 2 * p_sys->ts.params.i_max_prepare;

        int canc = vlc_savecancel();
        if ( i_next_muxing == -1 )
        {
            vlc_cond_wait( &p_sys->stream_wait, &p_sys->stream_lock );
            vlc_mutex_unlock( &p_sys->stream_lock );
        }
        else if ( i_next_muxing > i_current_date
                   + p_sys->ts.params.i_max_prepare )
        {
            vlc_cond_timedwait( &p_sys->stream_wait, &p_sys->stream_lock,
                             i_next_muxing - p_sys->ts.params.i_max_prepare );
            vlc_mutex_unlock( &p_sys->stream_lock );
        }
        else
        {
            block_t *p_blocks;

            MuxIncrementMuxing( p_stream, i_next_muxing );
            TableSend( p_stream );
            if ( b_init )
            {
                /* The tables are prepended so we must start earlier. */
                p_sys->i_last_muxing = -1;
                i_next_muxing = MuxShowMuxing( p_stream );
                MuxIncrementMuxing( p_stream, i_next_muxing );
            }
            p_blocks = Mux( p_stream );
            vlc_mutex_unlock( &p_sys->stream_lock );

            if ( p_blocks != NULL )
            {
                if ( i_current_date > p_sys->i_last_muxing + 5000 )
                    msg_Warn( p_stream, "output late buffer (%"PRId64")",
                              i_current_date - p_sys->i_last_muxing );
                else
                    mwait( p_sys->i_last_muxing );

                /* FIXME: we are not precise enough for the PCR, but mdate()
                 * only returns microsecond precision. */
                p_stream->p_next->pf_send( p_stream->p_next, p_sys->id,
                            MuxGather( p_stream, p_blocks, mdate() * 27 ) );
            }
        }
        vlc_restorecancel(canc);
    }

    return NULL;
}
