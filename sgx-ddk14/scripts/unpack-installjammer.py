#!/usr/bin/env python3
"""Drive the TI OMAP35x Graphics SDK InstallJammer console installer.

Older SDKs (3.01.00.02) page the EULA and ask "Continue?" twice, which a
pre-buffered pipe cannot align. pexpect answers each prompt as it appears.
Usage: unpack-installjammer.py <installer> <install_dir> <home_dir>
"""
import os
import sys

import pexpect

installer, install_dir, home_dir = sys.argv[1], sys.argv[2], sys.argv[3]
env = dict(os.environ, HOME=home_dir)
child = pexpect.spawn(installer, ["--mode", "console"], env=env,
                      timeout=300, encoding="utf-8")
child.logfile_read = sys.stdout

patterns = [
    r"Continue\?",                                 # 0 initial confirm (x1-2)
    r"Press space to continue or 'q' to quit",     # 1 EULA / post-install pager
    r"Do you agree",                               # 2 license accept
    r"Where do you want to install",               # 3 install directory
    r"Installation complete",                      # 4 done
    pexpect.EOF,
    pexpect.TIMEOUT,
]
while True:
    i = child.expect(patterns)
    if i == 0:
        child.sendline("Y")
    elif i == 1:
        child.send("q")
    elif i == 2:
        child.sendline("Y")
    elif i == 3:
        child.sendline(install_dir)
    elif i == 4:
        break
    else:
        sys.exit("installer ended before completion")
child.expect([pexpect.EOF, pexpect.TIMEOUT])
