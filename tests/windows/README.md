# Receiver protocol tests

These tests use a simulated sender over real IPv6 loopback and TLS on Windows.
They exercise AirDrop request fields, ODC CPIO, stored/compressed DVZip, legacy
gzip, consent, interrupted transfers, path handling and resource limits.
The process tests also launch real child processes to exercise readiness
validation, EOF shutdown, startup failures and receiver cleanup. They do not
open a radio or prove compatibility with an actual iPhone.

Run from the checkout with the Windows dependencies and pytest installed:

    python -m pytest tests/windows -q

The native radio probe is separate:

    clang -O2 -Wall -Wextra -Werror -municode native/radio_probe.c -o radio-probe.exe -lwlanapi -ladvapi32
    .\radio-probe.exe --probe-monitor --output radio-api-probe.json

The monitor-mode test targets only a disconnected AX211 and restores the old
mode immediately if setting the new mode succeeds. Run elevated to distinguish
access-denied (5) from unsupported (50). It does not install a driver.
