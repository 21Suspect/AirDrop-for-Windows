// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <guiddef.h>
#ifndef _KERNEL_MODE
#include <winioctl.h>
#endif
#define AX211_ABI_VERSION 1u
#define IOCTL_AX211_IDENTITY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)
DEFINE_GUID(GUID_DEVINTERFACE_OPENDROP_AX211,
    0xccbe5be2, 0x6283, 0x45c0, 0x8b, 0x91, 0xfc, 0x7f, 0x44, 0x54, 0x3c, 0xa7);
typedef struct AX211_IDENTITY {
    ULONG size, abi_version, bar_length, hw_revision, rf_id, reserved[3];
} AX211_IDENTITY;

#define IOCTL_AX211_STAGE_FIRMWARE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
typedef struct AX211_FIRMWARE_RESULT {
    ULONG size, abi_version, firmware_major, firmware_minor;
    ULONG lmac_sections, umac_sections, paging_sections, image_loader_bytes;
    ULONG buffer_count, staged_bytes, source_crc32, staged_crc32;
} AX211_FIRMWARE_RESULT;

#define IOCTL_AX211_POWER_CYCLE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_AX211_BOOT_FIRMWARE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x904, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
typedef struct AX211_BOOT_RESULT {
    ULONG size, version, error, kernel_status, stage, start_error, stop_error;
    ULONG allocated_buffers, allocated_bytes, dma_started, hardware_alive, firmware_alive;
    ULONG interrupts, msix_causes, fh_causes, boot_mirror, iml_response;
    ULONG rx_head, rx_packets, last_packet_code, alive_status, alive_payload_bytes;
    ULONG lmac_major, lmac_minor, umac_major, umac_minor, sku[3];
    ULONG buffers_released, function_reset_attempted, function_reset_status;
    ULONG gp_control, reset_status, elapsed_us;
    ULONG irq_resources, irq_connected, irq_vector, irq_hits, pci_command, pci_msix;
    ULONG rx_dma_config, rx_dma_status, iml_before;
} AX211_BOOT_RESULT;

#define IOCTL_AX211_INIT_FIRMWARE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x905, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define AX211_INIT_MAGIC 0x314d564eu
typedef struct AX211_INIT_INPUT {
    ULONG magic, version, firmware_bytes, pnvm_bytes;
} AX211_INIT_INPUT;
typedef struct AX211_INIT_RESULT {
    AX211_BOOT_RESULT boot;
    ULONG pnvm_version, pnvm_bytes, pnvm_complete, pnvm_status;
    ULONG commands_sent, init_complete, nvm_valid;
    ULONG nvm_version, nvm_flags, mac_flags, tx_chains, rx_chains;
    ULONG lar_enabled, channel_count, channel6_flags;
} AX211_INIT_RESULT;

// Fixed, read-only startup diagnostics; no arbitrary register/config access.
#define IOCTL_AX211_IRQ_DIAGNOSTICS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x906, METHOD_BUFFERED, FILE_READ_ACCESS)
typedef struct AX211_IRQ_RESOURCE {
    ULONG index, raw_type, raw_flags, translated_flags, message_count, vector;
} AX211_IRQ_RESOURCE;
typedef struct AX211_IRQ_DIAGNOSTICS {
    ULONG size, version, setup_status, raw_count, translated_count;
    ULONG resource_count, objects_created, connected, pci_status, pci_command, pci_msix;
    AX211_IRQ_RESOURCE resources[32];
} AX211_IRQ_DIAGNOSTICS;

#define IOCTL_AX211_MONITOR_TEST CTL_CODE(FILE_DEVICE_UNKNOWN, 0x907, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
typedef struct AX211_RADIO_RESULT {
    AX211_INIT_RESULT init;
    ULONG size, version, configured, command_id, command_status, command_response_bytes;
    ULONG mcc, mcc_status, channel6_flags, channel, duration_us;
    ULONG rx_notifications, valid_frames, beacons, action_frames, data_frames;
    ULONG filtered_frames, malformed_frames, wrong_channel_frames;
    ULONG tx_requested, tx_queue, tx_index, tx_submitted, tx_completed, tx_status;
    ULONG tx_response_bytes, tx_next_index, tx_frame_bytes, tx_reported_bytes, tx_rate, tx_retries;
} AX211_RADIO_RESULT;

#define IOCTL_AX211_TRANSMIT_TEST CTL_CODE(FILE_DEVICE_UNKNOWN, 0x908, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Version 1 continuous channel-6 session. One TX DMA slot is owned by firmware
// until its completion; each exchange returns at most eight normalized MPDUs.
#define IOCTL_AX211_SESSION_START CTL_CODE(FILE_DEVICE_UNKNOWN, 0x909, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_AX211_SESSION_STOP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x90a, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_AX211_SESSION_EXCHANGE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x90b, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define AX211_SESSION_VERSION 2u
#define AX211_SESSION_LEASE_MS 10000u
#define AX211_SESSION_TX_MAX 2304u
#define AX211_SESSION_RX_MAX 4024u
#define AX211_SESSION_RX_COUNT 8u
enum AX211_SESSION_STOP_REASON {
    Ax211SessionRunning=0, Ax211SessionExplicit=1, Ax211SessionFileClosed=2,
    Ax211SessionLeaseExpired=3, Ax211SessionPowerDown=4, Ax211SessionFault=5
};
typedef struct AX211_SESSION_STATUS {
    ULONG size, version, active, kernel_status, stop_reason, buffers_released;
    ULONG lease_ms, uptime_ms, exchanges, tx_pending, tx_submitted, tx_completed;
    ULONG tx_failed, last_tx_status, next_tx_index, rx_valid, rx_awdl, rx_dropped;
    ULONG ibss_configured, beacons_completed, last_beacon_status;
} AX211_SESSION_STATUS;
typedef struct AX211_SESSION_INPUT {
    ULONG size, version, tx_bytes, rate_half_mbps, no_ack;
    UCHAR frame[AX211_SESSION_TX_MAX];
} AX211_SESSION_INPUT;
typedef struct AX211_SESSION_FRAME {
    ULONG bytes;
    LONG rssi_dbm;
    UCHAR frame[AX211_SESSION_RX_MAX];
} AX211_SESSION_FRAME;
typedef struct AX211_SESSION_OUTPUT {
    AX211_SESSION_STATUS status;
    ULONG tx_accepted, rx_count;
    AX211_SESSION_FRAME received[AX211_SESSION_RX_COUNT];
} AX211_SESSION_OUTPUT;

#define IOCTL_AX211_IBSS_START CTL_CODE(FILE_DEVICE_UNKNOWN, 0x90c, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Read-only copy of firmware error tables captured at the first hardware fault.
// Addresses originate only in the ALIVE notification; callers provide no address.
#define IOCTL_AX211_FAULT_DIAGNOSTICS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x90d, METHOD_BUFFERED, FILE_READ_ACCESS)
typedef struct AX211_FAULT_DIAGNOSTICS {
    ULONG size, version, observed, read_mask, lmac_address, umac_address;
    ULONG lmac[38], umac[15];
} AX211_FAULT_DIAGNOSTICS;
