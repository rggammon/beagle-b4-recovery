# Getting the BeagleBoard B4 on the network under Angstrom (native SGX control)

How to bring up USB networking on the **Angstrom TI‑GNOME v2012.01** image (kernel `3.0.14+`)
used as the native DDK 1.6 SGX control, so we can transfer files without SD/FAT swaps.

Context: classic BeagleBoard has **no onboard Ethernet** — the only port is the OMAP3 **musb OTG**.
This image does not auto‑configure USB, so networking must be brought up by hand. Console is
serial: **COM12 @ 115200 8N1** (no flow control).

---

## musb state on this image (verified)

- `musb-hdrc`, `musb-omap2430`, and the `twl4030_usb` PHY are **built into the kernel** (no `.ko`),
  and all three are **bound** (present under `/sys/bus/platform/devices/` and
  `/sys/bus/platform/drivers/`). The controller + PHY are up.
- The OTG port comes up in mode **`b_idle`** (`cat /sys/bus/platform/devices/musb-hdrc/mode`).
- **No UDC** is registered until a gadget driver is loaded (`/sys/class/udc/` is empty at boot).
- `dmesg` boot messages may be gone if you ran `dmesg -c` (we do that during SGX tests).

Two usable paths: **gadget** (board = USB device to a PC) or **host** (USB NIC/hub into the board).

---

## Option A — Gadget: board as a USB NIC to a PC (point‑to‑point)

Works with just a normal mini‑USB cable from the board's OTG port to the PC.

```sh
insmod /lib/modules/3.0.14+/kernel/drivers/usb/gadget/g_ether.ko
ifconfig usb0 192.168.7.2 netmask 255.255.255.0 up
```

- This creates `usb0` (verified: `HWaddr` assigned, gadget "ready"). musb also registers an OTG
  host root hub (dual‑role) — harmless.
- Plug the board's **OTG mini‑USB into the PC**. On Windows it enumerates as a
  "USB Ethernet/RNDIS Gadget" (may need the inbox RNDIS driver; `g_ether` is CDC‑ECM primary with
  an RNDIS fallback — RNDIS is what Windows binds).
- Set the PC's new adapter to `192.168.7.1 / 255.255.255.0`, then `scp` to the board at
  `192.168.7.2`.
- Other gadget personalities are available if needed: `g_cdc.ko`, `g_ncm.ko`, `g_mass_storage.ko`,
  etc. in `/lib/modules/3.0.14+/kernel/drivers/usb/gadget/`.

---

## Option B — Host: real USB Ethernet NIC into the board (puts board on the LAN)

Plug a USB NIC (via an OTG/host cable, or a self‑powered hub) into the OTG port. Host mode works
(verified: a hub with an ASIX NIC, a flash drive `/dev/sda`, a BT dongle, and an HID all enumerated).
You need a NIC whose driver is present on the image.

### USB NIC drivers on this image

**Built‑in** (from `ls /sys/bus/usb/drivers/`) — these auto‑bind on plug:

| driver                                                                 | chipsets                                                 |
| ---------------------------------------------------------------------- | -------------------------------------------------------- |
| `smsc95xx`                                                             | SMSC/Microchip LAN95xx (**most reliable** — very common) |
| `dm9601`                                                               | Davicom DM9601 (cheap dongles)                           |
| `rtl8150`                                                              | Realtek RTL8150                                          |
| `pegasus`                                                              | ADMtek Pegasus                                           |
| `mcs7830` (MOSCHIP)                                                    | MosChip 7830                                             |
| `cdc_ether`                                                            | any CDC‑ECM NIC                                          |
| `rndis_host`                                                           | any RNDIS NIC                                            |
| `cdc_subset`, `kaweth`, `catc`, `plusb`, `net1080`, `gl620a`, `zaurus` | misc/legacy                                              |
| `asix`                                                                 | ASIX AX88xxx — see caveat below                          |

**Loadable modules** in `/lib/modules/3.0.14+/kernel/drivers/net/usb/`:
`cdc_ncm.ko`, `smsc75xx.ko`, `ipheth.ko`, `sierra_net.ko`, `cx82310_eth.ko`, `int51x1.ko`,
`lg-vl600.ko`, `hso.ko`.

**Recommended:** use an **SMSC95xx**, **DM9601**, **RTL8150**, or **Pegasus** NIC, or any
**CDC‑ECM/RNDIS** dongle — they bind automatically and create `eth0`.

### Bring it up + DHCP

```sh
# plug the NIC, then:
dmesg | tail            # confirm the driver bound and an ethX was created
ls /sys/class/net/      # look for eth0
udhcpc -i eth0          # BusyBox DHCP client (default on this image)
```

### Caveat: ASIX AX88772B (`0b95:7e2b`) does NOT auto‑bind

The built‑in `asix` driver in this 2011 kernel lacks the `7e2b` (AX88772**B**) ID, so it enumerates
but creates no `ethX`. Forcing it did **not** work cleanly:

```sh
# tried; 772B needs re-enumeration / a newer driver — prefer a different NIC
echo '0b95 7e2b' > /sys/bus/usb/drivers/asix/new_id
```

Use a plain AX88772 (`0b95:7720`), or better, an SMSC95xx/DM9601 NIC instead.

---

## Fallbacks if no NIC binds

- **USB flash drive**: works in host mode (enumerates as `/dev/sda`, e.g. `sda1`). Mount and copy —
  a faster sneakernet than the boot SD.
- **FAT boot partition**: `/dev/mmcblk0p1`, not auto‑mounted every boot:
  `mount /dev/mmcblk0p1 /mnt/boot`. Copy files onto the FAT from the PC (SD swap), read at `/mnt/boot`.

---

## Notes

- Serial console COM12 @ 115200 8N1. BusyBox 1.19.3: use `head -n N` / `tail -n N` (not `-N`);
  no `base64`/`xxd`/`strace`/`gdb`.
- To make any of this persist across reboots, add the `insmod`/`ifconfig`/`udhcpc` lines to a boot
  script; by default nothing here survives a reboot.
