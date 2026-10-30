# SPDX-License-Identifier: GPL-3.0-or-later
import sys
from pathlib import Path

if not getattr(sys, "frozen", False):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from opendrop.windows.app_backend import main

if __name__ == "__main__":
    main()
