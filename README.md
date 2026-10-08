# SmartPanel · 4 寸 86 盒智能面板

**中文** ｜ [English](README.en.md)

[![淘宝 · 整机购买](https://img.shields.io/badge/%E6%B7%98%E5%AE%9D-%E6%95%B4%E6%9C%BA%E8%B4%AD%E4%B9%B0-FF5000?style=flat-square)](https://item.taobao.com/item.htm?id=650865599184&skuId=5500508256776) [![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=flat-square)](LICENSE) [![Built with FlyThings MCP](https://img.shields.io/badge/Built%20with-FlyThings%20MCP-0A7AFF?style=flat-square)](https://github.com/zkswe/mcp_release)

> **GitHub 主仓** <https://github.com/zkswe/smartpanel> ｜ **Gitee 镜像** <https://gitee.com/Kwolve/SmartPanel>

---

## 一句话：让墙上的开关，变成云相框和电子鱼缸

一块标准 **86 盒**的 **4 寸 480×480 Linux 触控面板**：装上时它是 **3 路 10 A 继电器 + 情景中控**
（**原生接入 Home Assistant**，Domoticz 同样吃得下）；闲下来它是一块**云相框**、一个**电子鱼缸**——
手机扫码传图自动轮播，或循环播放一段视频，墙面就是一幅会动的画；
**2–4 台还能拼成一堵视频墙**，撑起门店与展厅的门面。

**上电 3 秒可用，现场扫码 30 秒配好，不用拆机、不用带电脑。**

> 🤖 **这是一套「AI 开发」的固件**：UI 布局、业务逻辑、真机验收全部是 **AI 助手 + FlyThings MCP** 做出来的。
> 二次开发同样是「说一句话」的活 —— **不用啃框架、不用装 IDE**（详见 §7）。

> 🛒 **整机购买（淘宝）**：<https://item.taobao.com/item.htm?id=650865599184&skuId=5500508256776>
>
> 型号 **SW48480040D1** ｜ 主控 **Z20 / SigmaStar SSD201·SSD202D** ｜ 许可 **MIT**

---

<table>
  <tr>
    <td align="center"><img src="docs/shots/01_home.png" width="150"><br><sub><b>主界面</b><br>3 路继电器 + 情景</sub></td>
    <td align="center"><img src="docs/shots/02_album_qr.png" width="150"><br><sub><b>相册</b><br>扫码传图</sub></td>
    <td align="center"><img src="docs/shots/03_settings.png" width="150"><br><sub><b>设置</b><br>全部功能一览</sub></td>
    <td align="center"><img src="docs/shots/04_screensaver_video.png" width="150"><br><sub><b>屏保</b><br>视频 / 图片轮播</sub></td>
    <td align="center"><img src="docs/shots/05_ha_config_qr.png" width="150"><br><sub><b>HA 配置</b><br>扫码填服务器与令牌</sub></td>
  </tr>
  <tr>
    <td colspan="5" align="center"><img src="docs/shots/10_wall_3split.png" width="860"><br><sub><b>三屏拼接</b> · 同一段视频按屏切段，相位对齐 + 校时（1×3 横排，机间丢弃 52 px）</sub></td>
  </tr>
</table>

---

## 1. 为什么选它

| # | 卖点 | 对买家意味着什么 |
|---|---|---|
| 1 | **一块顶三块** | 取代 3 路墙面开关 + 一个情景面板。少开槽、少买面板、少接线、少人工——一个 86 盒装完 |
| 2 | **原生 Home Assistant / Domoticz** | 上电连上 broker，HA 里**自动冒出 3 个开关实体**（名字就是面板上起的名字）。**零代码接入**，不用写一行集成 |
| 3 | **手机扫码，30 秒交付** | 装完不拆机：面板显示二维码 → 手机扫码打开板内网页 → 粘贴服务器地址与令牌 → 立即生效 |
| 4 | **多块拼一堵墙** | 2–4 台横排 = 一整幅大画面。门店背景墙、展厅循环展示、广告位，**不依赖任何服务器**（主机广播时间基准） |
| 5 | **它不只是开关** | 闲下来就是一块**云相框**（手机扫码传图、自动轮播）、一个**电子鱼缸**（循环视频，像一幅会动的画）；夜景时段整块熄屏；亮度分「工作 / 屏保」两档 |
| 6 | **现场好装、量产好发** | 标准 86 盒嵌入式；**一键 180° 倒装**（整屏 + 触摸 + 视频一起转，天花板下也能装）；出厂默认文件预置，**开箱不配置也能上线** |
| 7 | **AI 开发 —— 开发难度低、流程简单** | 这个工程本身就是 **AI 助手 + FlyThings MCP** 开发的：改界面 / 加功能 / 编译出包 / 抓真机屏验收，**全在对话里完成**。**不养嵌入式 UI 团队、不用先啃完框架文档**，全套源码 MIT 开源 |

---

## 2. 三大核心场景

### 2.1 家庭墙面：3 路继电器 + 一键盘活全屋情景

三张设备卡（**客厅灯 / 卧室灯 / 灯带**，名字随便改），底部一排情景（**回家 / 观影 / 睡眠 / 休闲**）。

| 元素 | 说明 |
|---|---|
| 设备卡 | **点整卡 = 开关该路继电器**，状态文字即时变化并向 HA 上报 |
| 情景条 | **点情景 = 执行整组联动**（回家 = 客厅灯 + 灯带同时亮），最多 8 个 |
| 双向同源 | HA 点开关 → 面板继电器动作；面板触摸 → HA 状态同步；**状态唯一事实源在板内**，两边永不说两套话 |
| 手机 / 语音也能控 | HA App、语音助手走标准 MQTT，面板照做 |

### 2.2 商用小屏墙：多台拼成一整幅画面

N 台同型号面板横排，屏保态播放同一段视频，每台只显示属于自己的那一块，**画面同帧同步（实测相位差 ≤ 10 ms）**。

- **网页切块工具**：PC 浏览器里拖框选画面区域（每屏可独立微调，补偿物理拼缝）→ 导出 `seg_1..N.mp4 + playlist.json` → **一键分发**到各屏
- **不依赖服务器**：同网段 UDP 自组网，主机广播时间基准
- 可放**一组多视频轮播**，做门店铺面 / 展厅循环 / 广告位

启动：`python tools/video_wall/web/server.py --port 8796` → 浏览器打开 `http://<PC>:8796/`

<img src="docs/release_shots/07_video_wall_tool_ui.png" width="720" alt="切块工具">

### 2.3 云相框 / 电子鱼缸：让墙面有气氛

它不工作的时候，才是它最出彩的时候：

- **云相框**：面板显示二维码 → 手机扫码传图（也支持微信小程序）→ 照片进面板，自动轮播
- **电子鱼缸 / 动态画框**：选一段视频当屏保循环播放——水族箱、落雪、城市夜色、品牌宣传片，墙面就是一幅会动的画
- 素材也可直接丢 **TF 卡 / U 盘**（即插即用），换内容不用重配置
- 屏保还能叠内容：**大时钟 + 日期 +（可选）温湿度 / 天气 / 底部状态条**，项可增删、可拖动摆位
- 轮播节奏可配：图片每张 N 秒（默认 15 s）、视频间隔（0 = 播完即切）

---

## 3. 规格参数

| 项目 | 规格 |
|---|---|
| 产品型号 | **SW48480040D1**（4 寸 86 盒系列，Z20 平台） |
| 屏 | 4 寸方形液晶 **480 × 480**，全贴合单点电容触控 |
| 主控 / 系统 | Z20（SigmaStar SSD201 / SSD202D，Cortex-A7 双核 1.2 GHz，内置 128 MB DDR3），FlyThings V2.1 + EasyUI |
| 存储 | 16 MB NOR + **128 MB SD NAND**（素材 / 相册放这里） |
| 供电 | **AC 220 V 强电** 或 **DC 9–24 V 弱电**（二选一，按现场底盒走线） |
| 继电器 | 最多 **3 路 10 A**（本工程用于客厅灯 / 灯带 / 茶台） |
| 无线 | 内置 **WiFi**；可选 Zigbee / 蓝牙模块 |
| 有线 | **RS485** + **百兆以太网** |
| 音频 | 内置 1 W 功放小喇叭 |
| 传感（可选） | 光感、人体测距 |
| 安装 | 标准 **86 盒**底盒，墙面嵌入；**正装 / 倒装（上下颠倒）都支持** |
| 开机 | 约 **3 秒** |

---

## 4. 三种运行模式（按场景选一种）

| 模式 | 适合谁 | 怎么工作 |
|---|---|---|
| **HA 模式** | 已有 Home Assistant / Domoticz 的家庭 | 接外部 broker（EMQX / Mosquitto），MQTT Discovery 自动出 3 个开关实体；情景交给 HA 统一管理 |
| **本地主机** | 没上 HA，想自己组网 | 板内直接起一个轻量 MQTT broker，同网段的从机 / 小程序直连 |
| **本地从机** | 多面板联动 | 连到同网段主机的 broker |

> 本地 broker 为自研轻量实现：QoS0/1、retained、Will、通配全支持，RSS ~1 MB；**不支持** TLS / QoS2 / 持久会话。

---

## 5. 现场 3 步上手

> 前提：面板与 HA 在同一局域网；HA 已配置 MQTT 集成并连上一台 broker。

**第 1 步 · 上电接线**：强电版接 220 V 火零（继电器输出接灯具回路），弱电版接 9–24 V 直流。**断电作业，先确认空开断开。**

**第 2 步 · 配网**：主页 → 右上角齿轮 → 设置 → **WiFi** → 选 SSID → 输入密码 → 连上后返回（有线场景直接走百兆网口）。

**第 3 步 · 选 HA 模式并扫码填服务器**：设置 → 运行模式 → **HA 模式** → 面板下方出现二维码 → 手机扫码打开板内网页：

<img src="docs/release_shots/06_web_config_page.png" width="260" alt="板内配置网页">

- **服务器地址**：`mqtt://192.0.2.188:1883`（只写 IP 会自动补 `:1883`）
- **用户名 / 密码（令牌）**：长令牌直接粘贴（自动去换行空格）
- 点**保存** → 面板立刻重连，「连接状态」变 **已连接**
- 回到 HA → 设置 → 设备与服务 → MQTT，设备 **SmartHomePanel** 自动出现，带 **3 个开关**（客厅灯 / 卧室灯 / 灯带）

**要改服务器 / 口令**：重复第 3 步即可（扫码 → 改 → 保存），**不用拆机、不用带电脑**。

> **量产 / 出厂**：随固件带一份 `panel-defaults.json`（放 `/mnt/sdnand` 或 `/res/etc`），设备首次开机自动写入配置 —— **开箱不配置也能上 HA**。见 `docs/FACTORY-DEFAULTS.md`。

---

## 6. 购买与联系

| 渠道 | 入口 |
|---|---|
| 🛒 **淘宝 · 整机购买** | <https://item.taobao.com/item.htm?id=650865599184&skuId=5500508256776> |
| 🏢 公司 | **深圳中科世为科技有限公司**（ZKSWE） |
| 🌐 官网 / 技术文档 | <https://www.zkswe.com> ｜ <https://developer.flythings.cn/> |
| 📞 电话 | 0755-23019045 |
| 📍 地址 | 广东省深圳市宝安区西乡街道共乐社区凤凰智谷 A 座 1407 室 |

> 批量采购 / 定制型号（尺寸、继电器路数、通信方式、屏配色）请走公司与电话联系人；开源仓只提供软件与文档。

---

## 7. 二次开发：AI 直接开发（FlyThings MCP）

**这是本工程最大的「隐藏卖点」：开发难度低、开发流程简单** —— 整个仓库就是这么开发出来的，
换你来做同样简单。

### 一个开发回合 = 3 步

| 步 | 谁做 | 干什么 |
|---|---|---|
| ① **说需求** | 你 | 「把主页第三张卡换成窗帘图标，尺寸不变」 |
| ② **动手** | AI | 查内置知识库 → 改 `ui/*.json` / 逻辑代码 → 出 HTML 预览稿给你确认 |
| ③ **验收** | AI | `fui pack` → `fun build -p Z20` → 推断真机 → **抓真机屏 + 像素比对**（±2 容差）→ 把截图发给你 |

### 你会用到的能力（全程由 AI 代劳）

| 你说的话 | AI 能做到 |
|---|---|
| 「帮我改这个页面」 | 可拖拽编辑器改布局 → 变更写回 json → 打包 |
| 「这个控件怎么配」 | **全离线知识库检索**（本地向量 + BM25，**不需要任何 API Key**），结论带来源与置信度 |
| 「编译一下推真机」 | `fun install` → `fun build` → 自动探测设备 → 推送运行 |
| 「现在长什么样」 | 真机抓屏（含视频层抓帧） |
| 「改完对不对」 | 像素 diff 回归（零 token 验收） |
| 「帮我出个升级包」 | `update.img` 固化打包（TF 卡 / ADB / OTA 多种刷法） |

### 接入只要一步

把 **FlyThings MCP** 装进你的 AI 客户端（Trae / Cursor / Claude Desktop / Kimi…）。
其 **release 版单独维护、单独发布**，本仓只引用、不复制。

| 用途 | 路径 |
|---|---|
| **release 版（用户获取 / 安装处，主）** | <https://github.com/zkswe/mcp_release> |
| 国内镜像（Gitee） | <https://gitee.com/Kwolve/flythingsmcp_release> |
| 本机路径（同工作区校验用，可选） | `tools/FlyThings_mcp_release/` |

安装与接入方式以该 release 仓的 README 为准（当前 release 版本 `0.27.134-open`，43 个工具）。

**最快上手**：对 AI 说 → 「帮我克隆并安装 `https://github.com/zkswe/mcp_release`」。

---

## 8. 工程结构

```
src/
  Main.cpp                 程序入口
  activity/                主 Activity
  logic/                   页面逻辑（home/main/settings/scenes/album/video/wall/off…）
  device/                  RelayManager（继电器唯一事实源）、SensorManager
  network/                 MqttBridge（HA 模式）、LocalLink（本地主/从机）、
                           MiniBroker（内嵌 broker，回退）、LocalBrokerService、
                           WebConfigServer（板内配置网页）、PanelLink、TcpReceive
  storage/ConfigStore      全部配置 / 持久化（prefs）
  wall/                    多屏拼接（WallLink 时钟对齐 / WallPlayer 播放）
ui/                        *.json 布局 + *.ftu 产物（ftu 由 fui pack 生成）
resources/images/          UI 图片资源
font/                      中文字库（裁剪字库：不支持 emoji / 特殊符号）
tools/                     多屏切块（网页版）、出厂默认预置、无设备回归冒烟
docs/                      发布说明 / 整机说明书 / 多屏拼接 / HA 配置 / Domoticz 兼容 / 出厂默认
```

---

## 9. 构建与部署

```bash
fun install                                   # 拉依赖（改 Manifest 后必须重跑）
fun build -p Z20                              # 编译 → .fun/z20 或 .fsc/z20 下 libzkgui.so
fun pack -p Z20 --startup-dir /res --release-version 1.0.N   # 出 update.img（U 盘 / TF 升级）
```

调试期热替换（不动系统分区，断电回落原厂）：

```bash
adb push libzkgui.so /tmp/lib/libzkgui.so
# /tmp/EasyUI.cfg 里 startupLibPath 指到 /tmp/lib/libzkgui.so（**不能带 BOM**，带 BOM 会被忽略）
adb shell "setprop ctl.restart zkswe"
```

⚠️ 设备端排查前先 `adb push tools/busybox/bin/z20/busybox /tmp/busybox`（设备 shell 没有 free/grep/nohup 等）。

---

## 10. MQTT 主题约定

```
smartpanel/<deviceId>/availability                  online / offline（LWT）
smartpanel/<deviceId>/status                        设备状态 JSON（retained）
smartpanel/<deviceId>/switch/relay_<1..3>/state     继电器状态 ON/OFF（retained）
smartpanel/<deviceId>/switch/relay_<n>/command      外部命令 ON/OFF → 面板执行
smartpanel/broadcast/scene                          本机模式：情景激活
smartpanel/ha/scenes                                HA 下发情景清单（retained）
homeassistant/switch/<uid>_<relay>/config           HA Discovery（HA 模式）
```

---

## 11. 文档索引

| 主题 | 文档 |
|---|---|
| 整机说明书（含全部真机截图） | `docs/整机说明书.md` |
| 多屏拼接专项 | `docs/说明书-多屏拼接.md`、`docs/wall-linkage-design.md` |
| HA 配置（扫码板内网页） | `docs/HA-CONFIG-WEB.md` |
| Domoticz 兼容（实测结论） | `docs/DOMOTICZ-COMPAT.md` |
| 出厂默认 / 量产预置 | `docs/FACTORY-DEFAULTS.md` |
| 发布说明（效果图合集） | `docs/发布说明.md` |
| 第三方依赖与许可 | `THIRD_PARTY_LICENSES.md`、`PUBLISH.md` |

---

## 12. 验收与质量

- 面板 UI 像素级回归：`tools/ui_tools/ui_diff.py`（±2 容差）
- 板内网页配置：`tools/webcfg_test/`（x86 替身冒烟，**无需设备**）
- 多设备跑批：MCP `flythings_test_run`（用例 JSON + logcat 断言）

---

## 13. 已知限制（先说清楚，免得踩坑）

- 面板本体**没有温湿度传感器**（屏保数据来自网关推送；要「传感器进 HA」由传感器侧按标准 discovery 发布即可）
- 板内 broker（本机模式）**不支持** TLS / QoS2 端到端 / 持久会话
- 拼接要求**各屏时间对齐**（NTP 或主机 epoch 广播），建议同组固件版本一致
- Domoticz 侧**情景没有等价物**（走 HA 自动化；Domoticz 需自行用 dzVents/Lua 接）
- 走网线时，面板网页二维码自动取 `eth0` 地址；无 IP 时提示「等待面板联网」
- 第三方包 `base-utility` 的播放器释放路径带 `drop_caches`（视频段循环时约每 10 s 触发一次，拉高负载）—— 已知问题，规避方式是不每次切段都销毁播放器

---

## 14. 开源与许可

- 本项目与应用层代码：**MIT**（见 `LICENSE`）；板内 MQTT 服务同 MIT
- 代码内**不含任何内网地址 / 账号**：现场参数走出厂默认文件或板内网页扫码配置
- 第三方依赖清单与再分发口径：`THIRD_PARTY_LICENSES.md`
- 发布流程与待拍板事项：`PUBLISH.md`

---

_深圳中科世为科技有限公司（ZKSWE）· [www.zkswe.com](https://www.zkswe.com) · [developer.flythings.cn](https://developer.flythings.cn/)_
