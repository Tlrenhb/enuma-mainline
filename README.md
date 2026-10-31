# enuma-mainline

Minimal, reproducible **mainline Linux probe build** for the **Xiaomi Mi Pad 5 Pro 5G**
(`enuma`, Qualcomm SM8250-AC / Snapdragon 870).

Output is a single boot image meant for `fastboot boot`. **No partition is written**, so
the tablet keeps its stock boot chain and returns to Android on the next reboot.

## What it builds

| Piece | Source |
| --- | --- |
| Kernel | `torvalds/linux` tag `v6.19` (unmodified) |
| Device tree | armbian's `patch/kernel/archive/sm8250-6.19` (65 patches), which carries `sm8250-xiaomi-enuma.dts` by Dawid Wróbel |
| Defconfig | armbian's `config/kernel/linux-sm8250-edge.config` + a small fragment in `mainline/config-fragment` |
| Initramfs | `mainline/init.c`, a static PID1 written for this probe |
| Packer | `mainline/pack-bootimg.py` |

The enuma device tree is upstream-adjacent rather than upstream: `sm8250-xiaomi-elish-common.dtsi`
is in mainline, `sm8250-xiaomi-enuma.dts` still travels as a patch. It is a sister board —
same SoC, single CSOT panel (`xiaomi,enuma-csot-nt36523`, Novatek NT36523), plus PCIe2 for
the external X55M modem.

## What the probe reports

Two independent output channels, so a silent HDMI-style failure still yields evidence:

1. **Panel**: eight colour bands painted into the bottom quarter, plus a white square in the
   top-left corner as an orientation marker. Written straight to `/dev/fb0`, so it works even
   without `fbcon`.
2. **USB CDC-ACM**: the whole kernel ring buffer is forwarded to `/dev/ttyGS0`, i.e. it shows
   up as `/dev/ttyACM0` on the host. The buffer is replayed a few times because the host
   enumerates after the first writes.

The kernel cmdline is compiled in (`CONFIG_CMDLINE_OVERRIDE`) because the stock boot image on
this device has an **empty** cmdline in its header.

## Boot image layout

Read back from the stock `boot.img` and from images already proven to boot on this unit:

| Field | Value |
| --- | --- |
| header version | `1` (1648-byte header, `header_size = 1648`) |
| `kernel_addr` | `0x00008000` |
| `ramdisk_addr` | `0x01000000` |
| `tags_addr` | `0x01e00000` |
| page size | `4096` |
| dtb | appended to the kernel blob, no separate dtb section |

This differs from what postmarketOS uses for the Wi-Fi `elish` (`header_version 0`,
`tags_addr 0x100`). The values above come from the device, not from a sibling port.

## Usage

Download `boot-enuma-mainline.img` from a workflow run, then:

```sh
fastboot boot boot-enuma-mainline.img
```

Expected: kernel log on the panel (if `fbcon` bound) plus bands at the bottom, and
`ttyACM0` on the host carrying the full log.

`panic=30` is in the cmdline so a panic reboots the device instead of leaving it wedged.

## Licensing

`mainline/init.c`, `mainline/pack-bootimg.py` and the workflow are MIT. Kernel sources keep
their own licenses; the resulting binaries are GPL-2.0. The enuma device tree is
`BSD-3-Clause` by Dawid Wróbel, carried through armbian's patch archive.

No firmware and no vendor blobs are included or redistributed here.