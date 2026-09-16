# ZTE MF253S (ZX297510) Linux 驱动

把运营商淘汰的 **ZTE MF253S / ME3760V2**（Sanechips ZX297510）mSATA 4G 模块变成 Linux 下的原生移动数据网卡。

装上这两个内核模块（`zte_ecm` + `zte_atfix`）后，**任何装 stock ModemManager / NetworkManager 的发行版都能即插即用**：任务栏出现「移动数据」，点一下就联网。无需守护进程、无需 udev 规则、无需手工拨号。

**功能一览**：数据（原生 4G 网卡，开机自动连接）/ 信号（真实百分比）/ EC20 等模块共存（无需黑名单）。
IPv6 在数据面焊死；制式与模式的显示受 ModemManager Icera 插件限制（详见下文）。

```
NetworkManager ── ModemManager ──┬─ ttyUSB0 (AT, MM 管理)
                                 │
                           zte_atfix.ko ── ttyUSB1 (私有 AT 口：初始化/拨号/保活)
                                 │
                           zte_ecm.ko ───── if1 ECM ── enpXsYfZuVi1 (数据网卡)
```

## 支持的硬件

| 项目 | 详情 |
|---|---|
| 模块 | ZTE MF253S / ME3760V2 |
| 基带 | ZTE Sanechips ZX297510（WF7510） |
| USB ID | `19d2:0199`（正常模式）、`19d2:0256`（BootROM） |
| 制式 | TD-LTE（移动定制，硬件仅校准 GSM / TD-SCDMA / LTE） |
| 接口 | if0 = USB-AT，if1 = USB-Rndis（ECM），if2 = USB-Modem，if3 = USB-log |

## 这个模块为什么难搞

- **QMI 是残废的**：任何 UIM/NAS 命令都会让它掉进 BootROM 重启；
- **PPP 拨号被拒绝**：LTE 默认承载常开，`ATD*99` 回 `+CME ERROR: 3`；
- **没有 DHCP**：IP/网关/DNS 只通过私有 URC `+ZGIPDNS` 下发；
- **数据通道要私有命令激活**：`AT+CGACT=1,1` 之后还必须发 `AT+ZGACT=1,1`；
- **21 秒自复位**：主机不轮询就自杀重启（CPE 逆向出的保活序列）；
- **一堆标准命令不支持**：`ATZ`、`AT+GCAP`、`AT+WS46=?` 全部 `+CME ERROR: 6003`。

## 工作原理

`zte_atfix` 把模块伪装成 ModemManager 完整支持的 **ZTE Icera** 机型（MF821 一类），因为 MM 的 ZTE 插件对非 Icera 模块会忽略网卡口，而对 Icera 模块走「NET 口 + 静态 IP」的 bearer 路径——正好契合这台 ECM-only 的机器。

| ModemManager 发出的命令 | 驱动处理 |
|---|---|
| `ATZ` | 重写为 `ATE0` |
| `AT+GCAP` | 伪造 `+GCAP: +CGSM,+CLTE`（启用 EPS/LTE 跟踪） |
| `AT+WS46=?` | 伪造模式列表 |
| `AT%IPSYS?` | 伪造 `%IPSYS: 0,1,0` → MM 判定为 Icera 模块 |
| `AT%IPDPACT=<cid>,1` | **翻译为真实拨号**：`AT+CGACT=1,1` + `AT+ZGACT=1,1`，完成后回 OK + `%IPDPACT` URC |
| `AT%IPDPADDR=<cid>` | 用嗅探到的 `+ZGIPDNS` 缓存应答静态 IPv4 配置 |

其他要点：

- **内核心跳**：probe 后自动执行 CPE 逆向出的初始化序列，并每 2 秒轮询 `AT+CSQ`/`AT+CEREG?`/`AT+COPS?`，彻底杜绝 21 秒自复位；
- **私有口隐身**：驱动用 if2 做自己的事情（初始化/拨号/保活），并把 MM 探口的裸 `AT` 探测吞掉，MM 会把该口标记为「非 AT 口」自动无视；
- **驱动内关闭 USB autosuspend**，不需要任何 udev 规则；
- **APN 自动识别**：按 IMSI 前五位选择 CMNET / 3gnet / ctnet；
- **设备认领**：模块的接口同时会被 `option`/`qmi_wwan` 匹配，驱动在加载时会把它们从别的驱动手里释放出来并接管（热插拔由 USB 通知 + 每秒看门狗兜底）。因此**不需要任何 modprobe 黑名单**，同一台机器上的 EC20 等模块照常用 `qmi_wwan`，互不干扰；
- **私有口防护**：if2/if3 是驱动的私有通道，数据永远不进 tty 层（否则 ModemManager 探测时会看到驱动的轮询应答，把私有口当 AT 口抢走）。

> **启动时序**：驱动初始化时会做一次 `CFUN=0 → CFUN=1` 强制重组并等待注册（约 1 分钟），
> 这是为了让模块进入干净的网络附着状态；随后自动激活并绑定 ECM 数据路径。
> 不需要可以 `force_reattach=0` 跳过，开机即可连接。

## 安装

```bash
# 依赖：内核头文件
sudo apt install build-essential linux-headers-$(uname -r)

# 编译
make -C drivers/zte_ecm
make -C drivers/zte_atfix

# 安装
sudo cp drivers/zte_ecm/zte_ecm.ko drivers/zte_atfix/zte_atfix.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a

# 开机自动加载（驱动会自己从 option/qmi_wwan 手里抢回设备，
# 不需要任何 modprobe 黑名单——qmi_wwan 可以留给 EC20 之类的模块用）
printf 'zte_ecm\nzte_atfix\n' | sudo tee /etc/modules-load.d/zte.conf

# 立即加载
sudo modprobe zte_ecm zte_atfix

# 确保 ModemManager 开机自启（数据/信号的上层管理器；通知走 MM GUI）
sudo systemctl enable --now ModemManager
```

然后重启 ModemManager（或直接重启电脑），`nmcli device status` 里就会出现 `gsm` 设备，点击连接即可。

## 在 OpenWrt / ImmortalWrt 上集成（实测记录）

> 驱动在 OpenWrt 系上同样即插即用；上层用官方 feed 的 `modemmanager` +
> `luci-proto-modemmanager`（LuCI 自带「Cellular Network」状态页）。
> 以下坑实测于 **ImmortalWrt 24.10.6（内核 6.6.133）**，都不是本模块固件的问题，
> 而是 OpenWrt / ModemManager 集成的一般性问题。

### 装包与接口配置

```bash
opkg install kmod-zte-ecm kmod-zte-atfix modemmanager luci-proto-modemmanager

uci set network.mm=interface
uci set network.mm.proto='modemmanager'
uci set network.mm.device='0'    # MM 的 modem 序号
uci set network.mm.apn='CMNET'
uci commit network
```

### 坑 1：MM 装好后看不到已插着的模块

OpenWrt 的 ModemManager 靠 hotplug 事件缓存发现设备；在装 MM **之前**就插着的设备
不会被上报，`mmcli -L` 为空。把 USB 设备 unbind/rebind 一次即可（或重启）：

```bash
echo 1-5 > /sys/bus/usb/drivers/usb/unbind
echo 1-5 > /sys/bus/usb/drivers/usb/rebind   # 路径按实际 USB 拓扑
```

### 坑 2：netifd 不认识 `modemmanager` 协议

netifd 启动时加载 proto handler；若 `modemmanager` 包是后装的，`ifup mm` 毫无反应
（接口报 `NO_DEVICE`）。需要**完整重启 netifd**（`restart` / `reload` 不够）：

```bash
/etc/init.d/network stop; sleep 2; /etc/init.d/network start
```

### 坑 3：开机竞争 + 失败后不重试

proto 可能跑在 MM 探测到模块之前；一次失败后接口被标记 `available=0` 且不再重试
（LuCI 显示「网络设备不存在」）。两步处理：

让 ModemManager 先于 network 启动：

```bash
mv /etc/rc.d/S60dbus /etc/rc.d/S18dbus
mv /etc/rc.d/S70modemmanager /etc/rc.d/S19modemmanager
```

并给 `/lib/netifd/proto/modemmanager.sh` 的模块校验加等待（最长 120s）：

```diff
+	mmcount=0
+	while [ "${mmcount}" -lt 120 ]; do
+		modemstatus=$(mmcli --modem="${device}" --output-keyvalue 2>/dev/null)
+		modempath=$(modemmanager_get_field "${modemstatus}" "modem.dbus-path")
+		[ -n "${modempath}" ] && break
+		mmcount=$((mmcount + 1))
+		sleep 1
+	done
+
 	# validate that ModemManager is handling the modem at the sysfs path
-	modemstatus=$(mmcli --modem="${device}" --output-keyvalue)
-	modempath=$(modemmanager_get_field "${modemstatus}" "modem.dbus-path")
```

### 坑 4：`/32` 承载的默认路由

LTE 下发的地址是 `/32`、网关离网，netifd 照网关装默认路由会失败（流量悄悄回落到
局域网网关）。`/32` 时改用接口路由：

```diff
 	[ -n "${gateway}" ] && {
-		echo "adding default IPv4 route via ${gateway}"
-		proto_add_ipv4_route "0.0.0.0" "0" "${gateway}" "${address}"
+		if [ "${prefix}" = "32" ]; then
+			# /32 point-to-point bearer: the gateway is off-link, so
+			# install the default route directly on the interface
+			echo "adding default IPv4 route over ${wwan}"
+			proto_add_ipv4_route "0.0.0.0" "0" "" "${address}"
+		else
+			echo "adding default IPv4 route via ${gateway}"
+			proto_add_ipv4_route "0.0.0.0" "0" "${gateway}" "${address}"
+		fi
 	}
```

### 其它注意

- **LAN 接口不要设 `gateway`**：会把默认路由抢走，造成「能上网但不是走模块」的假象（实测踩过，用 `ip route get 223.5.5.5` 才验出真身）；
- 要让 LAN 客户端共享模块上网：把 `mm` 加进 firewall 的 wan zone（`masq=1` 才有 NAT）；
- 状态查看：`mmcli -m 0`，或 LuCI → Status → Cellular Network。

## 已知限制

- **仅 IPv4**：固件虽支持 IPV6 PDP，但 ECM 通道没有 RA/NDP 转发能力，IPv6 实际不可用。驱动会**在数据面直接丢弃所有 IPv6 帧**（计入 `tx_dropped`），并尽力关闭该网卡的 IPv6 协议栈——上层开不开 IPv6 都行，不会有任何 v6 包发到模块；
- **仅 TD-LTE 硬件**：移动版模块没有 WCDMA 校准数据，插联通卡收不到信号（这不是锁）；
- **USSD 不支持**：固件没有 `AT+CUSD`，`*100#` 这类查话费/业务办理用不了；
- **语音不支持**：数据卡模块，没有通话功能（ModemManager 不暴露 Voice 接口）；
- **不要热插拔 mSATA**：关机 → 插拔 → 开机；
- 不同批次固件可能略有差异（开发基于 `ZTE_MF253SV1.0.0B01`）。

## ModemManager（Icera 插件）的限制与妥协

为了让 MM 使用 ECM 网口，驱动把模块伪装成 ZTE Icera 机型。但 MM 的 Icera 插件写于 LTE 普及之前，其**模式/制式映射里没有 LTE**（上游与本机 1.25.95 均如此，反汇编确认），因此界面上的网络类型只能"尽量接近"：

| 项目 | MM Icera 插件的行为 | 驱动的处理 |
|---|---|---|
| `AT+WS46=?` | Icera 类不用它加载模式 | 伪造 `+WS46: (28)`（纯 E-UTRAN），供其他实现使用。注意 **25 会被 MM 映射成 `MM_MODEM_MODE_ANY`（含 5G）**，不要报 |
| `AT%IPSYS=?` | 只能解析 2G/3G 组合（解析器无 LTE case） | 伪造 `%IPSYS: (1),(1)`（3G-only）→ GNOME 模式选择器只显示一个 3G 条目，**不会出现 5G/2G** |
| `AT%IPSYS=<n>` | 用户切换模式时下发 | 直接回 OK（模块实际仍锁 LTE） |
| `AT%NWSTATE` | 制式只能映射 2G/3G（无 LTE） | 伪造响应；`nwstate_tech` 模块参数可调（默认 `HSDPA-HSUPA-HSPA+` → GNOME 显示 3G） |
| `AT+CSQ` | 期望标准 0..31 | 固件返回 `253 + RSRP(dBm)` 私有刻度（如 149 → RSRP -104 dBm），MM 会钳位成 100%；驱动归一化后上报，显示真实信号（约 60%） |

**想让 GNOME 显示真正的 4G/LTE**：给 MM 的 Icera 插件打个小补丁即可——`add_supported_mode()` 增加 `case 4 → MM_MODEM_MODE_4G`，`nwstate_to_act()` 增加 `"lte"` 分支，共约 10 行。驱动层面做不到这件事（这是 MM 插件的限制，不是模块固件的）。

## 目录结构

```
drivers/zte_ecm/     ECM 数据口驱动（usbnet）
drivers/zte_atfix/   AT 修复 + Icera 伪装 + 拨号翻译 + 链路管理
tools/               （历史遗留）用户态守护进程版本，仅供参考
docs/                设计与逆向笔记
research/            固件分析、CPE 逆向资料（不含固件二进制）
extras/              同硬件上的蜂鸣器整活项目
```

## 许可证

本仓库中 **用户态工具与文档** 采用 MIT 许可证（见 `LICENSE`）。

`drivers/` 下的内核模块为 **GPL-2.0**（内核模块必须与内核兼容，源码头部已标注 SPDX）。

---

*本项目与 ZTE 无关，仅供学习研究。刷机、拆机有风险，请自行承担后果。*
