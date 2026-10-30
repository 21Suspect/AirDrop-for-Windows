// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iwl_firmware.h"

#define IWL_RX_MPDU_DESC_BYTES 64u
#define IWL_RX_FRAME_MAX 4024u
#define IWL_TX_FRAME_MAX 2304u
#define IWL_TX_SLOT_BYTES 4096u
#define IWL_TX_DESCRIPTOR_BYTES 256u
enum IWL_PACKET_RESULT {
    IwlPacketOk=0, IwlPacketMalformed=1, IwlPacketFiltered=2,
    IwlPacketUnsupported=3, IwlPacketCapacity=4
};

typedef struct IWL_RX_FRAME_INFO {
    uint32_t frame_bytes, header_bytes, channel, rate_n_flags;
    int32_t rssi_dbm; /* -256 means neither antenna reported a signal. */
    uint32_t phy_info, device_time, status, deaggregated;
} IWL_RX_FRAME_INFO;
/* Input is one RX_MPDU notification payload, without its 8-byte response
 * header. AX211 uses the 64-byte descriptor. Output starts at the 802.11
 * header and has neither hardware padding nor FCS. Buffers must not overlap.
 * Filtered frames are unrelated/unsupported traffic, not transport failures.
 * All failures clear info and leave the output frame untouched. */
IWL_FW_EXPORT int iwl_rx_frame_decode(const uint8_t *payload, size_t bytes,
    uint8_t *frame, size_t capacity, IWL_RX_FRAME_INFO *info);

typedef struct IWL_TX_FRAME_REQUEST {
    uint32_t queue, index, rate_half_mbps, antenna, no_ack;
    uint32_t command_version, notification_version;
} IWL_TX_FRAME_REQUEST;
typedef struct IWL_TX_FRAME_INFO {
    uint32_t slot_bytes, frame_bytes, header_bytes, byte_count, rate_n_flags;
} IWL_TX_FRAME_INFO;
/* Builds only API-77 TX command v10 / notification v7, using an explicit
 * legacy OFDM rate and one NVM-authorized antenna (1=A, 2=B). Queue 0 is
 * reserved for commands. The caller must allocate/configure the data queue,
 * validate regulatory permission, and supply the Windows common-buffer DMA
 * address. This function never submits DMA or grants permission to transmit.
 *
 * no_ack on unicast data uses the on-air QoS No Ack policy; plain DATA
 * gains a two-byte QoS header (TID 0). Unicast management no_ack is rejected,
 * not silently ignored. Broadcast/multicast inherently require no ACK.
 * No encryption/fragmentation. Actual firmware retry behavior needs RF tests.
 * Buffers must be separate. All failures leave slot and TFD untouched. */
IWL_FW_EXPORT int iwl_tx_frame_build(const IWL_TX_FRAME_REQUEST *request,
    const uint8_t *frame, size_t bytes, uint64_t dma_address,
    void *slot, size_t slot_capacity, void *tfd, size_t tfd_capacity,
    IWL_TX_FRAME_INFO *info);

// Limit the session transport to AWDL action/data frames for its fixed local MAC.
// This validates framing and addresses; Owly validates the AWDL payload itself.
IWL_FW_EXPORT int iwl_awdl_frame_validate(const uint8_t *frame,size_t bytes,
    const uint8_t local_address[6],unsigned transmit);
