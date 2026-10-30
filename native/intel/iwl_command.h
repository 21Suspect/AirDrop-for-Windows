// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iwl_firmware.h"
#define IWL_COMMAND_MAX_PAYLOAD 4088u
#define IWL_TFD_BYTES 256u
// Formats an internal firmware command and its Gen3 transfer descriptor.
// The caller owns one aligned 4096-byte Windows common buffer per command slot.
// It must not recycle a slot before the matching firmware response.
IWL_FW_EXPORT int iwl_command_build(uint8_t opcode, uint8_t group, uint8_t version,
    uint8_t index, uint64_t dma_address, const void *payload, size_t payload_bytes,
    void *command, size_t command_capacity, void *tfd, size_t tfd_capacity);

// Beacon template v13 has a 32-byte fixed header and a separate frame fragment.
// Keep the same three transfer-buffer boundaries used by Linux's DUP fragment.
// Both fragments remain in the caller's one owned 4096-byte common-buffer slot.
IWL_FW_EXPORT int iwl_beacon_command_build(uint8_t index, uint64_t dma_address,
    const void *payload, size_t payload_bytes, void *command,
    size_t command_capacity, void *tfd, size_t tfd_capacity);
