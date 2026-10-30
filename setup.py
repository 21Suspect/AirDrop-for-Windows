import re
from codecs import open
from os.path import abspath, dirname, join
from setuptools import find_packages, setup

this_dir = abspath(dirname(__file__))
with open(join(this_dir, "opendrop", "__init__.py"), encoding="utf-8") as version_file:
    __version__ = re.search(r'^__version__ = "([^"]+)"', version_file.read(), re.M).group(1)
with open(join(this_dir, "README.md"), encoding="utf-8") as file:
    long_description = file.read()

setup(
    name="opendrop",
    version=__version__,
    python_requires=">=3.10",
    description="AirDrop for Windows experimental AX211 desktop and protocol implementation",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/21Suspect/AirDrop-for-Windows",
    project_urls={
        "Source": "https://github.com/21Suspect/AirDrop-for-Windows",
        "Upstream": "https://github.com/seemoo-lab/opendrop",
        "Research Paper": "https://usenix.org/conference/usenixsecurity19/presentation/stute",
    },
    author="The Open Wireless Link Project",
    author_email="mstute@seemoo.tu-darmstadt.de",
    classifiers=[
        "License :: OSI Approved :: GNU General Public License v3 (GPLv3)",
        "Operating System :: Microsoft :: Windows",
        "Operating System :: MacOS",
        "Operating System :: POSIX :: Linux",
        "Natural Language :: English",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
    ],
    keywords="cli",
    packages=find_packages(exclude=["docs"]),
    package_data={"opendrop": ["certs/*.pem"]},
    install_requires=[
        "cryptography>=46,<51",
        "Pillow; sys_platform != 'win32'",
        "fleep; sys_platform != 'win32'",
        "ifaddr>=0.2.0",
        "libarchive-c; sys_platform != 'win32'",
        "requests; sys_platform != 'win32'",
        "requests_toolbelt; sys_platform != 'win32'",
        "zeroconf>=0.151,<0.152",
    ],
    entry_points={
        "console_scripts": [
            "opendrop=opendrop.cli:main",
            "opendrop-windows=opendrop.windows.cli:main",
        ],
    },
)
