#!/usr/bin/env python3
# Set CONFIG_BOOTCOMMAND for the NAND U-Boot: attach UBIFS rootfs, import
# /boot/nand-boot.txt, and run the bootmenu.
import re, sys

path = ".config"
new_cmd = (
    "setenv mtdids nand0=omap2-nand.0; "
    "setenv mtdparts mtdparts=omap2-nand.0:512k(spl),1920k(u-boot),128k(u-boot-env),4m(kernel),-(rootfs); "
    "ubi part rootfs 2048; "
    "ubifsmount ubi0:rootfs; "
    "ubifsload 0x81000000 /boot/nand-boot.txt; "
    "env import -t 0x81000000 ${filesize}; "
    "bootmenu"
)
new_line = 'CONFIG_BOOTCOMMAND="' + new_cmd + '"\n'

s = open(path).read()
s, n = re.subn(r'^CONFIG_BOOTCOMMAND=.*\n', new_line, s, flags=re.M)
if n == 0:
    s += new_line
open(path, "w").write(s)
print("bootcmd set (%d replaced):" % n)
print(new_line, end="")

