// SPDX-License-Identifier: GPL-3.0-or-later
fn main() {
    for path in [
        "../intel/iwl_packet.c",
        "../intel/iwl_packet.h",
        "../intel/iwl_firmware.h",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        for path in [
            "../intel/ax211_transport.c",
            "../intel/ax211_open.inc",
            "../intel/ax211_ioctl.h",
            "../intel/iwl_pnvm.h",
        ] {
            println!("cargo:rerun-if-changed={path}");
        }
        cc::Build::new()
            .file("../intel/ax211_transport.c")
            .warnings(true)
            .warnings_into_errors(true)
            .compile("opendrop_ax211_transport");
        println!("cargo:rustc-link-lib=setupapi");
        println!("cargo:rustc-link-lib=cfgmgr32");
    }
    cc::Build::new()
        .file("../intel/iwl_packet.c")
        .warnings(true)
        .warnings_into_errors(true)
        .compile("opendrop_iwl_packet");
}
