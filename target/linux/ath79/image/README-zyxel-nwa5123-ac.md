# Zyxel NWA5123-AC

Dual-band 802.11ac access point built on Qualcomm Atheros AR9344
(MIPS 74Kc, 560 MHz) with 128 MB DDR2 and two 16 MB SPI NOR flash
chips (Macronix). WLAN: on-chip AR9340 for 2.4 GHz, QCA988X on PCIe
for 5 GHz, GbE via AR8035 PHY.

## Flash layout at a glance

The device ships with Zyxel's proprietary ZLD bootbase (a locked-down
U-Boot 1.1.4 fork) that manages a Zyxel-flavoured dualimage setup:

```
CS0 (nor0, memory-mapped at 0x1F000000):
   0x000000-0x030000  u-boot            (ZLD bootbase, read-only)
   0x030000-0x150000  zy-image0         (OKLI stub, mirrored on first boot)
   0x150000-0xEA0000  firmware2         (OpenWrt kernel + rootfs when ZLD lands here)
   0xEA0000-0xFA0000  zy-conf           (Zyxel's running-config JFFS2)
   0xFA0000-0xFB0000  zy-myZyXEL        (empty slot)
   0xFB0000-0xFC0000  zy-mrd            (manufacturer data, MAC in last 8 bytes)
   0xFC0000-0xFF0000  zy-disklog        (Zyxel kernel ring-buffer)
   0xFF0000-0x1000000 art               (ath9k calibration)

CS1 (nor1, not memory-mapped; read via SPI commands in the OKLI loader):
   0x000000-0x180000  reserve           (Zyxel's built-in 4.22 recovery image)
   0x180000-0x2A0000  zy-image1-loader  (deadbeaf sub_hdr + OKLI uImage)
   0x2A0000-0xFF0000  firmware          (OKLI's kernel uImage + squashfs rootfs)
   0xFF0000-0x1000000 myZyXEL           (empty slot)
```

The MAC address printed on the sticker (eth0) is read from `zy-mrd`
at offset 0xfff8. The 2.4 GHz radio uses that MAC + 1, the 5 GHz
radio uses that MAC + 2.

## How boot works

ZLD can only boot kernels wrapped in Zyxel's "deadbeaf" container
(magic 0xDEADBEAF, 108-byte pre-header + 272-byte section entries +
512-byte sub_hdr, followed by a standard U-Boot uImage). Because CS1
isn't memory-mapped, a plain uImage there is unreadable: ZLD would
try to `bootm 0x9e180000` and get garbage.

The solution is OpenWrt's OKLI (OpenWrt Kernel LZMA Image) loader stub:

1. ZLD boots the deadbeaf-wrapped **OKLI stub** at `0x180200` on CS1
   (or on CS0, see next section). The stub is tiny (~65 KiB padded).
2. The stub runs in RAM, finds the real uImage magic in flash,
   LZMA-decompresses the kernel into RAM at `KERNEL_LOADADDR`, and
   jumps to it.
3. Linux boots from there as usual.

The OKLI customisation lives in
`target/linux/ath79/image/lzma-loader/src/loader.c`
(`CONFIG_FLASH_CS1` for the SPI-based read on CS1,
`CONFIG_FLASH_CS0_FALLBACK` for the memory-mapped fallback on CS0).

## The "which slot does ZLD boot" problem

Zyxel ships NWA5123-AC units with the ex-factory `bootImage` flag at
either 0 or 1, and the flag is sticky — nothing accessible from the
bootbase CLI reliably flips it, and writing to `sub_hdr+0x80` marks
the slot as `Fail` permanently.

Because ATNZ writes to the *inactive* slot:

- A `bootImage=0` unit lands OpenWrt on CS1 (zy-image1-loader).
- A `bootImage=1` unit lands OpenWrt on CS0 (zy-image0).

To make a single factory.bin work for both:

1. **Universal OKLI loader** — the stub probes CS0's memory-mapped
   region first (`LOADER_FLASH_CS0_FALLBACK=0x150000`); if it finds
   the OKLI uImage magic there, it boots from CS0, otherwise it
   falls back to SPI reads on CS1 at `0x2A0000`. One binary covers
   both ex-factory flag values.
2. **Slot-mirror on first boot** — the uci-defaults script
   `99_zyxel-nwa5123-ac-slot-mirror` detects where ZLD actually
   flashed OpenWrt (OKLI magic at offset 0 of `firmware` or
   `firmware2`), clones the OKLI stub into the *other* slot's loader
   partition, and preserves everything beyond the first 1152 KiB.
   After that both slots carry our OKLI stub; whichever slot ZLD
   picks on the next power-on boots OpenWrt.
3. **firmware2 partition on CS0** — the DT exposes `zy-image0`'s
   rootfs area as a second `openwrt,uimage` partition with
   `IH_MAGIC_OKLI`. On a `bootImage=1` unit, mtdsplit finds the OKLI
   magic there and splits off a rootfs. On a `bootImage=0` unit the
   same bytes are Zyxel's stock zldfs and mtdsplit skips the
   partition silently.

End result: after one UART-driven first flash, every subsequent
power-cycle lands directly in OpenWrt, from either flash chip, with
no serial-console intervention.

## Installation

### Via UART + ATNZ (recommended)

Serial console: 3.3 V TTL, 115200 8N1. ZLD prints
"Press any key to enter debug mode within 1 seconds" at power-on;
any key drops into the `NWA5123-AC>` prompt.

```
ATNA 192.168.1.2       # device IP
ATNS 192.168.1.1       # TFTP server IP
ATNZ                   # pulls 'ZLD-current' via TFTP
ATGO                   # boot the freshly flashed slot
```

Serve `openwrt-ath79-generic-zyxel_nwa5123-ac-squashfs-factory.bin`
as `ZLD-current` from TFTP at 192.168.1.1.

On first boot the `99_zyxel-nwa5123-ac-slot-mirror` uci-defaults
script mirrors the OKLI stub into the other slot and deletes itself.
From the next power cycle onward UART is no longer needed.

### Via Zyxel web UI

The stock web UI accepts firmware uploads that run through `ATNZ`
internally. The factory.bin should be accepted since it matches the
deadbeaf format. After upload + reboot, auto-boot works directly
(the slot-mirror runs on first boot the same way).

## Recovery

### OpenWrt bricked, UART available

Catch the bootbase prompt and `ATNZ` any valid deadbeaf image
(OpenWrt factory.bin or Zyxel stock). ATNZ writes to the inactive
slot and resets its status to `New`.

### Back to Zyxel firmware

With the slot-mirror strategy, both loader slots carry the OKLI
stub. To go back to stock, `ATNZ` Zyxel's original `610AAZY8C0.bin`
to overwrite the active slot's loader area, then flash it again
(or flash the stock image into the other slot too via `mtd write`
from within OpenWrt).

### Nothing works any more

Zyxel's internal recovery image sits at `nor1:0x000000` (`reserve`
partition — a Zyxel 4.22 firmware). The trigger for ZLD to actually
boot it is undocumented, but it is there as a safety net. Worst
case: external SPI programmer on the SOIC-8 chip and re-flash
`610AAZY8C0.bin` to CS0 directly.

## Ethernet / RGMII caveat

The AR8035 PHY needs careful delay handling. The combination of
`phy-mode = "rgmii-id"` on `eth0` (MAC-side RX+TX delay) with the
HiveAP-121-style `gmac-config` tuning is what works on this board:

```dts
&eth0 {
    pll-data = <0x06000000 0x00000101 0x00001313>;
    phy-mode = "rgmii-id";
    phy-handle = <&phy0>;

    gmac-config {
        device = <&gmac>;
        rgmii-gmac0 = <1>;
        rxd-delay = <1>;
        rxdv-delay = <1>;
    };
};
```

Using `rxd-delay = <3>` / `txd-delay = <3>` (as some ar9344 boards
do) produces frames that leave the PHY corrupted — the link comes
up at 1000M/Full but TX never reaches the switch (0 input packets
on the trunk port, no MAC learned). Dropping the TX delays and
reducing RX delay to `<1>` resolves it.

## Mesh / batman-adv

5 GHz mesh is not available because the ath10k-CT firmware refuses
802.11s unless loaded with `rawmode=1`, which cripples offload. The
2.4 GHz radio (ath9k) carries the batman-adv backhaul instead; the
5 GHz radio stays dedicated to client APs.

`batman-adv` + `batctl-tiny` are baked into the image
(`CONFIG_PACKAGE_kmod-batman-adv=y`,
`CONFIG_PACKAGE_kmod-lib-crc16=y`, `CONFIG_PACKAGE_batctl-tiny=y`).

Minimal mesh config on each AP (identical `mesh_id` + PSK on all
nodes):

```
config wifi-iface 'mesh_2g'
    option device 'radio1'
    option network 'mesh0'
    option mode 'mesh'
    option mesh_id 'NAME-OF-MESH'
    option mesh_fwding '0'
    option encryption 'sae'
    option key '…'
    option mtu '1536'           # batman OGMs need headroom
```

The debug tables under `/sys/kernel/debug/batman_adv/` are disabled
in the current kernel; `batctl-tiny` has no `originators`/`neighbors`
subcommand either. Verify mesh health by pinging a peer's mgmt IP
across bat0 — a reply confirms peering + OGM exchange + L2 routing
over the mesh.

## 802.11r fast roaming

Add the following to each client-facing `wifi-iface` (not to the
mesh interface) on every AP. The same `mobility_domain` across all
APs is what makes clients recognise them as one roaming domain.

```
option ieee80211r '1'
option ft_over_ds '0'               # over-air FT
option ft_psk_generate_local '1'    # local PMK-R0 holder, no RADIUS needed
option mobility_domain 'c15c'       # 4 hex chars, identical on every AP
option reassociation_deadline '20000'
```

## Known limitations

- **The deadbeaf template is NWA5123-AC-specific.** The 512-byte
  sub_hdr template in
  `tools/firmware-utils/patches/100-add-mkdeadbeaf.patch` was
  extracted verbatim from a stock `610AAZY8C0.bin` and includes
  the device's model_id (0x000526E1) and bootbase pointer values.
  Other Zyxel NWA models would need their own templates.
- **sysupgrade not plumbed end-to-end.** `IMAGE/sysupgrade.bin` is
  produced, but the upgrade path hasn't been exercised in anger.
  For firmware updates, UART + ATNZ remains the reliable path.
- **Zyxel's ZLD version string on flash is cosmetic only.** `ATDS`
  always shows the stock bootbase version; it is not rewritten by
  this port, and ZLD does not consult it at boot time.
- **5 GHz mesh is not supported** on the shipped ath10k-CT firmware
  (rawmode requirement). Mesh must run on the 2.4 GHz ath9k radio.

## Related files

| File | What it does |
|------|--------------|
| `target/linux/ath79/dts/ar9344_zyxel_nwa5123-ac.dts` | DT: dual-chip partition table, `firmware2` on CS0, MAC/calibration nvmem, LEDs, keys, AR8035 RGMII delays |
| `target/linux/ath79/image/generic.mk` (search `zyxel_nwa5123`) | Build recipe: deadbeaf wrapper, universal OKLI loader (CS0 fallback + CS1), factory.bin |
| `target/linux/ath79/image/lzma-loader/src/loader.c` (`CONFIG_FLASH_CS1`, `CONFIG_FLASH_CS0_FALLBACK`) | OKLI stub: probe CS0 memory-mapped, fall back to CS1 SPI |
| `tools/firmware-utils/patches/100-add-mkdeadbeaf.patch` | Host tool that builds the deadbeaf image ZLD will accept |
| `target/linux/ath79/generic/base-files/etc/uci-defaults/99_zyxel-nwa5123-ac-slot-mirror` | One-shot: clones OKLI stub into the other loader slot on first boot |
