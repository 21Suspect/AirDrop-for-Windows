# Intel driver development sources

The native Intel prototypes in this directory are new GPL-3.0-or-later source.
They provide a restricted development AirDrop radio path on the tested AX211,
not a general Windows Wi-Fi driver. Real iPhone transfers have succeeded in both
directions; see ../../WINDOWS-STATUS.md for evidence and remaining limitations.

Hardware identity offsets (0x28, 0x9c), SO/GF device identification and the
published firmware container layout were checked against OpenBSD iwx at commit
`65596250e06438442ce07bad87e5ba129229607d`:

- [Register and firmware format declarations](https://github.com/openbsd/src/blob/65596250e06438442ce07bad87e5ba129229607d/sys/dev/pci/if_iwxreg.h)
- [Driver and firmware selection](https://github.com/openbsd/src/blob/65596250e06438442ce07bad87e5ba129229607d/sys/dev/pci/if_iwx.c)

These sources inform the bounded controller power sequence, Gen3 startup
layout, firmware loader, PNVM/NVM initialization test, command/response formats
and packet codec. Derived files retain the OpenBSD ISC and Intel BSD notices.
The BSD option is used for dual-licensed Intel source. The parser reads the
published container; it does not disassemble or modify Intel firmware.
Register access, common-buffer staging, controller power cycles, firmware ALIVE
and PNVM/NVM initialization have passed on hardware. Bounded monitor and continuous sessions received real Wi-Fi frames and firmware
TX completions. Later IBSS tests and the desktop app completed real iPhone transfers in both directions. The fixed monitor command layouts follow the same pinned reference.

The user-mode PCI approach was informed by
[winawdl](https://github.com/raph559/winawdl/blob/main/DESIGN-NOTES.md).
Its Realtek radio implementation was not reused as an Intel implementation.
The kernel prototype obtains common buffers through KMDF, using Windows' DMA
adapter. The bounded firmware startup code programs only Windows-provided
common-buffer addresses and retains buffers until DMA shutdown is confirmed;
the startup and initialization tests confirmed shutdown on this PC.

## External firmware

The measured hardware revision 0x430 (SOF) and RF identity 0x2010d000 (GF) match
OpenBSD's `iwx_2ax_cfg_so_gf_a0` selection. The development image is
`iwx-so-a0-gf-a0-77`, version 77.f92b5fed.0. Its container has 15 LMAC sections,
16 UMAC sections, 24 paging sections and a 13,944-byte image loader.

Source package:
[iwx-firmware-20251021p0.tgz](https://firmware.openbsd.org/firmware/snapshots/iwx-firmware-20251021p0.tgz).

The package's SHA256 is
`9f0458f1198c7f8cc3e0a3974580e1d9b63726a2e1f8c15e24865bf011a67978`.
The downloader verifies the Ed25519 signature in OpenBSD's `SHA256.sig`, then
its package checksum and these individual SHA256 pins:

| File | SHA256 |
| --- | --- |
| iwx-so-a0-gf-a0-77 | 87590ea9535a8939fddb296bc7622f3a900f28f47b45cb4e435a2a1d5f496ccd |
| iwx-so-a0-gf-a0.pnvm | b37f038b3467ed568d107a9c1dfd916fa7e8cbd58b2f9bec0c2c53297ccfcab7 |
| iwx-license | 16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5 |

The pinned key is OpenBSD's
[8.0 firmware public key](https://github.com/openbsd/src/blob/65596250e06438442ce07bad87e5ba129229607d/etc/signify/openbsd-80-fw.pub),
Git blob `d8a2f02d074effef34b01399c1d4c3b98d14be3a`.

Firmware binaries remain separate from this GPL source. The downloader retains
Intel's accompanying license and only writes unmodified, verified files.
Downloading or parsing firmware does not demonstrate hardware compatibility.

## Microsoft interfaces and tools

- [Mapping resources](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/finding-and-mapping-hardware-resources)
- [DMA common buffers](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-common-buffers)
- [Driver signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-)
- [Test-signing boot option](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)

Build tools: Microsoft Visual Studio 2022 Build Tools, MSVC 14.44.35207,
Windows SDK 10.0.26100.0, WDK NuGet 10.0.26100.6584. The WDK package was
verified against NuGet's published SHA512:
jhddaBnhMDqt3GVr32RVTtaR0KHmZDjY0JCTMn10OQpk4+AShXCK9QA09vBazp8ni1Mju84qM5SGXfIfnyOJ+g==.
No Microsoft tooling or Intel driver backups are included in the repository.

## Additional packet and IBSS references

Linux iwlwifi, pinned to v6.1 commit
830b3c68c1fb1e9176028d02ef86f3cf76aa2476, supplies additional published reference
layouts and behavior under its GPL-2.0 OR BSD-3-Clause license (BSD option used):

- [Gen3 transmit commands](https://github.com/torvalds/linux/blob/830b3c68c1fb1e9176028d02ef86f3cf76aa2476/drivers/net/wireless/intel/iwlwifi/fw/api/tx.h)
- [Receive descriptors](https://github.com/torvalds/linux/blob/830b3c68c1fb1e9176028d02ef86f3cf76aa2476/drivers/net/wireless/intel/iwlwifi/fw/api/rx.h)
- [Transmit setup](https://github.com/torvalds/linux/blob/830b3c68c1fb1e9176028d02ef86f3cf76aa2476/drivers/net/wireless/intel/iwlwifi/mvm/tx.c)
- [RX padding, FCS and aggregation](https://github.com/torvalds/linux/blob/830b3c68c1fb1e9176028d02ef86f3cf76aa2476/drivers/net/wireless/intel/iwlwifi/mvm/rxmq.c)
- [IBSS MAC context](https://github.com/torvalds/linux/blob/830b3c68c1fb1e9176028d02ef86f3cf76aa2476/drivers/net/wireless/intel/iwlwifi/mvm/mac-ctxt.c)

The codec strips the descriptor-reported FCS and two-byte padding for
unencrypted frames. It clears A-MSDU control on hardware-deaggregated frames.
AX211 does not use the older 9000-family address-reversal workaround.
Gen3 TX notification v7 selects rate encoding v2. TX frames retain the AWDL
sequence number; the packet codec does not own queue allocation or completion.


The MSI-X integration uses Windows resource descriptors and read-only PCI
configuration access; Windows owns the PCI header and MSI-X table:

- [MSI installation declaration](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/enabling-message-signaled-interrupts-in-the-registry)
- [Interrupt resource descriptors](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-interrupt-resource-descriptors)
- [KMDF interrupt creation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfinterrupt/nf-wdfinterrupt-wdfinterruptcreate)
- [PCI configuration ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/pci/accessing-pci-device-configuration-space)

Bluetooth coexistence uses network arbitration and the MPLUT/SYNC2SCO/high-band
modules from [Linux coex.c](https://github.com/torvalds/linux/blob/830b3c68c1fb1e9176028d02ef86f3cf76aa2476/drivers/net/wireless/intel/iwlwifi/mvm/coex.c).
The source's Intel copyright notices and BSD option are retained in iwl_radio.c.

## IBSS implementation and Gen3 queue wrap

The new IBSS beacon/MAC/station command builders use the published Linux layouts
under the BSD-3-Clause option; the additional Intel copyright notices remain in
iwl_radio.c. They configure HT20, a single IBSS MAC with passive monitor
filtering, and fixed internal management/data stations. This is new Windows transport code, not a Linux driver binary.

- [IBSS startup order, v6.1](https://github.com/torvalds/linux/blob/830b3c68c1fb1e9176028d02ef86f3cf76aa2476/drivers/net/wireless/intel/iwlwifi/mvm/mac80211.c)
- [Internal station and queue setup, v6.1](https://github.com/torvalds/linux/blob/830b3c68c1fb1e9176028d02ef86f3cf76aa2476/drivers/net/wireless/intel/iwlwifi/mvm/sta.c)
- [Station command layouts, v6.1](https://github.com/torvalds/linux/blob/830b3c68c1fb1e9176028d02ef86f3cf76aa2476/drivers/net/wireless/intel/iwlwifi/fw/api/sta.h)
- [Beacon command v13 layout, v6.6](https://github.com/torvalds/linux/blob/ffc253263a1375a65fa6c9f62a893e9767fbebfa/drivers/net/wireless/intel/iwlwifi/fw/api/tx.h)
- [Beacon v13 link ID and rate selection, v6.6](https://github.com/torvalds/linux/blob/ffc253263a1375a65fa6c9f62a893e9767fbebfa/drivers/net/wireless/intel/iwlwifi/mvm/mac-ctxt.c)

Gen3 hardware queue positions use 16 bits; storage positions use the allocated
256-slot ring. This distinction was checked against the pinned OpenBSD iwx
tx path and the Linux v6.1 pcie/tx-gen2.c and pcie/tx.c references, then verified
by 1,024 successful hardware completions. The corrected IBSS startup passed
beacon/RX/TX checks and a later native iPhone file transfer.

Owly's monitor plus HT-capable IBSS configuration supplies the integration
rationale:
[pinned setup.rs](https://github.com/t4t5/owly/blob/9a676e11d011c34ea32798a2541f2b1eeb66e731/crates/owly-radio/src/injection/setup.rs).
Its MIT notice is retained in ../awdl/licenses/owly-MIT.txt.


Passive-monitor lifecycle was checked against [mac80211 iface.c at v6.6](https://github.com/torvalds/linux/blob/ffc253263a1375a65fa6c9f62a893e9767fbebfa/net/mac80211/iface.c)
as behavioral documentation. When an IBSS is already open, a passive monitor
does not add another firmware interface. The command DMA boundaries follow
the BSD-licensed iwlwifi Gen3 command transport (20-byte first TB, remainder
of the fixed command, then the beacon frame). Firmware v13 required its MAC
context before the beacon on the tested API-77 device.
