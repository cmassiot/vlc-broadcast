/*****************************************************************************
 * ts_audio.h: common code to audio TS encapsulator
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

#define T_STD_BUFFER        3740 /* bytes */
#define T_STD_PEAK_RATE     2000000 /* bi/s */
/* This is the theoritical max: */
//#define T_STD_MAX_RETENTION 1000 /* ms */
/* This is what we want: */
#define T_STD_MAX_RETENTION 100 /* ms */
#define DEFAULT_DELAY       100 /* ms, for non-compliant VBR streams */

/*****************************************************************************
 * TS packetizer module definition
 *****************************************************************************/
#ifndef TS_AUDIO_EXTRA_SYS
#   define TS_AUDIO_EXTRA_SYS
#endif

struct ts_packetizer_sys_t
{
    uint8_t i_stream_id;
    bool b_align, b_first;
    int i_nb_frames;
    char pi_language[3];
    char *psz_ref_language;
    uint8_t i_audio_type;

    block_t *p_frames;
    block_t **pp_last_frame;
    int i_frames;

    TS_AUDIO_EXTRA_SYS
};

#define LANG_TEXT N_("Language")
#define LANG_LONGTEXT N_("Assign a specific ISO-639 language to this ES")
#define AUDIO_TYPE_TEXT N_("Audio type")
#define AUDIO_TYPE_LONGTEXT N_("Define the audio type in the ISO-639 descriptor (0=unknown, 1=clean effects, 2=hearing impaired, 3=visual impaired commentary)")
#define ALIGN_TEXT N_("Always align")
#define ALIGN_LONGTEXT N_("Always align frame header to the start of a PES (suboptimal overhead)")
#define FRAMES_TEXT N_("Frames per PES")
#define FRAMES_LONGTEXT N_("Define the number of audio frames per PES")

/* Audio flags */
#define AUDIO_ALIGNED   0x100

#define TS_AUDIO_COMMON( flags )                                            \
    TS_INPUT_COMMON( flags )                                                \
                                                                            \
    add_string( SOUT_CFG_PREFIX "lang", "", LANG_TEXT, LANG_LONGTEXT,       \
                false )                                                     \
    add_integer( SOUT_CFG_PREFIX "audio-type", 0, AUDIO_TYPE_TEXT,          \
                 AUDIO_TYPE_LONGTEXT, false )                               \
    add_bool( SOUT_CFG_PREFIX "align", !!((flags) & AUDIO_ALIGNED),         \
              ALIGN_TEXT, ALIGN_LONGTEXT, false )                           \
    add_integer( SOUT_CFG_PREFIX "frames-per-pes", 6, FRAMES_TEXT,          \
                 FRAMES_LONGTEXT, false )

#define TS_AUDIO_COMMON_OPTIONS TS_INPUT_COMMON_OPTIONS, "lang", "align", "frames-per-pes", "audio-type"

/*****************************************************************************
 * tsaudio_GetLanguage:
 *****************************************************************************/
static inline void tsaudio_GetLanguage( ts_input_t *p_input )
{
    ts_packetizer_sys_t *p_sys = p_input->p_sys;
    const char *psz = p_input->fmt.psz_language;
    const iso639_lang_t *pl = NULL;

    free( p_sys->psz_ref_language );
    p_sys->psz_ref_language = strdup( p_input->fmt.psz_language );
    if ( strlen( psz ) == 2 )
    {
        pl = GetLang_1( psz );
    }
    else if ( strlen( psz ) == 3 )
    {
        pl = GetLang_2B( psz );
        if ( !strcmp( pl->psz_iso639_1, "??" ) )
            pl = GetLang_2T( psz );
    }
    if( pl != NULL && strcmp( pl->psz_iso639_1, "??" ) )
        memcpy( p_sys->pi_language, pl->psz_iso639_2T, 3 );
}

/*****************************************************************************
 * tsaudio_LanguageChanged:
 *****************************************************************************/
static inline bool tsaudio_LanguageChanged( ts_input_t *p_input )
{
    ts_packetizer_sys_t *p_sys = p_input->p_sys;
    return (p_sys->psz_ref_language != NULL && p_input->fmt.psz_language != NULL
             && strcmp( p_sys->psz_ref_language, p_input->fmt.psz_language ));
}

/*****************************************************************************
 * tsaudio_SetLanguageDescr:
 *****************************************************************************/
static inline void tsaudio_SetLanguageDescr( ts_input_t *p_input )
{
    ts_packetizer_sys_t *p_sys = p_input->p_sys;
    uint8_t *p_descriptor = p_input->p_descriptors;
    uint8_t *p_end = p_descriptor + p_input->i_descriptors;

    while ( p_descriptor < p_end )
    {
        if ( desc_get_tag( p_descriptor ) == 0xa )
            break;
        p_descriptor += DESC_HEADER_SIZE + desc_get_length( p_descriptor );
    }

    if ( p_descriptor >= p_end )
    {
        p_input->p_descriptors = realloc( p_input->p_descriptors,
           p_input->i_descriptors + DESC0A_HEADER_SIZE + DESC0A_LANGUAGE_SIZE );
        p_descriptor = p_input->p_descriptors + p_input->i_descriptors;
        p_input->i_descriptors += DESC0A_HEADER_SIZE + DESC0A_LANGUAGE_SIZE;

        desc_set_tag( p_descriptor, 0xa );
        desc_set_length( p_descriptor,
            DESC0A_HEADER_SIZE + DESC0A_LANGUAGE_SIZE - DESC_HEADER_SIZE );
    }

    desc0an_set_code( p_descriptor + DESC0A_HEADER_SIZE,
                      (const uint8_t *)p_sys->pi_language );
    desc0an_set_audiotype( p_descriptor + DESC0A_HEADER_SIZE,
                           p_sys->i_audio_type );
}

/*****************************************************************************
 * tsaudio_CommonOptions: called on input init
 *****************************************************************************/
static inline void tsaudio_CommonOptions( ts_input_t *p_input,
                                          uint8_t i_stream_id )
{
    ts_packetizer_sys_t *p_sys = p_input->p_sys;
    vlc_value_t val;

    tsinput_CommonOptions( p_input );

    p_input->i_peak_bitrate = T_STD_PEAK_RATE;
    if ( !p_input->fmt.audio.i_bytes_per_frame && p_input->fmt.audio.i_rate )
        p_input->fmt.audio.i_bytes_per_frame =
            (p_input->fmt.i_bitrate * p_input->fmt.audio.i_frame_length
              / p_input->fmt.audio.i_rate + 7) / 8;

    var_Get( p_input, SOUT_CFG_PREFIX "align", &val );
    p_sys->b_align = val.b_bool;

    var_Get( p_input, SOUT_CFG_PREFIX "frames-per-pes", &val );
    p_sys->i_nb_frames = val.i_int;
    if ( p_input->fmt.audio.i_bytes_per_frame * p_sys->i_nb_frames
          > T_STD_BUFFER  )
        p_sys->i_nb_frames = T_STD_BUFFER
                              / p_input->fmt.audio.i_bytes_per_frame;

    var_Get( p_input, SOUT_CFG_PREFIX "audio-type", &val );
    p_sys->i_audio_type = val.i_int;

    p_sys->psz_ref_language = NULL;
    memset( p_sys->pi_language, 0, 3 );
    var_Get( p_input, SOUT_CFG_PREFIX "lang", &val );
    if ( val.psz_string != NULL && *val.psz_string )
        memcpy( p_sys->pi_language, val.psz_string, 3 );
    else if ( p_input->fmt.psz_language != NULL )
        tsaudio_GetLanguage( p_input );
    free( val.psz_string );

    if ( p_sys->pi_language[0] )
        tsaudio_SetLanguageDescr( p_input );

    p_sys->i_stream_id = i_stream_id;
    p_sys->p_frames = NULL;
    p_sys->pp_last_frame = &p_sys->p_frames;
    p_sys->i_frames = 0;
    p_sys->b_first = true;
}

/*****************************************************************************
 * tsaudio_Close:
 *****************************************************************************/
static inline void tsaudio_Close( ts_input_t *p_input )
{
    ts_packetizer_sys_t *p_sys = p_input->p_sys;

    if ( p_sys->p_frames != NULL )
        block_ChainRelease( p_sys->p_frames );
    free( p_sys->psz_ref_language );
    free( p_input->p_descriptors );
}

/*****************************************************************************
 * tsaudio_SetPESHeader:
 *****************************************************************************/
static inline block_t *tsaudio_SetPESHeader( ts_input_t *p_input,
                                             block_t *p_frame )
{
    ts_packetizer_sys_t *p_sys = p_input->p_sys;

    p_frame = block_Realloc( p_frame, PES_HEADER_SIZE_PTS, p_frame->i_buffer );
    pes_init( p_frame->p_buffer );
    pes_set_streamid( p_frame->p_buffer, p_sys->i_stream_id );
    /* Length will be set later */
    pes_set_headerlength( p_frame->p_buffer, 0 );
    pes_set_pts( p_frame->p_buffer, p_frame->i_pts * 9 / 100 );

    if ( p_frame->i_flags & BLOCK_FLAG_ALIGNED )
        pes_set_dataalignment( p_frame->p_buffer );

    return p_frame;
}

/*****************************************************************************
 * tsaudio_OutputFrame:
 *****************************************************************************/
static inline block_t *tsaudio_OutputFrame( ts_input_t *p_input,
                                            block_t *p_frame )
{
    block_t *p_first;
    int i_length = p_frame->i_buffer - PES_HEADER_SIZE;
    /* shouldn't be > 65535 */
    pes_set_length( p_frame->p_buffer, i_length > 65535 ? 0 : i_length );

    p_first = tsinput_BuildTS( p_input, p_frame );

    block_Release( p_frame );
    return p_first;
}

/*****************************************************************************
 * tsaudio_HandleFrame:
 *****************************************************************************/
static inline block_t *tsaudio_HandleFrame( ts_input_t *p_input,
                                            block_t *p_frame )
{
    ts_packetizer_sys_t *p_sys = p_input->p_sys;
    block_t *p_out = NULL;

    if ( !p_sys->i_frames )
        p_frame->i_flags |= BLOCK_FLAG_ALIGNED;

    if ( p_sys->b_first )
    {
        p_frame->i_flags |= BLOCK_FLAG_DISCONTINUITY;
        p_sys->b_first = false;
    }

    if ( p_sys->i_frames >= p_sys->i_nb_frames )
    {
        block_t *p_pes = block_ChainGather( p_sys->p_frames );

        if ( p_input->i_bitrate )
            p_pes->i_delay = (T_STD_BUFFER - p_pes->i_buffer)
                                 * INT64_C(8000000) / p_input->i_bitrate;
        else
            p_pes->i_delay = DEFAULT_DELAY * 1000;
        if ( p_pes->i_delay > T_STD_MAX_RETENTION * 1000 )
            p_pes->i_delay = T_STD_MAX_RETENTION * 1000;
        tsinput_CheckMuxing( p_input, p_pes );

        p_frame->i_flags |= BLOCK_FLAG_ALIGNED;
        if ( !p_sys->b_align )
        {
            int i_overlap_size = tsinput_CheckOverlap( p_input,
                                                       p_pes );
            if ( i_overlap_size )
            {
                p_frame = tsinput_OverlapFrames( p_frame, p_pes,
                                                 i_overlap_size );
                p_frame->i_flags &= ~BLOCK_FLAG_ALIGNED;
            }
        }

        p_out = tsaudio_OutputFrame( p_input, p_pes );
        p_sys->p_frames = NULL;
        p_sys->pp_last_frame = &p_sys->p_frames;
        p_sys->i_frames = 0;
    }

    if ( !p_sys->i_frames )
        p_frame = tsaudio_SetPESHeader( p_input, p_frame );

    block_ChainLastAppend( &p_sys->pp_last_frame, p_frame );
    p_sys->i_frames++;

    return p_out;
}
