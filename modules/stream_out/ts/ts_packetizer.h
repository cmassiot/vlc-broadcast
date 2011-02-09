/*****************************************************************************
 * ts_packetizer.h: common code and structures for TS inputs & tables
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

#define T_STD_TS_BUFFER     512 /* bytes */

/*****************************************************************************
 * TS packetizer module definition
 *****************************************************************************/
typedef struct ts_packetizer_t ts_packetizer_t;
typedef struct ts_packetizer_sys_t ts_packetizer_sys_t;

#define TSPACK_PRIORITY_NONE    0
#define TSPACK_PRIORITY_PCR     1
#define TSPACK_PRIORITY_SI      2

#define TSPACK_COMMON_MEMBERS                                               \
    /* Module properties */                                                 \
    module_t *p_module;                                                     \
    config_chain_t *p_cfg;                                                  \
                                                                            \
    uint16_t i_pid, i_cfg_pid;                                              \
    unsigned int i_priority;                                                \
    unsigned int i_total_bitrate; /* including TS overhead */               \
    unsigned int i_peak_bitrate; /* for T-STD compliance */                 \
    mtime_t i_ts_delay;                                                     \
    ts_packetizer_sys_t *p_sys;

struct ts_packetizer_t
{
    VLC_COMMON_MEMBERS

    TSPACK_COMMON_MEMBERS
};

#define PID_TEXT N_("PID")
#define PID_LONGTEXT N_("Assign a specific PID to this ES")
#define TOTAL_BITRATE_TEXT N_("Total bitrate")
#define TOTAL_BITRATE_LONGTEXT N_("Define in bi/s the total bitrate, including PES and TS overhead")

#define TS_PACKETIZER_COMMON( pid )                                         \
    add_integer( SOUT_CFG_PREFIX "pid", pid, PID_TEXT, PID_LONGTEXT,        \
                 false );                                                   \
    add_integer( SOUT_CFG_PREFIX "total-bitrate", 0,                        \
                 TOTAL_BITRATE_TEXT, TOTAL_BITRATE_LONGTEXT, false );

#define TS_PACKETIZER_COMMON_OPTIONS "pid", "total-bitrate"

/*****************************************************************************
 * tspack_CommonOptions: called on packetizer init
 *****************************************************************************/
static inline void tspack_CommonOptions( ts_packetizer_t *p_packetizer )
{
    vlc_value_t val;

    var_Get( p_packetizer, SOUT_CFG_PREFIX "pid", &val );
    p_packetizer->i_cfg_pid = val.i_int;

    var_Get( p_packetizer, SOUT_CFG_PREFIX "total-bitrate", &val );
    p_packetizer->i_total_bitrate = val.i_int;
}

