// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iwl_firmware.h"
typedef struct IWL_RESPONSE_INFO {
    uint32_t opcode, group, index, queue, packet_bytes, payload_bytes;
} IWL_RESPONSE_INFO;
// AX210-family devices deliver one packet per RX buffer. 0=valid, 1=empty,
// 2=malformed. The packet includes its 4-byte length and 4-byte command header.
IWL_FW_EXPORT int iwl_response_parse(const uint8_t *, size_t, IWL_RESPONSE_INFO *);
typedef struct IWL_NVM_INFO {
    uint32_t version, board_type, address_count, flags, mac_flags;
    uint32_t tx_chains, rx_chains, lar_enabled, channel_count;
    uint32_t channels[110];
} IWL_NVM_INFO;
// Decodes NVM_GET_INFO response payload versions 3 and 4. Does not override
// channel restrictions or infer permission to transmit from an absent flag.
IWL_FW_EXPORT int iwl_nvm_decode(const uint8_t *, size_t, unsigned, IWL_NVM_INFO *);
