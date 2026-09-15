# ZTE MF253S Open-Source Driver Project — Executive Summary

**Goal:** Build open-source Linux tools/drivers so an ordinary PC can drive the LTE modem inside a ZTE MF253S 4G CPE (ZX297510 baseband + RT3352 MIPS AP).

**Final qoder workflow status:** All 8 agents completed. Originally 5 (A4, B1, B2, B3, C1) failed due to credit limits; partial on-disk work was synthesized by 5 parallel sub-agents into structured JSON deliverables. Final consolidated output: `work/final_output.json` (189 KB).

## TL;DR for the user

Build a thin Python+pyusb wrapper around the two already-public BootROM/TBootPro implementations
([rva3/CVE-2026-40003](https://github.com/rva3/CVE-2026-40003) + [sandibabsel/ZX297520-Flasher (zxdl)](https://github.com/sandibabsel/ZX297520-Flasher)),
flash stock firmware over `19d2:0256`, then let mainline drivers take over.

- **Effort:** ~3 person-days
- **Brick risk:** LOW — BootROM is mask-ROM, non-fused, never signature-verified
- **OS reuse score:** 8/10 — both upstream tools already speak the wire byte-exact

## Recommendation matrix

| Phase | Work | Risk |
|-------|------|------|
| 1 | Synthesize spec docs (done) | none |
| 2 | Build pyusb flasher (reuse rva3+ zxdl patterns) + container repacker (PARTMAP.md) + udev rules | none — offline |
| 3 | First device touch: `--probe` (1-byte read) → full flash → power-cycle + verify | LOW — recoverable |
| 4 | Post-flash: `AT+ZMODESWITCH` + `AT+ZNCARD=1` → 19d2:0199 (CDC-ACM + RNDIS) | LOW |
| 5 | Mainline `option` + `rndis_host` + ModemManager auto-detect | none |

## Key facts established

- **Device currently exposed as bare USB 19d2:0256** (modem SoC pulled out of CPE, plugged in directly via mini-PCIe/USB).
- **Stock SalesDL is NOT a USB flasher** — it's a TCP/Ethernet tool (PC listens TCP:4536, CPE connects out from 192.168.0.1/2.2.2.2). Inner ZX297510 BootROM protocol (0x5A/0x7A/0x31/0x14/0x8A + 1-byte ACKs + 2048/8192-byte packets) was fully recovered but only matters when CPE is assembled.
- **Normal-mode USB composition (after AT+ZMODESWITCH)**: 2× CDC-ACM (`usbsio/0`, `usbsio/1`) + RNDIS (`usbRndis/1`) at PID 19d2:0199 (most-likely).
- **pscpu default runtime PID = 19d2:0197** "ZTE Trap" (diag).
- **BootROM has no signing** — `xuke.dat` Windows license is PC-only, can be skipped.
- **Container is well-documented**: 25 records, byte-exact format spec at `work/container/PARTMAP.md`.
- **AP filesystem**: Ralink RT3352 + Linux 2.6.21 + squashfs v3 (non-standard magic `shsq`, extracted with sasquatch) + cramfs web UI.
- **Hardware**: MF253S = RT3352 AP + W25Q128 16 MiB SPI NOR + UART 57600 8N1 3.3 V + removable mini-PCIe ZX297510 modem.

## Deliverables (specs already on disk)

| File | Purpose |
|------|---------|
| `work/container/PARTMAP.md` | Container byte-exact format spec (25 records + boot region + partition table) |
| `work/salesdl/PROTOCOL.md` | TCP/Ethernet flasher protocol (A3 disassembly) |
| `work/salesdl/verify/FLASHER_SPEC.md` | Open-source flasher spec, USB + Ethernet paths |
| `work/ap/deep/TOPOLOGY.md` | AP↔modem internal bus + PC-facing composition |
| `work/modem/MODEM_USB.md` | Modem-side USB stack, PIDs, mode transitions |
| `work/research/FINAL.md` | Community/web findings + reuse candidates |
| `work/DESIGN.md` | Solution architecture + 4 options + phased plan |
| `work/final_output.json` | All 8 agent JSONs consolidated |

## Code deliverables to be built (Phase 2)

- `bflasher.py` — Python+pyusb, direct USB 19d2:0256 flasher (reuse rva3 + zxdl)
- `bflasher_net.py` — TCP flasher for assembled CPE (reuse A3 protocol)
- `container_unpack.py` / `container_repack.py` — based on PARTMAP.md
- `ap_repack.sh` — squashfs+mkimage wrapper for root_uImage rebuild
- `udev/99-mf253s.rules` — for 19d2:0256 + 19d2:0199 + 19d2:0197
- `docs/PROTOCOL.md`, `docs/FLASHER_SPEC.md`, `docs/TOPOLOGY.md`, `docs/MODEM_USB.md`, `docs/CONTAINER.md`, `docs/INSTALL.md`

## What still needs the device (6 unknowns)

1. Exact post-`AT+ZMODESWITCH` PID (0199 candidate) — `lsusb -v -d 19d2:` after flash
2. Bare-USB `0x5A → 0xA5` sync — 1-byte `--probe`
3. Whether tboot re-enumerates to new PID — observe `dmesg -w` during flash
4. `AT+ZNCARD=1` behavior (network-card mode toggle) — AT-only, no flash
5. 16-byte container key purpose — irrelevant if reusing stock container
6. Partition-table checksum algorithm — safest = reuse stock 4 KiB block verbatim

All 6 can be resolved safely without bricking: AT commands are non-destructive; `--probe` reads 1 byte; `lsusb` is passive.