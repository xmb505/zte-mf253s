# ZTE MF253S — Open-source Driver / Flasher Implementation Plan (Design C1)

**Subject:** Bare `19d2:0256` ZTE ZX297510 modem-SoC pulled out of the CPE, currently exposed
directly on the user's Linux PC over USB. Goal: open-source tools that recover the device,
then drive it as a regular LTE modem.

---

## Headline

**The work is mostly synthesis.** Every protocol byte the flasher needs already exists in
public open-source form (rva3/CVE-2026-40003 for BootROM stage 1+2, sandibabsel/ZX297520-
Flasher aka "zxdl" for TBootPro ASCII stage 3). The remaining engineering is a Python+
pyusb wrapper, a container repacker, a udev rule, and a Linux userland driver stack that
already exists in the mainline kernel. **Brick risk for the user's *current* state is LOW**
— the BootROM is mask-ROM and non-fused, so any failure is recoverable by power-cycle +
BOOT-pad short. Phase 1+2 are zero-risk offline plus a single 1-byte `--probe`.

**Five-bullet TL;DR**
- **Recommended option:** A (open-source USB flasher over `19d2:0256`, reusing rva3/zxdl
  patterns); feasibility **5/5**, effort **3 person-days**.
- **Key risks:** wrong container → wrong IMEI → permanent SIM lock (HIGH); mid-flash power
  loss → half-written partition (MEDIUM); tboot re-enumeration ambiguity (LOW).
- **Brick verdict:** **Recoverable**. BootROM non-fused, no one-shot flag; power-cycle +
  BOOT-pad short always recovers.
- **Open-source reuse score:** **HIGH (8/10)**. Two complete public implementations cover
  every wire byte; mainline `option`+`cdc_acm`+`rndis_host` cover normal-mode (ZTE
  vendor-wide RNDIS quirk already upstream). Net new code ≈ 2000 lines.
- **Unknowns needing device (count 6):** exact post-`AT+ZMODESWITCH` PID (most-likely 0199);
  bare-USB `0x5A→0xA5` sync (1-byte `--probe`); tboot re-enumeration PID; AT+ZNCARD=1
  behavior on this firmware; 16-byte container key purpose; partition-table checksum.

**One-line TL;DR:** Phase 1 is glue around rva3+zxdl, Phase 2 is one safe `--probe` byte,
Phase 3 is recovery-to-19d2:0199, Phase 4 is release polish — and the whole path is LOW-
risk because the BootROM is non-fused.

## 1. Executive brief

The ZTE MF253S is a two-board product: a MIPS router AP (Ralink RT3352) and a removable
mini-PCIe 4G module (ZTE Microelectronics ZX297510 / "WF7510"). The user has the modem
pulled out of the CPE and plugged directly into a Linux PC, where it currently enumerates
as bare `VID 19d2 PID 0256` — ZTE BootLoader composition. This is exactly the entry state
the Windows-only "Downloader 7510 V2.0B01" (rebranded as `SalesDL_LTE_MF253SV1.00.00.exe`)
expects, so the path forward is to re-implement that tool on Linux.

Upstream work (A1–B3) produced a byte-exact picture of every layer:

- **Container** (`MF253SV1.0.0B10.bin`, 27,252,890 B): flat `0xD0`-byte header + 25 ×
  128-byte record table + boot-region + contiguous payload region (every byte position in
  `work/container/PARTMAP.md`). The 16-byte key at offset 0 is hardcoded in the Windows
  flasher's `.rdata` but **never sent to the device** — safe to reuse verbatim.
- **BootROM binary protocol** (0x5A sync / 0x7A+BE32+BE32 / 0x2000-B chunks / 0xA1/0xA7
  ACKs / 0x8A+BE32 / 0xA8 ACK) is byte-for-byte the same as `rva3/CVE-2026-40003` (AGPLv3).
  MF253S deviations: BOOT1 chunk = **0x800**, BOOT1 base = **0x00080000** (proven by
  `Logs/log.log`: 6 packets of 2048 B for the 10532-B tloader.bin).
- **TBootPro ASCII stage** (sync 0x5A → 0xA7; "set partitions" / "compat_write" /
  "compat_read" / "erase" / "reboot"; CRC32 init 0xFFFFFFFF poly 0xEDB88320 **NO final
  inversion**) is byte-for-byte the same as `sandibabsel/ZX297520-Flasher` aka "zxdl"
  (MIT).
- **`xuke.dat`** is a PC-side Windows license. **No device-side authentication anywhere in
  the protocol.** Skip on Linux.
- **AP rootfs** is squashfs v3 with non-standard `shsq` magic, extracted by `sasquatch`;
  the web partition is a standard cramfs (extracted by 7z).
- **Runtime USB composition** (pscpu default `19d2:0197` "ZTE Trap"; post-`AT+ZMODESWITCH`
  most-likely `19d2:0199` after `AT+ZNCARD=1`) is **2× CDC-ACM + RNDIS** (`usbsio/0`,
  `usbsio/1`, `usbRndis/1`). Mainline `option` + `cdc_acm` + `rndis_host` already cover
  this — the vendor-wide ZTE RNDIS quirk is in mainline kernel.
- **CPE external USB-A** is a host socket — no path exposes the modem to a PC in normal
  boot. Once the bare modem is flashed with stock firmware, it re-enumerates as the
  normal-mode composition and a Linux PC drives it via the standard driver stack.

---

## 2. Known architecture

The CPE is AP-as-USB-host (Ralink RT3352, MIPS 24KEc, Linux 2.6.21) with **NO gadget stack**
(verified: zero `gadget/g_ether/rndis_gadget/cdc_ether_gadget/android/composite` strings
in `vmlinux.bin`); the modem SoC (ZX297510, three sub-CPUs: pscpu Enea-RTOS, phycpu+zsp+
m0, proc-app ARM Linux-3.4.5) is wired to the AP's internal EHCI/OHCI root hub and is
invisible from the external USB-A socket. The user's "bare USB" configuration pulls the
modem out of the CPE and exposes its device-side USB controller directly to the PC.
Boot-time USB composition is **19d2:0256** ("ZTE BootLoader", 2 vendor bulk interfaces
0x01/0x81 and 0x02/0x82) — exact match to rva3/CVE-2026-40003 and `zxdl`'s expected
device. After BOOT1 (tloader.bin → RAM 0x00080000) and BOOT2 (tboot uImage → RAM
0x27EF0000), pscpu takes over and re-enumerates as **19d2:0197** ("ZTE Trap") by default;
after `AT+ZMODESWITCH=<mode>` the Synopsys DWC2 descriptor-builder dynamically rebuilds
the composition to **2× CDC-ACM + RNDIS** (most likely PID **19d2:0199** after
`AT+ZNCARD=1`, matching the ME3760 public Linux-porting writeup and mainline kernel's
`option.c`+`qmi_wwan.c` ZTE-MF820S entries). Container: flat WF7510-specific 0xD0-byte
header + 25 × 128-byte record table (filename[64]/partition[16]/medium[16] `nand|zftl|
ddr|raw`/size u32/target_offset u32/container_offset u32/reserved[16]) + boot region
(tloader.bin 10532 B at 0xD50, tboot uImage 328684 B at 0x3674, partition.bin 4096 B at
0x53A60 — header fields at 0x64/0x68/0x6C/0x70/0x74 point to all three) + 26,906,170 B
payload region. AP rootfs: squashfs v3 with non-standard `shsq` magic; web partition:
standard cramfs.

**PROVEN**: every header offset, every record entry, every container byte-exact match against
binwalk + manual analysis; A3 disassembly of `SalesDL_LTE_MF253SV1.00.00.exe` (every opcode,
every frame format, every string at known VA); rva3 `loader/src/main.rs` (every BootROM
byte); zxdl `proto.c` (every TBootPro string); captured `Logs/log.log` (486 lines matching
the recovered state machines line-for-line).

**UNKNOWN**: exact post-`AT+ZMODESWITCH` PID (most-likely 0199); tboot re-enumeration PID
after BOOT2 jump; the 16-byte container key's exact purpose (best hypothesis: package
key, safe to reuse); the partition-table checksum algorithm (stored 0xE9DDF3D5 — XOR
matches zxdl's `zx_xorsum32`); `AT+ZMODESWITCH`/`AT+ZNCARD` exact behavior.

## 3. Solution options

### (A) Open-source Linux flasher over USB `19d2:0256` — RECOMMENDED

Re-implement the ZX297510 download protocol as a Python+pyusb tool (`bflasher.py`),
copying protocol constants from `rva3/CVE-2026-40003/loader/src/main.rs` for stages 1+2
and `sandibabsel/ZX297520-Flasher/src/proto.c` for stage 3. Add container
parser/repacker (`container_unpack.py`/`container_repack.py`) per `PARTMAP.md`; add
`udev/99-mf253s.rules` matching `99-zxdl.rules` for the bare `19d2:0256`. After flashing,
standard mainline `option`+`cdc_acm`+`rndis_host` take over.

* **Feasibility: 5/5** — two public impls cover every byte; user's bare-USB state is
  exactly the entry condition.
* **Effort:** **3 person-days** (1d skeleton+probe+BOOT1, 1d BOOT2+TBootPro, 1d repacker+udev+docs).
* **Risks:** wrong-container flash (HIGH) → SHA-256 whitelist + refuse-on-unknown;
  mid-flash power loss (MEDIUM) → read-back verify + UPS; 0x5A unresponsive on bare
  USB (LOW) → BOOT-pad short; tboot re-enumerates to CDC-ACM (LOW) → re-probe after BOOT2.

### (B) Open-source Linux flasher over Ethernet (SalesDL re-implementation)

Reimplement the full TCP/Ethernet flow (`bflasher_net.py`): arp-clear → ICMP ping
2.2.2.2 → HTTP `/goform/goform_process?SwitchFactoryMode` → TCP connect 2.2.2.2:3535 →
PC listen TCP:4536 (≤8 channels, '@' hello, seq=1) → framed 'A' 61530-byte bulk /
'C' 1392-byte control carrying the same inner payload protocol as Path A. For assembled
CPE only.

* **Feasibility: 4/5** — every byte recovered; Python socket layer is straightforward.
* **Effort:** **2 person-days** additional (TCP plumbing; inner protocol reuses A).
* **Risks:** CPE must be assembled and AP must boot (**BLOCKER** for current state);
  CPE-side bridge daemon undocumented beyond `remserial -p 10005`; carrier variants may
  change ports.

### (C) Modified AP firmware via repacker

Patch extracted AP squashfs to start telnetd, force modem USB composition to expose
on external USB-A (requires adding gadget support — currently ABSENT), or modify the AP
firewall. Re-flash via Path A or SalesDL.

* **Feasibility: 2/5** — squashfs repacker known; gadget stack requires new kernel build; AP
  kernel has no embedded config (no `IKCFG_ST`).
* **Effort:** **14+ person-days**.
* **Risks:** AP-side brick (HIGH; CH341 SPI programmer + W25Q128 clip recovery); AP/modem
  firmware drift; SPI layout sensitive to byte boundaries.

### (D) Custom OpenWrt-style for MIPS AP

Build OpenWrt for the RT3352-based MF253S AP using `rt3352_zte_mf283plus.dts` (same SoC,
same mPCIe modem). Configure `kmod-usb-net-qmi-wwan` + `kmod-usb-serial-option` +
ModemManager. Recovery: TFTP from u-boot to `root_uImage` on 192.168.0.1/22.

* **Feasibility: 3/5** — DTS small; build + integrate + LTE attach test is medium-effort.
* **Effort:** **7–10 person-days**.
* **Risks:** AP-side brick (HIGH; wrong DTS → SPI programmer required); MAC/IMEI persistence;
  SPI differs from MF283+ (16 MB vs 32 MB); operator customization lock.

**Scorecard:**

| Option | Feasibility | Effort | Brick risk | OS reuse |
|---|---|---|---|---|
| **A (USB flasher)** | 5/5 | 3 pd | LOW | HIGH |
| **B (Ethernet)** | 4/5 | 2 pd | LOW | HIGH |
| **C (Patched AP)** | 2/5 | 14+ pd | HIGH | MEDIUM |
| **D (OpenWrt)** | 3/5 | 7–10 pd | HIGH | MEDIUM |

**Plan: A first (the user's blocker), B second (assembled-CPE follow-up), C+D deferred
to v2.**

---

## 4. Recommended plan (phased)

### Phase 1 — Zero-risk offline + tool skeleton

1. **Lock the spec set.** Consolidate `work/salesdl/PROTOCOL.md` +
   `work/salesdl/verify/FLASHER_SPEC.md` + `work/container/PARTMAP.md` +
   `work/ap/deep/TOPOLOGY.md` + `work/modem/MODEM_USB.md` into
   `docs/{PROTOCOL,FLASHER_SPEC,TOPOLOGY,MODEM_USB,CONTAINER}.md`.
2. **Write `bflasher.py` skeleton** (Python 3.11+ + pyusb). Modules: `usb.py`
   (`detach_kernel_driver` + `claim_interface`); `proto_bin.py` (constants + BE32
   packer + BOOT1 stage with 6×2048-B chunks + BOOT2 with 41×8192-B chunks);
   `proto_ascii.py` (NUL-terminated send, sync helper, all TBootPro commands, CRC32
   zxdl-style init 0xFFFFFFFF poly 0xEDB88320 NO final inversion); `container.py`
   (parse 0xD0 header + 25×128 records); `cli.py` (`--probe`/`--dry-run`/`--commit`/
   `--container`/`--skip-nv`/`--clobber-nv`/`--chunk-size`/`--verify`). Safeguards:
   dry-run default; refuse unknown container SHA-256; refuse NV overwrite unless
   `--clobber-nv`; refuse re-run within 30s; SHA-256 verify every `compat_write` via
   `compat_read`; mandatory power-cycle advisory.
3. **Write `bflasher_net.py`** — TCP/Ethernet for assembled-CPE users. Reuses
   `container.py` + `proto_ascii.py`; wraps inner protocol in A/C frames over
   `socket` + listen on 0.0.0.0:4536.
4. **Write `container_unpack.py`** + **`container_repack.py`** — per `PARTMAP.md`;
   default preserves the 16-byte key verbatim and reuses stock partition.bin.
5. **Write `ap_repack.sh`** — `mksquashfs` v3+LZMA + `shsq` magic patch + `mkimage`.
6. **Write `udev/99-mf253s.rules`** — adapted from `99-zxdl.rules` for
   `19d2:0256`/`0197`/`0199`.
7. **Write `docs/INSTALL.md`** — modprobe + udev + ModemManager install steps.
8. **No device interaction.** All Phase 1 is offline.

**Verification:** `pytest tests/test_container.py` (round-trip); `pytest tests/test_
proto_bin.py` (constants match rva3); `pytest tests/test_proto_ascii.py` (CRC32 =
zxdl vectors); `--dry-run` prints plan without USB I/O.

### Phase 2 — First device interaction with user present

Default: read-only probe, no writes.

1. Plug the bare modem into the Linux PC.
2. Check `lsusb -d 19d2:0256 -v` — confirm VID/PID and capture the descriptor (live
   confirmation of A1 evidence).
3. Run `./bflasher.py --container MF253SV1.0.0B10.bin --probe`. Detaches any kernel
   driver from interface 0; claims interface 0; sends 1 byte `0x5A` on EP 0x01 OUT;
   reads 1 byte on EP 0x81 IN with 30 ms timeout; logs the result; releases
   interface; exits. **Zero follow-on writes.**
4. **Decision tree:** `0xA5` → BootROM/tloader alive, proceed to BOOT1. `0xA7` →
   already in tboot, skip BOOT1+BOOT2. NAK/timeout → device needs BOOT-pad short
   per `zx297520v3-loader` README (14-pad grid), power-cycle, re-probe.
5. First commit run `./bflasher.py --container MF253SV1.0.0B10.bin --commit`:
   - **BOOT1:** `0x7A+BE32(0x00080000)+BE32(0x00002924)` → `0xA1`; tloader.bin in
     6×2048-B chunks → `0xA7`; `0x8A+BE32(0x00080000)` → `0xA8`.
   - **BOOT2:** sync `0x5A → 0xA5/0xA7`; `0x7A+BE32(0x27EF0000)+BE32(0x000503EC)`
     → `0xA1`; tboot uImage in 41×8192-B chunks (last 1004) → `0xA7`;
     `0x8A+BE32(0x27EF0000)` → `0xA8`.
   - **ASCII:** sync `0x5A → 0xA7`; `set partitions 1000\0` → `OKAY RECV_TABLES`;
     raw 4096-B partition.bin → `OKAY`; loop 25 records, `compat_write` →
     `DATA <hex>` → raw → `OKAY` → `compat_read` → `DATA <hex>` → host `OKAY\0` →
     raw → host `OKAY\0` → SHA-256 compare; abort on mismatch.
   - `reboot\0` → optional `OKAY REBOOT`. **Print "POWER-CYCLE REQUIRED" and exit.**
6. User power-cycles the device. Wait 10 s.
7. Re-check `lsusb` — expect `19d2:0197` ("ZTE Trap") or `19d2:0199`.
8. **Recovery** (if step 5 fails): power-cycle, BOOT-pad short, re-enumerate as
   `19d2:0256`, re-run from step 5. BootROM non-fused; unlimited retries safe.

**Verification:** `lsusb -v` matches A1 descriptor strings; after flash, `lsusb -d
19d2:0197 -v` enumerates 2+ interfaces (CDC-ACM+RNDIS); `/dev/ttyUSB0`+`1` appear;
`ip link` shows `usb0`; `sudo mmcli -L` shows ZTE modem.

### Phase 3 — Normal-mode enablement

After Phase 2 the modem is alive; drive it as a normal LTE modem.

1. Default AT port test — `minicom -D /dev/ttyUSB0 -b 115200`: `AT` → `OK`;
   `AT+CGMI` → `ZTE CORPORATION`; `AT+CGSN` → IMEI; `AT+CPIN?` → `+CPIN: READY`.
2. Discover the runtime PID — `AT+ZMODESWITCH=USB-Rndis` per B2 string table at
   pscpu `0x8c04f0..0x8c0560`; capture new PID with `lsusb` (expected: `19d2:0199`).
   If different, update `udev/99-mf253s.rules` + kernel whitelist.
3. Test data path — `sudo ip link set usb0 up && sudo udhcpc -i usb0`; modem returns
   DHCP lease.
4. ModemManager — `sudo mmcli -m 0 --simple-connect="apn=internet"`; `mmcli -m 0
   --enable`; `nmcli connection up`.
5. Optional: lock to `19d2:0199` permanently — patch `lte_ati2_nv_0x10000.bin` via
   `container_repack.py --allow-key-edit --patch-ati2=0x10000`; re-flash via Path A.
   **HIGH brick risk if malformed; defaults to OFF.**

**Verification:** `ping -c 3 8.8.8.8` from PC over `usb0` succeeds; `mmcli -m 0`
shows signal bars, network name, IMEI; `AT+ZRSSI` returns valid RSSI.

### Phase 4 — Polish for GitHub release

1. Repo layout — `bflasher/`, `docs/`, `udev/`, `tests/`, `ap_repack/`, `examples/`,
   `README.md`, `LICENSE` (GPLv2-or-later for original code; MIT notes for reused
   zxdl).
2. **Disclaimers:** "Wrong container → wrong IMEI → permanent SIM lock. Use only the
   SHA-256-verified stock container." "Device may be carrier-locked; see
   `docs/SIM_LOCK.md`." "Always power-cycle after a successful flash."
3. GitHub Actions — pytest on push; build `bflasher.py` as a single-file script via
   `pyinstaller`.
4. Issue templates — "Brick report" (requires `lsusb -v`, `--probe` log, container
   SHA-256); "Feature request".

**Verification:** `pip install bflasher-0.1.0.tar.gz` installs cleanly;
`bflasher --version` prints `0.1.0` + commit hash; all docs cross-link to evidence.

---

## 5. Deliverables

| File | What it is | Lines |
|---|---|---|
| `bflasher.py` | USB-path Python+pyusb flasher (Phase 1+2) | ~600 |
| `bflasher_net.py` | TCP/Ethernet-path flasher (assembled CPE) | ~400 |
| `container_unpack.py` / `container_repack.py` | Parse / repack `MF253SV1.0.0B10.bin` per PARTMAP.md (preserve key, re-CRC uImage) | ~150 / ~200 |
| `ap_repack.sh` | AP-rootfs squashfs v3+LZMA + 'shsq' magic patch + mkimage wrapper | ~80 |
| `udev/99-mf253s.rules` | udev for 19d2:0256/0197/0199 (adapted from `99-zxdl.rules`) | ~30 |
| `docs/{PROTOCOL,FLASHER_SPEC,TOPOLOGY,MODEM_USB,CONTAINER}.md` | Consolidated spec set (copies of A3/B3/B1/B2/A1) | exists |
| `docs/INSTALL.md` | Driver install + ModemManager usage | ~120 |
| `docs/SIM_LOCK.md` | IMEI-unlock + kozik47 reference | ~80 |
| `tests/test_{container,proto_bin,proto_ascii}.py` | pytest vectors (round-trip, rva3 constants, zxdl CRC32) | ~100/~80/~80 |
| `README.md` + `LICENSE` | Quickstart + GPLv2-or-later / MIT notes | ~150 |

**Total new code: ~2000 lines** (within 3 person-days for a single competent developer).

## 6. Unknowns needing device (with SAFE procedures)

Each unknown touches the device only with read-only or 1-byte-write operations; none
requires the full flash to succeed first.

1. **Exact post-`AT+ZMODESWITCH` PID** (most likely `0199`, candidates `0257`/`0167`/
   `1275`). **Safe:** After Phase 2 succeeds, issue `AT+ZMODESWITCH=USB-Rndis` over
   `/dev/ttyUSB0`; `lsusb` in another terminal captures the new PID. **One AT command
   write, one `lsusb` read.**

2. **Whether `0x5A → 0xA5` sync works directly on the bare USB.** **Safe:**
   `./bflasher.py --probe` sends 1 byte `0x5A`, reads 1 byte, logs, exits. **One byte
   write, one byte read.**

3. **Whether the 16-byte container key** matters at runtime. **Safe:**
   `container_repack.py` preserves the stock key bytes by default; flash via Phase 2.
   **No additional risk vs stock flash.**

4. **Whether tboot re-enumerates to a new PID or stays on `19d2:0256`.** **Safe:**
   After BOOT2 jump, before ASCII stage, re-`lsusb`. If changed, re-run `--probe` to
   re-find bulk EPs. **One `lsusb` read between BOOT2 and ASCII.**

5. **AT command to toggle network-card mode** (`AT+ZNCARD=1`). **Safe:** Try it over
   `/dev/ttyUSB0`; capture reply + `lsusb` re-enumeration. **One AT command write.**

6. **Exact partition-table checksum algorithm** (stored `0xE9DDF3D5`). **Safe:** Test
   candidate algorithms offline using the 17-entry table from `partition.bin` (XOR,
   CRC32, Adler-32, sum bytes/u16s). The candidate yielding `0xE9DDF3D5` is the
   algorithm. Best guess: XOR matches zxdl. **No device interaction.**

All 6 resolvable within the first hour of Phase 2; none adds brick risk.

## 7. Open-source reuse scorecard

| Component | Source | Reused | Net new |
|---|---|---|---|
| BootROM binary protocol | rva3/CVE-2026-40003 (AGPLv3) | ~80 | ~50 |
| TBootPro ASCII protocol | sandibabsel/ZX297520-Flasher (MIT) | ~120 | ~80 |
| Partition parser | sandibabsel/ZX297520-Flasher (MIT) | ~50 | ~30 |
| Container format | (no public impl) | 0 | ~350 |
| Linux PC drivers | mainline kernel (GPLv2) | unlimited | 0 (docs only) |
| AP squashfs tools | sasquatch, mkimage (GPLv2) | unlimited | ~80 |
| OpenWrt precedent | openwrt_mf283plus (GPLv2) | DTS only | n/a (Phase D) |

**Open-source reuse score: HIGH (8/10).** Two complete public implementations cover every
wire byte; mainline kernel covers all userland drivers; only the container format is
net-new.

## 8. Risk register (final)

| Risk | Severity | Mitigation |
|---|---|---|
| Wrong container → wrong IMEI/SIM-lock | HIGH | SHA-256 whitelist; refuse-on-unknown; explicit `--clobber-nv` |
| Power loss mid-flash | MEDIUM | UPS recommendation; read-back verify; power-cycle advisory |
| BootROM unresponsive on bare USB | LOW | `--probe` first; BOOT-pad short fallback |
| tboot re-enumerates to CDC-ACM | LOW | `--probe` after BOOT2 jump |
| Mid-flash Ctrl-C | MEDIUM | Signal handlers abort between stages only |
| Wrong ATI2 NV patch in Phase 3 | HIGH if used | `--allow-key-edit` default OFF |

**Overall brick risk for Phase 1+2+3: LOW** (BootROM non-fused; any failure recoverable).

## 9. Final scorecard for the user

| Question | Answer |
|---|---|
| Can an ordinary Linux PC drive this LTE module? | **YES**, after flashing stock firmware via Path A and rebooting to `19d2:0199`. |
| Is Windows required? | **NO.** Phase 1+2+3 are 100% Linux; the Windows SalesDL is reproduced byte-exact. |
| Brick risk right now? | **LOW.** BootROM non-fused; power-cycle + BOOT-pad short always recovers. |
| How long until it works? | **3 person-days** for a competent developer (Phase 1+2). |
| Graphical installer? | **Phase 4** adds pip-installable + `pyinstaller` single-file binary. |
| SIM lock? | **Documented as a known risk**; unlock port (kozik47) referenced but out of scope. |
| What's NOT covered? | AP-side OpenWrt port (Option D, deferred); patched AP firmware (Option C, deferred); full RF calibration preservation (future work). |
