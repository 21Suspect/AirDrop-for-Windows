// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iwl_firmware.h"
enum IWL_RADIO_COMMAND_ID {
    IwlRadioAntenna, IwlRadioCoexistence, IwlRadioSoc, IwlRadioTemperature,
    IwlRadioPower, IwlRadioMcc, IwlRadioPhy, IwlRadioRlc,
    IwlRadioMac, IwlRadioBinding, IwlRadioStation, IwlRadioCommandCount
};
typedef struct IWL_RADIO_COMMAND {
    uint32_t group, opcode, version, length;
    uint8_t data[160];
} IWL_RADIO_COMMAND;
typedef struct IWL_MCC_INFO {
    uint32_t status, mcc, capabilities, source, count, channels[110];
} IWL_MCC_INFO;
// Pinned API-77 SO/GF command set: receive-only channel 6, 20 MHz, no TX queue.
// No user-controlled channel, firmware command, country override or frame data.
IWL_FW_EXPORT int iwl_radio_monitor_command(unsigned id, unsigned tx_chains,
    unsigned rx_chains, const uint8_t local_address[6], IWL_RADIO_COMMAND *);
IWL_FW_EXPORT int iwl_radio_mcc_decode(const uint8_t *, size_t, IWL_MCC_INFO *);

typedef struct IWL_TX_COMPLETION_INFO {
    uint32_t status, queue, next_index, rate, byte_count, retries, success;
} IWL_TX_COMPLETION_INFO;
IWL_FW_EXPORT int iwl_radio_queue_command(uint64_t tfd_address,uint64_t count_address,
    IWL_RADIO_COMMAND *);
IWL_FW_EXPORT int iwl_radio_tx_completion(const uint8_t *,size_t,
    unsigned queue,unsigned index,IWL_TX_COMPLETION_INFO *);

enum IWL_IBSS_COMMAND_ID {
    IwlIbssBeacon, IwlIbssMac, IwlIbssBinding,
    IwlIbssMulticast, IwlIbssBroadcast, IwlIbssCommandCount
};
// Fixed HT20 channel-6 cell on AWDL's BSSID. No regulatory permission implied.
IWL_FW_EXPORT int iwl_radio_ibss_command(unsigned id,const uint8_t local_address[6],
    IWL_RADIO_COMMAND *);
IWL_FW_EXPORT int iwl_radio_station_queue(uint64_t tfd,uint64_t counts,
    unsigned station,unsigned tid,IWL_RADIO_COMMAND *);
