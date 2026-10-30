# Third-party notices

This Windows fork retains OpenDrop's GPL-3.0-or-later licensing and original authorship. See [LICENSE](LICENSE), original source headers and [upstream documentation](README-UPSTREAM.md). New Windows components carry GPL-3.0-or-later SPDX identifiers where specified.

| Component | Source / license |
| --- | --- |
| OpenDrop | [seemoo-lab/opendrop](https://github.com/seemoo-lab/opendrop), GPL-3.0-or-later; base 11fe7ba7861093b302bc0637e8cb10adf2d29337 |
| Owly AWDL | [t4t5/owly](https://github.com/t4t5/owly), MIT; pinned revision in Cargo.toml/Cargo.lock; [notice](native/awdl/licenses/owly-MIT.txt) |
| Intel/OpenBSD/Linux reference material | Applicable ISC/BSD notices retained in derived source; [provenance](native/intel/PROVENANCE.md) |
| Intel firmware | External, unmodified download under its accompanying Intel license; not included in this source repository |
| Wintun | External official pinned binary with its own distribution license; not included in this source repository |
| Python/Rust dependencies | Installed separately from requirements/setup.py and Cargo.lock; retain their respective licenses |
| Microsoft SDK/WDK/tools | Installed separately; not redistributed here |

The app packager retains the Intel firmware, Wintun and Owly notices next to a locally built package. Publishing a binary distribution needs a separate review of all bundled runtime/dependency notices. The first public publication is source only; it contains no signing private keys, firmware binaries, vendor-driver backups or received personal files.

AirDrop is an Apple trademark. The name describes interoperability; it does not imply Apple endorsement.
