/*****************************************************************************
 * Copyright © 2010 VideoLAN
 *
 * Authors: Jean-Baptiste Kempf <jb@videolan.org>
 *          Narendra Sankar <nsankar@broadcom.com>
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

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/* VLC includes */
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>

#if !defined(_WIN32) && !defined(__APPLE__)
   #define __LINUX_USER__
#endif

/* CrystalHD */
#include <libcrystalhd/bc_dts_defs.h>
#include <libcrystalhd/bc_dts_types.h>
#if HAVE_LIBCRYSTALHD_BC_DRV_IF_H
  #include <libcrystalhd/bc_drv_if.h>
#else
  #include <libcrystalhd/libcrystalhd_if.h>
#endif

#include <assert.h>

/* BC pts are multiple of 100ns */
#define TO_BC_PTS( a ) ( a * 10 + 1 )
#define FROM_BC_PTS( a ) ((a - 1) /10)

//#define DEBUG_CRYSTALHD 1

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int        OpenDecoder  ( vlc_object_t * );
static void       CloseDecoder ( vlc_object_t * );

vlc_module_begin ()
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_VCODEC )
    set_description( N_("Crystal HD hardware video decoder") )
    set_capability( "decoder", 0 )
    set_callbacks( OpenDecoder, CloseDecoder )
    add_shortcut( "crystalhd" )
vlc_module_end ()

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static picture_t *DecodeBlock   ( decoder_t *p_dec, block_t **pp_block );
static void crystal_CopyPicture ( picture_t *, BC_DTS_PROC_OUT* );
static int crystal_insert_sps_pps(decoder_t *, uint8_t *, uint32_t);

/*****************************************************************************
 * decoder_sys_t : CrysalHD decoder structure
 *****************************************************************************/
struct decoder_sys_t
{
    HANDLE bcm_handle;       /* Device Handle */

    uint8_t *p_sps_pps_buf;  /* SPS/PPS buffer */
    uint32_t i_sps_pps_size; /* SPS/PPS size */

    uint32_t i_nal_size;     /* NAL header size */
};

/*****************************************************************************
 * OpenDecoder: probe the decoder and return score
 *****************************************************************************/
static int OpenDecoder( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys;

    /* Codec specifics */
    uint32_t i_bcm_codec_subtype = 0;
    switch ( p_dec->fmt_in.i_codec )
    {
    case VLC_CODEC_H264:
        if( p_dec->fmt_in.i_original_fourcc == VLC_FOURCC( 'a', 'v', 'c', '1' ) )
            i_bcm_codec_subtype = BC_MSUBTYPE_AVC1;
        else
            i_bcm_codec_subtype = BC_MSUBTYPE_H264;
        break;
    case VLC_CODEC_VC1:
        i_bcm_codec_subtype = BC_MSUBTYPE_VC1;
        break;
    case VLC_CODEC_WMV3:
        i_bcm_codec_subtype = BC_MSUBTYPE_WMV3;
        break;
    case VLC_CODEC_WMVA:
        i_bcm_codec_subtype = BC_MSUBTYPE_WMVA;
        break;
    case VLC_CODEC_MPGV:
        i_bcm_codec_subtype = BC_MSUBTYPE_MPEG2VIDEO;
        break;
/* Not ready for production yet
    case VLC_CODEC_MP4V:
        i_bcm_codec_subtype = BC_MSUBTYPE_DIVX;
        break; */
    default:
        return VLC_EGENERIC;
    }

#ifdef _WIN32
    HINSTANCE p_bcm_dll = LoadLibrary( "bcmDIL.dll" );
    if( !p_bcm_dll )
    {
        #ifdef DEBUG_CRYSTALHD
        msg_Dbg( p_dec, "Couldn't load the CrystalHD dll");
        #endif
        return VLC_EGENERIC
    }
#endif

    /* Allocate the memory needed to store the decoder's structure */
    p_sys = malloc( sizeof(*p_sys) );
    if( !p_sys )
        return VLC_ENOMEM;

    /* Fill decoder_sys_t */
    p_dec->p_sys          = p_sys;
    p_sys->i_nal_size     = 4; // assume 4 byte start codes
    p_sys->i_sps_pps_size = 0;
    p_sys->p_sps_pps_buf  = NULL;


#ifdef DEBUG_CRYSTALHD
    msg_Dbg( p_dec, "Trying to open CrystalHD HW");
#endif

    /* Get the handle for the device */
    if( DtsDeviceOpen( &p_sys->bcm_handle,
                      (DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB ) )
                      // | DTS_DFLT_RESOLUTION(vdecRESOLUTION_720p29_97) ) )
                     != BC_STS_SUCCESS )
    {
        msg_Err( p_dec, "Couldn't find and open the BCM CrystalHD device" );
        free( p_sys );
        return VLC_EGENERIC;
    }

#ifdef DEBUG_CRYSTALHD
    BC_INFO_CRYSTAL info;
    if( DtsCrystalHDVersion( p_sys->bcm_handle, &info ) == BC_STS_SUCCESS )
    {
        msg_Dbg( p_dec, "Using CrystalHD Driver version: %i.%i.%i, "
            "Library version: %i.%i.%i, "
            "Firmware version: %i.%i.%i",
            info.drvVersion.drvRelease, info.drvVersion.drvMajor, info.drvVersion.drvMinor,
            info.dilVersion.dilRelease, info.dilVersion.dilMajor, info.dilVersion.dilMinor,
            info.fwVersion.fwRelease, info.fwVersion.fwMajor, info.fwVersion.fwMinor );
    }
#endif

    /* Special case for AVC1 */
    if( i_bcm_codec_subtype == BC_MSUBTYPE_AVC1 )
    {
        if( p_dec->fmt_in.i_extra > 0 )
        {
            msg_Dbg( p_dec, "Parsing extra infos for avc1" );
            if( crystal_insert_sps_pps( p_dec, (uint8_t*)p_dec->fmt_in.p_extra,
                        p_dec->fmt_in.i_extra ) != VLC_SUCCESS )
                goto error;
        }
        else
        {
            msg_Err( p_dec, "Missing extra infos for avc1" );
            goto error;
        }
    }

    /* Always YUY2 color */
    if( DtsSetColorSpace( p_sys->bcm_handle, OUTPUT_MODE422_YUY2 ) != BC_STS_SUCCESS )
    {
        msg_Err( p_dec, "Couldn't set the color space. Please report this!" );
        goto error;
    }

    /* Prepare Input for the device */
    BC_INPUT_FORMAT p_in;
    memset( &p_in, 0, sizeof(BC_INPUT_FORMAT) );
    p_in.OptFlags    = 0x51; /* 0b 0 1 01 0001 */
    p_in.mSubtype    = i_bcm_codec_subtype;
    p_in.startCodeSz = p_sys->i_nal_size;
    p_in.pMetaData   = p_sys->p_sps_pps_buf;
    p_in.metaDataSz  = p_sys->i_sps_pps_size;
    p_in.width       = p_dec->fmt_in.video.i_width;
    p_in.height      = p_dec->fmt_in.video.i_height;
    p_in.Progressive = true;

    if( DtsSetInputFormat( p_sys->bcm_handle, &p_in ) != BC_STS_SUCCESS )
    {
        msg_Err( p_dec, "Couldn't set the color space. Please report this!" );
        goto error;
    }

    /* Open a decoder */
    if( DtsOpenDecoder( p_sys->bcm_handle, BC_STREAM_TYPE_ES ) != BC_STS_SUCCESS )
    {
        msg_Err( p_dec, "Couldn't open the CrystalHD decoder" );
        goto error;
    }

    /* Start it */
    if( DtsStartDecoder( p_sys->bcm_handle ) != BC_STS_SUCCESS )
    {
        msg_Err( p_dec, "Couldn't start the decoder" );
        goto error;
    }

    if( DtsStartCapture( p_sys->bcm_handle ) != BC_STS_SUCCESS )
    {
        msg_Err( p_dec, "Couldn't start the capture" );
        goto error_complete;
    }

    /* Set output properties */
    p_dec->fmt_out.i_cat          = VIDEO_ES;
    p_dec->fmt_out.i_codec        = VLC_CODEC_YUYV;
    p_dec->fmt_out.video.i_width  = p_dec->fmt_in.video.i_width;
    p_dec->fmt_out.video.i_height = p_dec->fmt_in.video.i_height;
    p_dec->b_need_packetized      = true;

    /* Set callbacks */
    p_dec->pf_decode_video = DecodeBlock;

    msg_Info( p_dec, "Opened CrystalHD hardware with success" );
    return VLC_SUCCESS;

error_complete:
    DtsCloseDecoder( p_sys->bcm_handle );
error:
    DtsDeviceClose( p_sys->bcm_handle );
    free( p_sys );
    return VLC_EGENERIC;
}

/*****************************************************************************
 * CloseDecoder: decoder destruction
 *****************************************************************************/
static void CloseDecoder( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t *)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( DtsFlushInput( p_sys->bcm_handle, 2 ) != BC_STS_SUCCESS )
        goto error;
    if( DtsStopDecoder( p_sys->bcm_handle ) != BC_STS_SUCCESS )
        goto error;
    if( DtsCloseDecoder( p_sys->bcm_handle ) != BC_STS_SUCCESS )
        goto error;
    if( DtsDeviceClose( p_sys->bcm_handle ) != BC_STS_SUCCESS )
        goto error;

error:
    free( p_sys->p_sps_pps_buf );
#ifdef DEBUG_CRYSTALHD
    msg_Dbg( p_dec, "done cleaning up CrystalHD" );
#endif
    free( p_sys );
}

/****************************************************************************
 * DecodeBlock: the whole thing
 ****************************************************************************/
static picture_t *DecodeBlock( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_block;

    BC_DTS_PROC_OUT proc_out;
    BC_DTS_STATUS driver_stat;

    picture_t *p_pic;

    /* First check the status of the decode to produce pictures */
    if( DtsGetDriverStatus( p_sys->bcm_handle, &driver_stat ) != BC_STS_SUCCESS )
        return NULL;

    p_block = *pp_block;
    if( p_block )
    {
        if( ( p_block->i_flags&(BLOCK_FLAG_DISCONTINUITY|BLOCK_FLAG_CORRUPTED) ) == 0 )
        {
            /* Valid input block, so we can send to HW to decode */

            BC_STATUS status = DtsProcInput( p_sys->bcm_handle,
                                             p_block->p_buffer,
                                             p_block->i_buffer,
                                             p_block->i_pts >= VLC_TS_INVALID ? TO_BC_PTS(p_block->i_pts) : 0, false );

            block_Release( p_block );
            *pp_block = NULL;

            if( status != BC_STS_SUCCESS )
                return NULL;
        }
    }
#ifdef DEBUG_CRYSTALHD
    else
    {
        if( driver_stat.ReadyListCount != 0 )
            msg_Err( p_dec, " Input NULL but have pictures %u", driver_stat.ReadyListCount );
    }
#endif

    if( driver_stat.ReadyListCount == 0 )
        return NULL;

    /* Prepare the Output structure */
    /* We always expect and use YUY2 */
    memset( &proc_out, 0, sizeof(BC_DTS_PROC_OUT) );
    proc_out.PicInfo.width  = p_dec->fmt_out.video.i_width;
    proc_out.PicInfo.height = p_dec->fmt_out.video.i_height;
    proc_out.YbuffSz        = p_dec->fmt_out.video.i_width * p_dec->fmt_out.video.i_height  / 2;
    proc_out.Ybuff          = malloc( proc_out.YbuffSz  * 4);               // Allocate in bytes
    proc_out.PoutFlags      = BC_POUT_FLAGS_SIZE;                           //FIXME why?

#ifdef DEBUG_CRYSTALHD
    msg_Dbg( p_dec, "%i, %i",  p_dec->fmt_out.video.i_width, p_dec->fmt_out.video.i_height );
#endif
    if( !proc_out.Ybuff )
        return NULL;

    BC_STATUS sts = DtsProcOutput( p_sys->bcm_handle, 128, &proc_out );
#ifdef DEBUG_CRYSTALHD
    if( sts != BC_STS_SUCCESS )
        msg_Err( p_dec, "DtsProcOutput returned %i", sts );
#endif

    uint8_t b_eos;
    switch( sts )
    {
        case BC_STS_SUCCESS:
            if( !(proc_out.PoutFlags & BC_POUT_FLAGS_PIB_VALID) )
            {
                msg_Dbg( p_dec, "Invalid PIB" );
                break;
            }

            p_pic = decoder_NewPicture( p_dec );
            if( !p_pic )
                break;

            crystal_CopyPicture( p_pic, &proc_out );
            p_pic->date = proc_out.PicInfo.timeStamp > 0 ? FROM_BC_PTS(proc_out.PicInfo.timeStamp) : VLC_TS_INVALID;
            //p_pic->date += 100 * 1000;
#ifdef DEBUG_CRYSTALHD
            msg_Dbg( p_dec, "TS Output is %"PRIu64, p_pic->date);
#endif
            free( proc_out.Ybuff );
            return p_pic;

        case BC_STS_DEC_NOT_OPEN:
        case BC_STS_DEC_NOT_STARTED:
            msg_Err( p_dec, "Decoder not opened or started" );
            break;

        case BC_STS_INV_ARG:
            msg_Warn( p_dec, "Invalid arguments. Please report" );
            break;

        case BC_STS_FMT_CHANGE:    /* Format change */
            /* if( !(proc_out.PoutFlags & BC_POUT_FLAGS_PIB_VALID) )
                break; */
            p_dec->fmt_out.video.i_width  = proc_out.PicInfo.width;
            p_dec->fmt_out.video.i_height = proc_out.PicInfo.height;
#define setAR( a, b, c ) case a: p_dec->fmt_out.video.i_sar_num = b; p_dec->fmt_out.video.i_sar_den = c; break;
            switch( proc_out.PicInfo.aspect_ratio )
            {
                setAR( vdecAspectRatioSquare, 1, 1 )
                setAR( vdecAspectRatio12_11, 12, 11 )
                setAR( vdecAspectRatio10_11, 10, 11 )
                setAR( vdecAspectRatio16_11, 16, 11 )
                setAR( vdecAspectRatio40_33, 40, 33 )
                setAR( vdecAspectRatio24_11, 24, 11 )
                setAR( vdecAspectRatio20_11, 20, 11 )
                setAR( vdecAspectRatio32_11, 32, 11 )
                setAR( vdecAspectRatio80_33, 80, 33 )
                setAR( vdecAspectRatio18_11, 18, 11 )
                setAR( vdecAspectRatio15_11, 15, 11 )
                setAR( vdecAspectRatio64_33, 64, 33 )
                setAR( vdecAspectRatio160_99, 160, 99 )
                setAR( vdecAspectRatio4_3, 4, 3 )
                setAR( vdecAspectRatio16_9, 16, 9 )
                setAR( vdecAspectRatio221_1, 221, 1 )
                default: break;
            }
#undef setAR
            msg_Dbg( p_dec, "Format Change Detected [%i, %i], AR: %i/%i",
                    proc_out.PicInfo.width, proc_out.PicInfo.height,
                    p_dec->fmt_out.video.i_sar_num, p_dec->fmt_out.video.i_sar_den );
            break;

        /* Nothing is documented here... */
        case BC_STS_NO_DATA:
            if( DtsIsEndOfStream( p_sys->bcm_handle, &b_eos ) == BC_STS_SUCCESS )
                if( b_eos )
                    msg_Dbg( p_dec, "End of Stream" );
            break;
        case BC_STS_TIMEOUT:       /* Timeout */
            msg_Err( p_dec, "ProcOutput timeout" );
            break;
        case BC_STS_IO_XFR_ERROR:
        case BC_STS_IO_USER_ABORT:
        case BC_STS_IO_ERROR:
            msg_Err( p_dec, "ProcOutput return mode not implemented. Please report" );
            break;
        default:
            msg_Err( p_dec, "Unknown return status. Please report %i", sts );
            break;
    }
    free( proc_out.Ybuff );
    return NULL;
}

/* Copy the data
 * FIXME: this should not exist */
static void crystal_CopyPicture ( picture_t *p_pic, BC_DTS_PROC_OUT* p_out )
{
    int i_dst_stride;
    uint8_t *p_dst, *p_dst_end;
    uint8_t *p_src = p_out->Ybuff;

    p_dst         = p_pic->p[0].p_pixels;
    i_dst_stride  = p_pic->p[0].i_pitch;
    p_dst_end     = p_dst  + (i_dst_stride * p_out->PicInfo.height);

    for( ; p_dst < p_dst_end; p_dst += i_dst_stride, p_src += (p_out->PicInfo.width * 2))
        vlc_memcpy( p_dst, p_src, p_out->PicInfo.width * 2); // Copy in bytes
}

/* Parse the SPS/PPS Metadata to feed the decoder for avc1 */
static int crystal_insert_sps_pps(decoder_t *p_dec, uint8_t *p_buf, uint32_t i_buf_size)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    int i_profile;
    uint32_t i_data_size = i_buf_size, i_nal_size;
    unsigned int i_loop_end;

    p_sys->i_sps_pps_size = 0;

    p_sys->p_sps_pps_buf = malloc( p_dec->fmt_in.i_extra * 2 );
    if( !p_sys->p_sps_pps_buf )
        return VLC_ENOMEM;

    /* */
    if( i_data_size < 7 )
    {
        msg_Err( p_dec, "Input Metadata too small" );
        goto error;
    }

    /* Read infos in first 6 bytes */
    i_profile         = (p_buf[1] << 16) | (p_buf[2] << 8) | p_buf[3];
    p_sys->i_nal_size = (p_buf[4] & 0x03) + 1;
    p_buf += 5;
    i_data_size -= 5;

    for ( unsigned int j = 0; j < 2; j++ )
    {
        /* First time is SPS, Second is PPS */
        if (i_data_size < 1) {
            msg_Err( p_dec, "PPS too small after processing SPS/PPS %u", i_data_size );
            goto error;
        }
        i_loop_end = p_buf[0] & (j == 0 ? 0x1f : 0xff);
        p_buf++; i_data_size--;

        for ( unsigned int i = 0; i < i_loop_end; i++)
        {
            if (i_data_size < 2 ) {
                msg_Err( p_dec, "SPS is too small %u", i_data_size );
                goto error;
            }

            i_nal_size = (p_buf[0] << 8) | p_buf[1];
            p_buf += 2;
            i_data_size -= 2;

            if (i_data_size < i_nal_size ) {
                msg_Err( p_dec, "SPS size does not match NAL specified size %u", i_data_size );
                goto error;
            }

            p_sys->p_sps_pps_buf[p_sys->i_sps_pps_size++] = 0;
            p_sys->p_sps_pps_buf[p_sys->i_sps_pps_size++] = 0;
            p_sys->p_sps_pps_buf[p_sys->i_sps_pps_size++] = 0;
            p_sys->p_sps_pps_buf[p_sys->i_sps_pps_size++] = 1;

            memcpy(p_sys->p_sps_pps_buf + p_sys->i_sps_pps_size, p_buf, i_nal_size);
            p_sys->i_sps_pps_size += i_nal_size;

            p_buf += i_nal_size;
            i_data_size -= i_nal_size;
        }
    }

    return VLC_SUCCESS;

error:
    free( p_sys->p_sps_pps_buf );
    p_sys->p_sps_pps_buf = NULL;
    return VLC_ENOMEM;
}

