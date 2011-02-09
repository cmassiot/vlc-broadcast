/*****************************************************************************
 * ts_input.h: common code and structures for TS inputs
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

#define DEFAULT_PID             0x1fff /* not for real, just a stub */
#define DEFAULT_PCR_PERIOD      70 /* ms */
#define DEFAULT_PCR_TOLERANCE   5 /* ms */
#define DEFAULT_BITRATE         3000000 /* bi/s */

/*****************************************************************************
 * TS parameters structure definition
 *****************************************************************************/
typedef struct ts_parameters_t ts_parameters_t;
typedef struct ts_charset_t ts_charset_t;

#define CONFORMANCE_NONE 0
#define CONFORMANCE_ISO  1
#define CONFORMANCE_ATSC 2 /* System A */
#define CONFORMANCE_DVB  3 /* System B */
#define CONFORMANCE_HDMV 4

struct ts_parameters_t
{
    unsigned int i_conformance;
    ts_charset_t *p_charset;
    uint8_t *(*pf_charset)( ts_charset_t *, char *, size_t * );

    mtime_t i_packet_interval; /* interval between two <granularity> packets */
    /* packets for time T shouldn't arrive later than T - max_prepare */
    mtime_t i_max_prepare;

    /* This is typically where you'd put stuff like non-188 bytes TS,
     * FEC, etc. */
};

/*****************************************************************************
 * TS packetizer module definition
 *****************************************************************************/
typedef struct ts_input_t ts_input_t;

struct ts_input_t
{
    VLC_COMMON_MEMBERS

    TSPACK_COMMON_MEMBERS

    es_format_t fmt;
    ts_parameters_t *p_ts_params;
    block_t *(*pf_send)( ts_input_t *, block_t * );
    mtime_t i_cfg_pcr_period, i_pcr_period, i_pcr_tolerance;
    unsigned int i_bitrate;

    /* For PMT */
    int i_es_version;
    uint8_t i_stream_type;
    uint8_t *p_descriptors;
    int i_descriptors;

    mtime_t i_next_pcr, i_last_muxing;

    uint8_t i_cc;
};

/* Input flags 0x01-0x80 - none currently */

#define PCR_TEXT N_("PCR period")
#define PCR_LONGTEXT N_("Activate PCRs on this PID with given period in ms")
#define BITRATE_TEXT N_("Theorical bitrate")
#define BITRATE_LONGTEXT N_("Define in bi/s the ES bitrate to use")

#define TS_INPUT_COMMON( flags )                                            \
    TS_PACKETIZER_COMMON( DEFAULT_PID )                                     \
    add_integer( SOUT_CFG_PREFIX "pcr", 0, PCR_TEXT, PCR_LONGTEXT,          \
                 false )                                                    \
    add_integer( SOUT_CFG_PREFIX "bitrate", 0,                              \
                 BITRATE_TEXT, BITRATE_LONGTEXT, false )

#define TS_INPUT_COMMON_OPTIONS TS_PACKETIZER_COMMON_OPTIONS, "pcr", "bitrate"

/*****************************************************************************
 * tsinput_CommonOptions: called on input init
 *****************************************************************************/
static inline void tsinput_CommonOptions( ts_input_t *p_input )
{
    vlc_value_t val;

    tspack_CommonOptions( (ts_packetizer_t *)p_input );

    var_Get( p_input, SOUT_CFG_PREFIX "pcr", &val );
    p_input->i_cfg_pcr_period = val.i_int * 1000;

    var_Get( p_input, SOUT_CFG_PREFIX "bitrate", &val );
    if ( val.i_int )
        p_input->i_bitrate = val.i_int;
    else if ( p_input->fmt.i_bitrate )
        p_input->i_bitrate = p_input->fmt.i_bitrate;
}

/*****************************************************************************
 * tsinput_CheckMuxing: check for discontinuities
 *****************************************************************************/
static inline void tsinput_CheckMuxing( ts_input_t *p_input, block_t *p_block )
{
    mtime_t i_bitrate = p_input->i_bitrate ? p_input->i_bitrate :
                        DEFAULT_BITRATE;
    mtime_t i_interpolated_muxing;

    i_interpolated_muxing = p_block->i_dts - p_block->i_delay
            - p_block->i_buffer * INT64_C(8000000) / i_bitrate;

    if ( !p_input->i_last_muxing
          || ((p_block->i_flags & BLOCK_FLAG_DISCONTINUITY)
               && p_input->i_last_muxing < i_interpolated_muxing) )
    {
        msg_Dbg( p_input, "resetting muxing date (%"PRId64"->%"PRId64")",
                 p_input->i_last_muxing, i_interpolated_muxing );
        p_input->i_last_muxing = i_interpolated_muxing;
    }
}

/*****************************************************************************
 * tsinput_NbPCR: return the number of PCRs for a given period
 *****************************************************************************/
static inline int tsinput_NbPCR( ts_input_t *p_input, mtime_t i_end )
{
    mtime_t i_next_pcr = p_input->i_next_pcr;
    int i_nb_pcr = 0;

    if ( !p_input->i_pcr_period )
        return 0;

    if ( !i_next_pcr )
        i_next_pcr = p_input->i_next_pcr = p_input->i_last_muxing;

    while ( i_next_pcr <= i_end + p_input->i_pcr_tolerance )
    {
        i_nb_pcr++;
        i_next_pcr += p_input->i_pcr_period;
    }

    return i_nb_pcr;
}

/*****************************************************************************
 * tsinput_CheckOverlap: return the number of bytes in the last incomplete TS
 *****************************************************************************/
static inline int tsinput_CheckOverlap( ts_input_t *p_input,
                                        const block_t *p_frame )
{
    int i_nb_pcr = tsinput_NbPCR( p_input, p_frame->i_dts - p_frame->i_delay );
    int i_next_ts_size = TS_SIZE
        - (i_nb_pcr ? TS_HEADER_SIZE_PCR : TS_HEADER_SIZE);
    int i_frame_size = p_frame->i_buffer;

    if ( i_frame_size <= i_next_ts_size )
        return 0; /* do not allow to destroy the PES */

    while ( i_frame_size >= i_next_ts_size )
    {
        i_frame_size -= i_next_ts_size;
        if ( i_nb_pcr ) i_nb_pcr--;
        i_next_ts_size = TS_SIZE
            - (i_nb_pcr ? TS_HEADER_SIZE_PCR : TS_HEADER_SIZE);
    }

    return i_frame_size;
}

/*****************************************************************************
 * tsinput_OverlapFrames: copy the last incomplete TS to the next PES
 *****************************************************************************/
static inline block_t *tsinput_OverlapFrames( block_t *p_dest, block_t *p_src,
                                              int i_overlap )
{
    p_dest = block_Realloc( p_dest, i_overlap, p_dest->i_buffer );
    memcpy( p_dest->p_buffer, p_src->p_buffer + p_src->i_buffer - i_overlap,
            i_overlap );
    p_src->i_buffer -= i_overlap;

    return p_dest;
}

/*****************************************************************************
 * tsinput_NbTS: return the number of TS packets for a PES
 *****************************************************************************/
static inline int tsinput_NbTS( ts_input_t *p_input, const block_t *p_frame )
{
    int i_nb_pcr = tsinput_NbPCR( p_input, p_frame->i_dts - p_frame->i_delay );
    int i_next_ts_size;
    int i_nb_ts = 0;
    int i_frame_size = p_frame->i_buffer;

    if ( i_nb_pcr )
        i_next_ts_size = TS_SIZE - TS_HEADER_SIZE_PCR;
    else if ( p_frame->i_flags & BLOCK_FLAG_DISCONTINUITY )
        i_next_ts_size = TS_SIZE - TS_HEADER_SIZE_AF;
    else
        i_next_ts_size = TS_SIZE - TS_HEADER_SIZE;

    while ( i_frame_size > 0 )
    {
        i_frame_size -= i_next_ts_size;
        i_nb_ts++;
        if ( i_nb_pcr ) i_nb_pcr--;
        i_next_ts_size = TS_SIZE
            - (i_nb_pcr ? TS_HEADER_SIZE_PCR : TS_HEADER_SIZE);
    }

    return i_nb_ts;
}

/*****************************************************************************
 * tsinput_BuildPCRTS: build dummy TS packet conveying a PCR
 *****************************************************************************/
static inline block_t *tsinput_BuildPCRTS( ts_input_t *p_input )
{
    block_t *p_block = block_New( p_input, TS_SIZE );

    ts_init( p_block->p_buffer );
    ts_set_pid( p_block->p_buffer, p_input->i_pid );
    ts_set_cc( p_block->p_buffer, p_input->i_cc );
    ts_set_adaptation( p_block->p_buffer, TS_SIZE - TS_HEADER_SIZE - 1 );

    return p_block;
}

/*****************************************************************************
 * tsinput_BuildPayloadTS: build TS packet containing payload and PCR
 *****************************************************************************/
static inline block_t *tsinput_BuildPayloadTS( ts_input_t *p_input,
                                               uint8_t *p_buffer, int i_buffer )
{
    block_t *p_block = block_New( p_input, TS_SIZE );

    ts_init( p_block->p_buffer );
    ts_set_pid( p_block->p_buffer, p_input->i_pid );
    ts_set_cc( p_block->p_buffer, ++p_input->i_cc );

    if ( i_buffer < TS_SIZE - TS_HEADER_SIZE )
        ts_set_adaptation( p_block->p_buffer,
                           TS_SIZE - i_buffer - TS_HEADER_SIZE - 1 );
    ts_set_payload( p_block->p_buffer );

    vlc_memcpy( ts_payload( p_block->p_buffer ), p_buffer, i_buffer );

    return p_block;
}

/*****************************************************************************
 * tsinput_BuildTS: build a chain of TS packets for a PES
 *****************************************************************************/
static inline block_t *tsinput_BuildTS( ts_input_t *p_input,
                                        const block_t *p_frame )
{
    int i_nb_ts, i;
    mtime_t i_duration, i_peak_duration;
    block_t *p_first = NULL;
    block_t **pp_last = &p_first;
    uint8_t *p_buffer = p_frame->p_buffer;
    int i_buffer = p_frame->i_buffer;

    i_nb_ts = tsinput_NbTS( p_input, p_frame );
    i_duration = p_frame->i_dts - p_frame->i_delay - p_input->i_last_muxing;

    if ( p_input->i_peak_bitrate )
        i_peak_duration = p_frame->i_buffer * INT64_C(8000000)
                           / p_input->i_peak_bitrate;
    else
        i_peak_duration = i_duration;

#if 0 /* too depressing */
    if ( p_input->i_total_bitrate
          && i_nb_ts * TS_SIZE * INT64_C(8000000) / i_duration
              > p_input->i_total_bitrate )
        msg_Warn( p_input,
                  "input bitrate higher than provisioned bitrate (%"PRId64")",
                  i_nb_ts * TS_SIZE * INT64_C(8000000) / i_duration );
#endif

    for ( i = i_nb_ts - 1; i >= 0; i-- )
    {
        mtime_t i_muxing = p_frame->i_dts - p_frame->i_delay
                            - i * i_duration / i_nb_ts;
        bool b_has_pcr;
        int i_ts_payload;

        if ( p_input->i_pcr_period )
        {
            while ( p_input->i_next_pcr < i_muxing - p_input->i_pcr_tolerance )
            {
                /* Insert adaptation field-only packet. */
                *pp_last = tsinput_BuildPCRTS( p_input );
                (*pp_last)->i_dts = p_frame->i_dts - i * i_peak_duration
                                     / i_nb_ts;
                (*pp_last)->i_delay = (*pp_last)->i_dts - p_input->i_next_pcr
                                       + p_input->i_ts_delay;
                /* will be overwritten later */
                tsaf_set_pcr( (*pp_last)->p_buffer, 0 );
                p_input->i_next_pcr += p_input->i_pcr_period;
                pp_last = &(*pp_last)->p_next;
            }
        }

        b_has_pcr = p_input->i_pcr_period
                      && p_input->i_next_pcr <= i_muxing
                                                 + p_input->i_pcr_tolerance;
        if ( b_has_pcr )
            i_ts_payload = TS_SIZE - TS_HEADER_SIZE_PCR;
        else if ( i == i_nb_ts - 1
                   && (p_frame->i_flags & BLOCK_FLAG_DISCONTINUITY) )
            i_ts_payload = TS_SIZE - TS_HEADER_SIZE_AF;
        else
            i_ts_payload = TS_SIZE - TS_HEADER_SIZE;
        if ( i_ts_payload > i_buffer )
            i_ts_payload = i_buffer;

        *pp_last = tsinput_BuildPayloadTS( p_input, p_buffer, i_ts_payload );
        (*pp_last)->i_dts = p_frame->i_dts - i * i_peak_duration / i_nb_ts;
        (*pp_last)->i_delay = (*pp_last)->i_dts - i_muxing
                               + p_input->i_ts_delay;
        if ( (*pp_last)->i_delay <= 0 )
        {
            msg_Warn( p_input, "too short delay %"PRId64" (pes=%"PRId64"), d=%"PRId64" p=%"PRId64,
                      (*pp_last)->i_delay, p_frame->i_delay, i_duration,
                      i_peak_duration );
            (*pp_last)->i_delay = 0;
        }

        if ( i == i_nb_ts - 1 )
        {
            ts_set_unitstart( (*pp_last)->p_buffer );
            if ( p_frame->i_flags & BLOCK_FLAG_DISCONTINUITY )
                tsaf_set_discontinuity( (*pp_last)->p_buffer );
        }
        if ( b_has_pcr )
        {
            /* will be overwritten later */
            tsaf_set_pcr( (*pp_last)->p_buffer, 0 );
            p_input->i_next_pcr += p_input->i_pcr_period;
        }

        p_buffer += i_ts_payload;
        i_buffer -= i_ts_payload;
        pp_last = &(*pp_last)->p_next;
    }
    p_input->i_last_muxing = p_frame->i_dts - p_frame->i_delay;

    if ( i_buffer )
        msg_Err( p_input, "internal error #2 %d", i_buffer );

    return p_first;
}

