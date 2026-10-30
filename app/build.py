# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the native WPF shell using Windows' installed .NET Framework compiler."""
import argparse
import os
import shutil
import subprocess
from pathlib import Path


def build(output):
    source = Path(__file__).resolve().parent
    framework = Path(os.environ["WINDIR"]) / "Microsoft.NET/Framework64/v4.0.30319"
    refs = ["System.dll", "System.Core.dll", "System.Xaml.dll", "System.Web.Extensions.dll",
            "System.Windows.Forms.dll", "System.Drawing.dll", "System.Runtime.dll",
            "System.ObjectModel.dll", "System.Runtime.WindowsRuntime.dll",
            "System.Runtime.InteropServices.WindowsRuntime.dll", "WPF/PresentationCore.dll",
            "WPF/PresentationFramework.dll", "WPF/WindowsBase.dll"]
    sdk = Path(os.environ["ProgramFiles(x86)"]) / "Windows Kits/10/UnionMetadata"
    winmd = max((p for p in sdk.glob("*/Windows.winmd") if p.parent.name[0].isdigit()),
                key=lambda p: tuple(int(n) for n in p.parent.name.split(".")))
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    args = [str(framework / "csc.exe"), "/nologo", "/codepage:65001", "/target:winexe", "/platform:x64",
            "/out:" + str(output / "AirDrop for Windows.exe"),
            "/win32manifest:" + str(source / "app.manifest")]
    if (source / "OpenDrop.ico").exists():
        args.append("/win32icon:" + str(source / "OpenDrop.ico"))
    args += ["/r:" + str(framework / r) for r in refs]
    args += ["/r:" + str(winmd), str(source / "OpenDrop.cs")]
    subprocess.run(args, check=True)
    for file in ("MainWindow.xaml", "OpenDrop.exe.config"):
        shutil.copy2(source / file, output / ("AirDrop for Windows.exe.config" if file == "OpenDrop.exe.config" else file))
    return output

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    build(parser.parse_args().output)
