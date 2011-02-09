/*****************************************************************************
 * ts_table.h: functions and structures for PSI
 *****************************************************************************
 * Copyright (C) 2009-2011 VideoLAN
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
 * The main difference between a TS input and a TS table is that
 * TS tables have access to the stream structure. That is why they are
 * always entered with the mux lock. --Meuuh
 */

#define DEFAULT_INTERVAL        30 /* ms */
#define DEFAULT_TS_INTERVAL     20 /* ms */
#define DEFAULT_MIN_PERIOD      200 /* ms */

/*****************************************************************************
 * TS stream structure definition
 *****************************************************************************/
typedef struct ts_stream_t ts_stream_t;

struct sout_stream_id_t
{
    block_fifo_t *p_fifo;
    ts_packetizer_t *p_packetizer;

    bool b_deleted;
    /* T-STD stuff */
    mtime_t i_min_muxing;
    unsigned int i_muxed_size;
};

struct ts_stream_t
{
    int i_stream_version;
    mtime_t *pi_raps; /* next random access points */
    int i_nb_raps;

    sout_stream_id_t **pp_inputs;
    int i_nb_inputs;

    sout_stream_id_t **pp_tables;
    int i_nb_tables;

    ts_parameters_t params;

    uint16_t i_tsid;
    uint16_t i_nid;
};

/*****************************************************************************
 * TS table module definition
 *****************************************************************************/
typedef struct ts_table_t ts_table_t;

struct ts_table_t
{
    VLC_COMMON_MEMBERS

    TSPACK_COMMON_MEMBERS

    char *psz_name;
    ts_stream_t *p_ts_stream;
    block_t *(*pf_send)( ts_table_t *, mtime_t );

    /* table generation */
    int i_last_stream_version;
    block_t *p_last_table;

    /* table repetition */
    mtime_t i_interval, i_ts_interval;
    mtime_t i_period, i_offset, i_rap_advance, i_min_period, i_max_period;
    mtime_t i_last_muxing;
    uint8_t i_cc;

    /* PAT information */
    bool b_defines_program;
    uint16_t i_program;

    /* PMT information */
    uint8_t *p_ecm_descriptor;
    int i_ecm_descriptor;
};

#define INTERVAL_TEXT N_("Interval between sections")
#define INTERVAL_LONGTEXT N_("Defines the interval between sections, in milliseconds (DVB >= 25 ms, default 30 ms).")
#define TS_INTERVAL_TEXT N_("Interval between TS")
#define TS_INTERVAL_LONGTEXT N_("Defines the interval between two TS packets of the same section, in milliseconds (MPEG T-STD model: max 1 Mbi/s for system, beware of the extra bitrate peak in CBR mode).")
#define PERIOD_TEXT N_("Table period")
#define PERIOD_LONGTEXT N_("Use a periodic scheme to output this table (default).")
#define OFFSET_TEXT N_("Offset")
#define OFFSET_LONGTEXT N_("In periodic mode, defines the offset time (in ms) at which the first packet is output.")
#define RAP_TEXT N_("RAP advance")
#define RAP_LONGTEXT N_("Places the table approximately x millisecond before a random access point (default -1: disabled).")
#define RAP_MIN_PERIOD_TEXT N_("Min table period")
#define RAP_MIN_PERIOD_LONGTEXT N_("Minimum table period in RAP mode.")
#define RAP_MAX_PERIOD_TEXT N_("Max table period")
#define RAP_MAX_PERIOD_LONGTEXT N_("Maximum table period in RAP mode.")

#define TS_TABLE_COMMON( pid, period, max_period, offset )                  \
    TS_PACKETIZER_COMMON( pid )                                             \
    add_integer( SOUT_CFG_PREFIX "interval", DEFAULT_INTERVAL,              \
                 INTERVAL_TEXT, INTERVAL_LONGTEXT, false );                 \
    add_integer( SOUT_CFG_PREFIX "ts-interval", DEFAULT_TS_INTERVAL,        \
                 TS_INTERVAL_TEXT, TS_INTERVAL_LONGTEXT, false );           \
    add_integer( SOUT_CFG_PREFIX "period", period,                          \
                 PERIOD_TEXT, PERIOD_LONGTEXT, false );                     \
    add_integer( SOUT_CFG_PREFIX "offset", offset,                          \
                 OFFSET_TEXT, OFFSET_LONGTEXT, false );                     \
    add_integer( SOUT_CFG_PREFIX "rap-advance", -1, RAP_TEXT,               \
                 RAP_LONGTEXT, false );                                     \
    add_integer( SOUT_CFG_PREFIX "rap-min-period", DEFAULT_MIN_PERIOD,      \
                 RAP_MIN_PERIOD_TEXT, RAP_MIN_PERIOD_LONGTEXT, false );     \
    add_integer( SOUT_CFG_PREFIX "rap-max-period", max_period,              \
                 RAP_MAX_PERIOD_TEXT, RAP_MAX_PERIOD_LONGTEXT, false );

#define TS_TABLE_COMMON_OPTIONS TS_PACKETIZER_COMMON_OPTIONS, "interval", "ts-interval", "period", "offset", "rap-advance", "rap-min-period", "rap-max-period"

/*****************************************************************************
 * tstable_CommonOptions: called on table init
 *****************************************************************************/
static inline void tstable_CommonOptions( ts_table_t *p_table )
{
    vlc_value_t val;

    tspack_CommonOptions( (ts_packetizer_t *)p_table );

    var_Get( p_table, SOUT_CFG_PREFIX "interval", &val );
    p_table->i_interval = val.i_int * 1000;

    var_Get( p_table, SOUT_CFG_PREFIX "ts-interval", &val );
    p_table->i_ts_interval = val.i_int * 1000;

    var_Get( p_table, SOUT_CFG_PREFIX "period", &val );
    p_table->i_period = val.i_int * 1000;

    var_Get( p_table, SOUT_CFG_PREFIX "offset", &val );
    p_table->i_offset = val.i_int * 1000;

    var_Get( p_table, SOUT_CFG_PREFIX "rap-advance", &val );
    p_table->i_rap_advance = val.i_int == -1 ? -1 : val.i_int * 1000;

    var_Get( p_table, SOUT_CFG_PREFIX "rap-min-period", &val );
    p_table->i_min_period = val.i_int * 1000;

    var_Get( p_table, SOUT_CFG_PREFIX "rap-max-period", &val );
    p_table->i_max_period = val.i_int * 1000;
}

/*****************************************************************************
 * tstable_Close: called on table exit
 *****************************************************************************/
static inline void tstable_Close( ts_table_t *p_table )
{
    block_ChainRelease( p_table->p_last_table );
}

/*****************************************************************************
 * tstable_NbTS: return the number of TS packets for a PSI section
 *****************************************************************************/
static inline int tstable_NbTS( ts_table_t *p_table, const block_t *p_section )
{
    VLC_UNUSED(p_table);
    int i_ts_payload = TS_SIZE - TS_HEADER_SIZE;
    return (p_section->i_buffer + i_ts_payload - 1) / i_ts_payload;
}

/*****************************************************************************
 * tstable_BuildTS: build a chain of TS packets for a PSI section
 *****************************************************************************/
static inline block_t *tstable_BuildTS( ts_table_t *p_table,
                                        const block_t *p_section )
{
    int i_nb_ts, i;
    block_t *p_first = NULL;
    block_t **pp_last = &p_first;
    uint8_t *p_buffer = p_section->p_buffer;
    int i_buffer = p_section->i_buffer;

    /* The difference with the one from ts_input.h is that we do not
     * deal with duration and muxing timestamp here. */

    i_nb_ts = tstable_NbTS( p_table, p_section );

    for ( i = i_nb_ts - 1; i >= 0; i-- )
    {
        int i_ts_payload = TS_SIZE - TS_HEADER_SIZE;
        uint8_t *p_ts_payload;

        *pp_last = block_New( p_table, TS_SIZE );
        (*pp_last)->i_flags = p_section->i_flags;
        ts_init( (*pp_last)->p_buffer );
        ts_set_pid( (*pp_last)->p_buffer, p_table->i_pid );
        ts_set_cc( (*pp_last)->p_buffer, ++p_table->i_cc );
        if ( i == i_nb_ts - 1 )
            ts_set_unitstart( (*pp_last)->p_buffer );

        ts_set_payload( (*pp_last)->p_buffer );
        p_ts_payload = ts_payload( (*pp_last)->p_buffer );
        vlc_memcpy( p_ts_payload, p_buffer, __MIN(i_ts_payload, i_buffer) );
        if ( i_buffer < i_ts_payload )
            vlc_memset( &p_ts_payload[i_buffer], 0xff, i_ts_payload - i_buffer );

        p_buffer += i_ts_payload;
        i_buffer -= i_ts_payload;
        pp_last = &(*pp_last)->p_next;
    }

    return p_first;
}

/*****************************************************************************
 * tstable_Force: force output of the table at the next send()
 *****************************************************************************/
static inline void tstable_Force( ts_table_t *p_table )
{
    p_table->i_last_muxing = -1;
}

/*****************************************************************************
 * tstable_NextRAP: return next relevant RAP
 *****************************************************************************/
static inline mtime_t tstable_NextRAP( ts_table_t *p_table,
                                       mtime_t i_last_muxing )
{
    VLC_UNUSED(i_last_muxing);
    ts_stream_t *p_ts_stream = p_table->p_ts_stream;
    int i;

    for ( i = 0; i < p_ts_stream->i_nb_raps; i++ )
    {
        mtime_t i_next_rap = p_ts_stream->pi_raps[i] - p_table->i_rap_advance;
        if ( i_next_rap > p_table->i_last_muxing + p_table->i_min_period )
            return i_next_rap;
    }

    return -1;
}

/*****************************************************************************
 * tstable_Duration: return how much time needed to send all sections of table
 *****************************************************************************/
static inline mtime_t tstable_Duration( ts_table_t *p_table )
{
    mtime_t i_duration = -p_table->i_interval;
    block_t *p_section = p_table->p_last_table;

    while ( p_section != NULL )
    {
        i_duration += (tstable_NbTS( p_table, p_section ) - 1)
                       * p_table->i_ts_interval
                       + p_table->i_interval;
        p_section = p_section->p_next;
    }

    if ( i_duration < 0 ) return 0; /* shouldn't happen */
    return i_duration;
}

/*****************************************************************************
 * tstable_NextMuxing: return muxing timestamp of the next packet to go
 *****************************************************************************/
static inline mtime_t tstable_NextMuxing( ts_table_t *p_table,
                                          mtime_t i_last_muxing )
{
    mtime_t i_prepare = p_table->p_ts_stream->params.i_max_prepare
                         + p_table->p_ts_stream->params.i_packet_interval;
    mtime_t i_next_muxing;

    if ( p_table->i_last_muxing == -1 ) /* forced */
        return i_last_muxing + i_prepare;

    if ( p_table->i_rap_advance == -1 )
    {
        i_next_muxing = p_table->i_last_muxing + p_table->i_period;

        /* Offset is there to avoid that after some error affecting all
         * tables with the same configuration, all tables get sent at the
         * same time. */
        if ( i_next_muxing < i_last_muxing )
        {
            msg_Warn( p_table, "exceeding period by %"PRId64" us",
                i_last_muxing + i_prepare + p_table->i_offset - i_next_muxing );
            return i_last_muxing + i_prepare + p_table->i_offset;
        }
    }
    else
    {
        if ( i_last_muxing + i_prepare
              > p_table->i_last_muxing + p_table->i_max_period )
            return i_last_muxing + i_prepare;

        i_next_muxing = tstable_NextRAP( p_table, i_last_muxing );
        if ( i_next_muxing == -1 ) return -1;

        i_next_muxing -= tstable_Duration( p_table );

        if ( i_next_muxing < i_last_muxing )
            return i_last_muxing + i_prepare;
    }

    return i_next_muxing;
}

/*****************************************************************************
 * tstable_Send: check if a section or part of a section needs to be sent
 *****************************************************************************/
static inline block_t *tstable_Send( ts_table_t *p_table,
                                     mtime_t i_last_muxing )
{
    mtime_t i_next_muxing = tstable_NextMuxing( p_table, i_last_muxing );
    mtime_t i_packet_interval = p_table->p_ts_stream->params.i_packet_interval;
    block_t *p_ts = NULL;
    block_t **pp_last_ts = &p_ts;
    block_t *p_section = p_table->p_last_table;

    if ( i_next_muxing == -1
          || i_next_muxing > i_last_muxing
                              + p_table->p_ts_stream->params.i_max_prepare
                              + 3 * i_packet_interval )
        return NULL;

    while ( p_section != NULL )
    {
        *pp_last_ts = tstable_BuildTS( p_table, p_section );

        while ( *pp_last_ts != NULL )
        {
            (*pp_last_ts)->i_dts = i_next_muxing + i_packet_interval;
            (*pp_last_ts)->i_delay = i_packet_interval * 2;
            pp_last_ts = &(*pp_last_ts)->p_next;
            i_next_muxing += p_table->i_ts_interval;
        }
        p_section = p_section->p_next;
        i_next_muxing += p_table->i_interval - p_table->i_ts_interval;
    }

    if ( p_table->i_last_muxing == -1 && p_table->i_rap_advance == -1
          && p_table->i_offset )
        /* Try to take into account the offset even though we are forced. */
        p_table->i_last_muxing = i_next_muxing
            - (p_table->i_period - p_table->i_offset);
    else
        p_table->i_last_muxing = i_next_muxing;

    return p_ts;
}

/*****************************************************************************
 * tstable_UpdateTotalBitrate: self-explanatory
 *****************************************************************************/
static inline void tstable_UpdateTotalBitrate( ts_table_t *p_table )
{
    block_t *p_section = p_table->p_last_table;
    unsigned int i_total_size = 0;
    unsigned int i_total_bitrate;

    while ( p_section != NULL )
    {
        i_total_size += tstable_NbTS( p_table, p_table->p_last_table )
                         * TS_SIZE * 8;
        p_section = p_section->p_next;
    }

    if ( p_table->i_rap_advance == -1 )
        i_total_bitrate = (i_total_size * INT64_C(1000000)
                            + p_table->i_period - 1)
                           / p_table->i_period;
    else
        i_total_bitrate = (i_total_size * INT64_C(1000000)
                            + p_table->i_min_period - 1)
                           / p_table->i_min_period;

    if ( p_table->i_total_bitrate != i_total_bitrate )
    {
        p_table->i_total_bitrate = i_total_bitrate;
        /* Make sure the operating mode is changed. */
        p_table->p_ts_stream->i_stream_version++;
    }

    p_table->i_ts_delay = T_STD_TS_BUFFER * INT64_C(8000000)
                           / p_table->i_total_bitrate;
}
