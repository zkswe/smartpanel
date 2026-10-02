---
id: uicontrols-更正-z20-easyui2-6-setposition-的负-left-top-是字面量-不是
title: 更正：Z20/easyui2.6 setPosition 的负 left/top 是字面量（不是「从右/下算」），屏保浮层可以摆到负坐标
category: uicontrols
status: draft
confidence: unverified
verified_at: 2026-10-01
stale_days: 180
origin: project
source: 现场标定 smartpanel 屏保自定义布局
needs_evidence: true
platforms: [z20]
tags: [setPosition, 负坐标, LayoutPosition, 重启跑位, 屏保浮层, 拖动]
evidence:
  - {kind: real-device, cmd: python tools/_alm_dev/set_prefs.py --restart sp_ss_pos_almanac_x=-136 ... ; 截图 + node tools/png_bands.js <png> 量墨迹带, artifact: temp/_onlyalm.png / temp/_probe2.png / temp/_edit_neg.png（本仓库 temp/）}
---
# 更正：Z20/easyui2.6 setPosition 的负 left/top 是字面量（不是「从右/下算」），屏保浮层可以摆到负坐标

## 结论（2026-10-01 真机标定，钟工需求「支持负坐标、任意位置摆动」）

在 **Z20 / easyui 2.6.0** 上，`ZKBase::setPosition(LayoutPosition(x, y, w, h))` 的 **负 x/y 就是字面量坐标**，引擎**不会**把它换算成「从右/下算」的 bottom/right 值。

⇒ 运行期把控件摆到屏幕外（左/上为负）**是可行的**，不需要任何补偿；只要保证「落盘存的值」与「喂给 setPosition 的值」是同一套绝对坐标，就不会出现「重启后跑到另一侧」。

## 与旧结论的关系（`knowledge/uicontrols/json-layer-rules.md`）

旧文写「负的绝对坐标 = 从右/下算」（源自问题单 09251751-8「重启后控件跑位」）。本次三组探针**证伪了它对 setPosition 的适用性**：

| 控件 | 盒宽 | prefs 存 x | 引擎收到 | 实测左边界 | 若是「从右算」应为 |
|---|---|---|---|---|---|
| date | 480（=父宽） | -100 | -100 | **-101**（像素带 0..222，文本中心算得 -100） | -100（同值，不可分辨） |
| almanac | 400 | -136 | 先 -216 后 -136 | **-216**（像素带 0..152 ↔ 按 -216 算 0..153） | -136（像素带应到 233） |
| weather(icon 22) | 168 | -1 | -1 | **0..39 有墨**（右边界 x450..480 无墨） | 457..479 |

负 y 同理：almanac 传 y=-40，实测 line2/line3 按 y-10/−40 偏移后落在屏幕顶部（y0..34），**不是**落到 y=480-h-40=370 那一侧。

## 旧问题单 09251751-8 的真实成因（推断）

旧代码把「未设置」哨兵写成 **-1**，回读时漏判就变成 `setPosition(-1, -1, 480, 140)`。**对全宽/全高盒子**，-1 无论按哪种语义都会把盒子甩到贴右下 —— 症状「重启后贴右贴下」由此而来。现在把哨兵改成 -100000 并显式比较，根因即消除。

## 落地口径（本工程 SmartPanel）

- 内部模型统一「绝对坐标（可负）」，落盘/回读同一套；「未设置」哨兵 `SS_POS_UNSET = -100000`（**不能再用 -1**）。
- 拖动**不夹取**（任意位置，允许整块出屏）；只留 ±20000 的离谱值护栏。
- 兜底：编辑态**长按空白处 = 全部复位到默认位置**（摆出屏外后唯一能找回来的口子）。
- `ssEncLeft/ssEncTop` 保留为唯一 choke point（当前为恒等），万一下个 SDK 真变成「从右算」只改这两处。
- ⚠️ 本结论只覆盖 **setPosition（运行期）**；`ui/*.json` 里写负 left/top 本来就是合法写法（见旧文适用范围）。
