# SmartPanel · 4-inch 86-box Smart Panel

[中文](README.md) ｜ **English**

[![Buy on Taobao](https://img.shields.io/badge/Buy%20on%20Taobao-hardware-FF5000?style=flat-square)](https://item.taobao.com/item.htm?id=650865599184&skuId=5500508256776) [![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=flat-square)](LICENSE) [![Built with FlyThings MCP](https://img.shields.io/badge/Built%20with-FlyThings%20MCP-0A7AFF?style=flat-square)](https://github.com/zkswe/mcp_release)

> **GitHub (primary)** <https://github.com/zkswe/smartpanel> ｜ **Gitee mirror** <https://gitee.com/Kwolve/SmartPanel>

---

## In one line: your wall switch becomes a cloud photo frame — and an electronic fish tank

A **4-inch 480×480 Linux touch panel** that drops into a standard **86-box**:
while it's working it's a **3 × 10 A relay + scene control panel**
(**native Home Assistant integration**, Domoticz works too);
when it's idle it's a **cloud photo frame** and an **electronic fish tank** —
scan a QR code to push photos from your phone, or loop a video clip and let the wall become
moving artwork. **2–4 panels** can also be tiled into one **video wall** for storefronts and showrooms.

**Boots in 3 seconds. Commissioned by phone in 30 seconds — no unboxing, no laptop on site.**

> 🤖 **This firmware was built by AI**: the UI layouts, the business logic and the on-device
> verification were all produced by an **AI assistant + FlyThings MCP**. Secondary development is the
> same "just ask" workflow — **no framework deep-dive, no IDE required** (see §7).

> 🛒 **Buy the hardware (Taobao)**: <https://item.taobao.com/item.htm?id=650865599184&skuId=5500508256776>
>
> Model **SW48480040D1** ｜ SoC **Z20 / SigmaStar SSD201·SSD202D** ｜ License **MIT**

---

<table>
  <tr>
    <td align="center"><img src="docs/shots/01_home.png" width="150"><br><sub><b>Home</b><br>3 relays + scenes</sub></td>
    <td align="center"><img src="docs/shots/02_album_qr.png" width="150"><br><sub><b>Album</b><br>scan to upload</sub></td>
    <td align="center"><img src="docs/shots/03_settings.png" width="150"><br><sub><b>Settings</b><br>all features at a glance</sub></td>
    <td align="center"><img src="docs/shots/04_screensaver_video.png" width="150"><br><sub><b>Screensaver</b><br>video / photo rotation</sub></td>
    <td align="center"><img src="docs/shots/05_ha_config_qr.png" width="150"><br><sub><b>HA config</b><br>scan &amp; paste token</sub></td>
  </tr>
  <tr>
    <td colspan="5" align="center"><img src="docs/shots/10_wall_3split.png" width="860"><br><sub><b>3-screen video wall</b> · one clip split per panel, phase-aligned + time-synced (1×3, 52 px discarded between panels)</sub></td>
  </tr>
</table>

---

## 1. Why buy it

| # | Selling point | What it means for you |
|---|---|---|
| 1 | **One panel replaces three** | Takes the place of 3 wall switches **plus** a scene panel. Fewer cut-outs, fewer parts, less wiring, less labour — all inside a single 86-box |
| 2 | **Native Home Assistant / Domoticz** | Power it on, connect to your broker, and **3 switch entities appear automatically** in HA (named exactly as on the panel). **Zero code, zero custom integration** |
| 3 | **Scan, paste, done — 30 seconds** | No unboxing to reconfigure: the panel shows a QR code → scan it to open the **on-panel web page** → paste broker address and token → live immediately |
| 4 | **Tile several into one wall** | 2–4 panels side by side become one large picture. Storefront backdrop walls, showroom loops, ad slots — with **no server required** (the master broadcasts the time base) |
| 5 | **More than a switch** | When it's idle it's a **cloud photo frame** (scan to upload from your phone, auto-rotating) and an **electronic fish tank** (loop a clip — moving artwork on the wall); full screen-off window at night; separate working/screensaver brightness |
| 6 | **Easy to install, easy to mass-produce** | Standard 86-box flush mount; **one-tap 180° flip** (screen + touch + video rotate together, so it works under a ceiling); factory-default file means **it can go live straight out of the box** |
| 7 | **AI-developed — low effort, short cycle** | This project itself was built by an **AI assistant + FlyThings MCP**: change UI / add features / build & package / capture and verify on the real device — **all inside a conversation**. **No embedded UI team to hire, no framework docs to digest first**, and the full source is MIT |

---

## 2. Three core scenarios

### 2.1 On the home wall: 3 relays + one-tap scenes

Three device cards (**Living room / Bedroom / Light strip** — rename freely) above a row of scenes
(**Home / Movie / Sleep / Relax**).

| Element | Description |
|---|---|
| Device card | **Tap the whole card = toggle that relay**; the state label updates instantly and is reported to HA |
| Scene row | **Tap a scene = run the whole group** (Home = living room + light strip on), up to 8 scenes |
| One source of truth | HA toggles → relay acts; panel touch → HA state syncs. The state lives on-panel, so the two sides can never disagree |
| Phone / voice control too | HA app and voice assistants drive it over standard MQTT — the panel follows |

### 2.2 Commercial mini-wall: several panels, one picture

N identical panels in a row play the same clip while idling. Each shows only its own slice, and the
picture stays **frame-synced (measured phase difference ≤ 10 ms)**.

- **Web splitter**: drag a box per screen in the browser (each screen can be nudged independently to compensate for physical seams) → export `seg_1..N.mp4 + playlist.json` → **one-click distribute** to every panel
- **No server**: peer-to-peer UDP on the same subnet; the master broadcasts the time base
- Supports **a playlist of multiple clips** — good for storefronts, showrooms and ad slots

Start it: `python tools/video_wall/web/server.py --port 8796` → open `http://<PC>:8796/`

<img src="docs/release_shots/07_video_wall_tool_ui.png" width="720" alt="Splitter tool">

### 2.3 Cloud photo frame / electronic fish tank: give the wall some atmosphere

It is at its best when it isn't working:

- **Cloud photo frame**: the panel shows a QR code → scan it with your phone to upload photos (mini-program supported too) → they land on the panel and rotate automatically
- **Electronic fish tank / moving artwork**: pick a video as the screensaver and loop it — an aquarium, falling snow, city nightscape or a brand film. The wall becomes moving artwork
- Or just drop files on a **TF card / USB stick** — plug and play, no reconfiguration to change content
- The screensaver can layer content too: **big clock + date + optional temperature/humidity / weather / status bar**, each item can be added, removed and repositioned
- Configurable pacing: N seconds per photo (default 15 s), clip interval (0 = next when finished)

---

## 3. Specifications

| Item | Spec |
|---|---|
| Model | **SW48480040D1** (4-inch 86-box series, Z20 platform) |
| Display | 4-inch square LCD **480 × 480**, fully laminated single-touch capacitive |
| SoC / OS | Z20 (SigmaStar SSD201 / SSD202D, dual Cortex-A7 @1.2 GHz, 128 MB DDR3 onboard), FlyThings V2.1 + EasyUI |
| Storage | 16 MB NOR + **128 MB SD NAND** (media / albums live here) |
| Power | **AC 220 V** or **DC 9–24 V** (either one, chosen on site wiring) |
| Relays | Up to **3 × 10 A** (this project uses living room / light strip / tea table) |
| Wireless | Built-in **WiFi**; optional Zigbee / Bluetooth module |
| Wired | **RS485** + **100M Ethernet** |
| Audio | Built-in 1 W amplifier speaker |
| Sensors (optional) | Ambient light, human presence |
| Mounting | Standard **86-box**, flush wall mount; **normal or upside-down (180°) both supported** |
| Boot | ~**3 seconds** |

---

## 4. Three run modes (pick one per site)

| Mode | Who it's for | How it works |
|---|---|---|
| **HA mode** | Homes already running Home Assistant / Domoticz | Connects to an external broker (EMQX / Mosquitto); MQTT Discovery creates 3 switch entities automatically; scenes are managed by HA |
| **Local master** | No HA yet — you want your own network | The panel runs a lightweight MQTT broker itself; slaves and the mini-program connect directly |
| **Local slave** | Multi-panel linkage | Connects to the master panel's broker on the same subnet |

> The on-panel broker is a lightweight in-house implementation: QoS0/1, retained, Will and wildcards
> are supported; **no** TLS / QoS2 / persistent sessions.

---

## 5. Three steps on site

> Prerequisite: the panel and HA are on the same LAN, and HA already has the MQTT integration connected to a broker.

**Step 1 · Power and wiring**: AC version takes 220 V live/neutral (relay outputs go to the light
circuits); DC version takes 9–24 V. **Always work with the breaker off.**

**Step 2 · Join WiFi**: Home → gear icon (top right) → Settings → **WiFi** → pick SSID → enter password → back.
(For wired sites, just use the 100M Ethernet port.)

**Step 3 · Switch to HA mode and scan to configure**: Settings → Run mode → **HA mode** → a QR code
appears below → scan it with your phone to open the on-panel web page:

<img src="docs/release_shots/06_web_config_page.png" width="260" alt="On-panel config page">

- **Server address**: `mqtt://192.0.2.188:1883` (a bare IP gets `:1883` appended automatically)
- **Username / password (token)**: paste long tokens directly (line breaks and spaces are stripped)
- Hit **Save** → the panel reconnects immediately and the status turns **Connected**
- Back in HA → Settings → Devices & Services → MQTT: the device **SmartHomePanel** appears with **3 switches** (Living room / Bedroom / Light strip)

**To change the server or token later**: just repeat step 3 (scan → edit → save). **No unboxing, no laptop.**

> **Mass production / factory**: ship a `panel-defaults.json` (broker, etc.) on `/mnt/sdnand` or in
> `/res/etc`; the device writes it into prefs on first boot — so **it can reach HA without any on-site configuration**.
> See `docs/FACTORY-DEFAULTS.md`.

---

## 6. Buy & contact

| Channel | Where |
|---|---|
| 🛒 **Buy the hardware (Taobao)** | <https://item.taobao.com/item.htm?id=650865599184&skuId=5500508256776> |
| 🏢 Company | **Shenzhen ZKSWE Technology Co., Ltd.** |
| 🌐 Website / docs | <https://www.zkswe.com> ｜ <https://developer.flythings.cn/> |
| 📞 Phone | +86 755-23019045 |
| 📍 Address | Room 1407, Tower A, Fenghuang Zhigu, Gongle Community, Xixiang Street, Bao'an District, Shenzhen, Guangdong, China |

> For volume orders or customisation (size, relay count, connectivity, bezel colour) please contact the
> company directly. This repository covers software and documentation only.

---

## 7. Secondary development: let the AI do it (FlyThings MCP)

**This is the project's biggest hidden selling point: low effort, short cycle** — the whole repository
was built this way, and you work the same way.

### One development round = 3 steps

| Step | Who | What happens |
|---|---|---|
| ① **Describe** | you | "Turn the third card on the home page into a curtain icon, same size" |
| ② **Do it** | AI | searches the built-in knowledge base → edits `ui/*.json` / logic → renders an HTML preview for you to confirm |
| ③ **Verify** | AI | `fui pack` → `fun build -p Z20` → push to the device → **capture the real screen + pixel-compare** (±2 tolerance) → sends you the screenshot |

### What you get (the AI does all of it)

| You say | The AI can |
|---|---|
| "change this page for me" | edit the layout in a drag-and-drop editor → write changes back to json → package |
| "how do I configure this control?" | **fully offline knowledge-base search** (local vectors + BM25, **no API key needed**), answers carry source and confidence |
| "build it and push to the device" | `fun install` → `fun build` → auto-detect the device → push & run |
| "what does it look like now?" | capture the real device screen (including video-layer frames) |
| "is the change correct?" | pixel-diff regression (zero-token verification) |
| "make me an upgrade package" | build `update.img` (TF card / ADB / OTA flashing routes) |

### Onboarding takes one step

Plug **FlyThings MCP** into your AI client (Trae / Cursor / Claude Desktop / Kimi…).
Its **release build is maintained and published separately**; this repo only references it, never vendors it.

| Purpose | Path |
|---|---|
| **Release version (where users get it, primary)** | <https://github.com/zkswe/mcp_release> |
| China mirror (Gitee) | <https://gitee.com/Kwolve/flythingsmcp_release> |
| Local path (same workspace, for verification, optional) | `tools/FlyThings_mcp_release/` |

Install and integration steps follow that release repo's README (current release `0.27.134-open`, 43 tools).

**Fastest start**: tell your AI → "clone and install `https://github.com/zkswe/mcp_release`".

---

## 8. Project layout

```
src/
  Main.cpp                 entry point
  activity/                main activity
  logic/                   page logic (home/main/settings/scenes/album/video/wall/off…)
  device/                  RelayManager (single source of truth), SensorManager
  network/                 MqttBridge (HA mode), LocalLink (local master/slave),
                           MiniBroker (embedded fallback), LocalBrokerService,
                           WebConfigServer (on-panel config web page), PanelLink, TcpReceive
  storage/ConfigStore      all persisted settings (prefs)
  wall/                    video wall (WallLink clock sync / WallPlayer playback)
ui/                        *.json layouts + *.ftu artifacts (built by `fui pack`)
resources/images/          UI image assets
font/                      Chinese fonts (trimmed font set: no emoji / special symbols)
tools/                     video-wall splitter (web UI), factory defaults, host-side smoke tests
docs/                      release notes / manuals / wall guide / HA config / Domoticz / factory defaults
```

---

## 9. Build & deploy

```bash
fun install                                   # fetch dependencies (re-run after editing Manifest)
fun build -p Z20                              # → .fun/z20 (or .fsc/z20) libzkgui.so
fun pack -p Z20 --startup-dir /res --release-version 1.0.N   # build update.img (USB/TF upgrade)
```

Hot-swap during development (keeps system partitions untouched; a power cycle reverts):

```bash
adb push libzkgui.so /tmp/lib/libzkgui.so
# point startupLibPath in /tmp/EasyUI.cfg to /tmp/lib/libzkgui.so (must be BOM-free!)
adb shell "setprop ctl.restart zkswe"
```

⚠️ Push a busybox first for on-device debugging: `adb push tools/busybox/bin/z20/busybox /tmp/busybox`
(the device shell lacks free/grep/nohup).

---

## 10. MQTT topics

```
smartpanel/<deviceId>/availability                  online / offline (LWT)
smartpanel/<deviceId>/status                        device status JSON (retained)
smartpanel/<deviceId>/switch/relay_<1..3>/state     relay state ON/OFF (retained)
smartpanel/<deviceId>/switch/relay_<n>/command      external command ON/OFF
smartpanel/broadcast/scene                          local mode: scene activation
smartpanel/ha/scenes                                scene list pushed by HA (retained)
homeassistant/switch/<uid>_<relay>/config           HA Discovery (HA mode)
```

---

## 11. Documentation

| Topic | Document |
|---|---|
| Full manual (with all real-device screenshots) | `docs/整机说明书.md` |
| Video wall deep dive | `docs/说明书-多屏拼接.md`, `docs/wall-linkage-design.md` |
| HA configuration (on-panel web page by QR) | `docs/HA-CONFIG-WEB.md` |
| Domoticz compatibility (measured) | `docs/DOMOTICZ-COMPAT.md` |
| Factory defaults / provisioning | `docs/FACTORY-DEFAULTS.md` |
| Release notes (screenshot gallery) | `docs/发布说明.md` |
| Third-party deps & licenses | `THIRD_PARTY_LICENSES.md`, `PUBLISH.md` |

---

## 12. Verification & quality

- Panel UI pixel regression: `tools/ui_tools/ui_diff.py` (±2 tolerance)
- On-panel web config: `tools/webcfg_test/` (x86 stub smoke test, **no device needed**)
- Multi-device batch: MCP `flythings_test_run` (case JSON + logcat assertions)

---

## 13. Known limitations (stated up front)

- The panel itself has **no temperature/humidity sensor** (screensaver data comes from a gateway push; to get "sensors into HA", publish them from the sensor side via standard discovery)
- The on-panel broker (local modes) does **not** support TLS / end-to-end QoS2 / persistent sessions
- Tiling requires **clock alignment** across panels (NTP or master epoch broadcast); keep the firmware version identical within a group
- On the Domoticz side there is **no equivalent for scenes** (use HA automation; on Domoticz you'd wire it up with dzVents/Lua)
- On a wired site, the QR page automatically advertises the `eth0` address; with no IP it shows "waiting for the panel to get online"
- The third-party `base-utility` player release path calls `drop_caches` (~every 10 s when looping video segments, raising load) — known issue; the workaround is not to destroy the player on every segment switch

---

## 14. Open source & license

- This project and the application layer: **MIT** (see `LICENSE`); the on-panel MQTT service is MIT as well
- **No LAN addresses or credentials in code**: site parameters come from the factory-default file or the on-panel web page
- Third-party dependency list and redistribution notes: `THIRD_PARTY_LICENSES.md`
- Release process and open decisions: `PUBLISH.md`

---

_Shenzhen ZKSWE Technology Co., Ltd. · [www.zkswe.com](https://www.zkswe.com) · [developer.flythings.cn](https://developer.flythings.cn/)_
