/*
 * wallLogic.cc -- 「多屏拼接」设置页（wall.ftu）
 * 钟工 2026-09-25：多面板拼接联动（同一时刻同一帧）。本页只做配置与状态展示：
 *   拼接联动 开/关 · 组名 · 本机序号 · 排布(1x2/1x3/1x4) · 角色(主机/从机) · 片段时长 · 对齐状态
 * 配置存 /data：
 *   sp_wall_en     0/1
 *   sp_wall_group  组名（默认 zksw-wall）—— v5：在 /mnt/sdnand/wall 下真实存在的组目录间循环
 *   sp_wall_idx    本机序号 1..N（决定播 seg_<idx>）
 *   sp_wall_n      排布台数 N（2/3/4 -> 1x2/1x3/1x4）
 *   sp_wall_role   1=主机 0=从机
 *   sp_wall_seg_ms 片段时长（毫秒，来自切块工具 wall.json）
 * 播放与协议在 src/wall/（WallLink）里，本页只读写配置 + 触发其重载。
 *
 * v5（2026-09-27，钟工）一行不动结构，只改文案与行为：
 *   行2「组名」：点一下在 /mnt/sdnand/wall 下真实存在的组目录之间循环（当前值不在列表则放首位）
 *               —— “组名自己定义”= 用切块工具/用户建的目录名；目录扫不到给提示文案。
 *   行6「片段时长」改成「播放内容」：值显示 `<K> 段 · 总 <X.X>s`（读 playlist.json）；
 *               旧布局（无 playlist.json）显示 `1 段 · <X.X>s`。
 *   状态行：`状态：拼接中（本机 seg_<idx>/<N> · <K> 段轮播 · 主机/从机）`
 *   保存：触发 WallLink::init() 重载清单（组名/序号/排布变更同样立即重载）。
 */
#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD

/**
 * @brief 串口数据回调接口
 */
static void onProtocolDataUpdate(const SProtocolData &data) {
  LOGD_TRACE("");
}
#pragma once

#include "utils/Log.h"
#include "entry/EasyUIContext.h"
#include "storage/StoragePreferences.h"
#include "control/ZKButton.h"
#include "control/ZKTextView.h"
#include "control/ZKListView.h"
#include "wall/WallLink.h"

#include <stdio.h>
#include <string>
#include <vector>

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {0, 1000},
};

#define WK_EN     "sp_wall_en"
#define WK_GROUP  "sp_wall_group"
#define WK_IDX    "sp_wall_idx"
#define WK_N      "sp_wall_n"
#define WK_ROLE   "sp_wall_role"
#define WK_SEGMS  "sp_wall_seg_ms"

static bool wkEn()    { return StoragePreferences::getBool(WK_EN, false); }
static int  wkIdx()   { int v = StoragePreferences::getInt(WK_IDX, 1); return v < 1 ? 1 : v; }
static int  wkN()     { int v = StoragePreferences::getInt(WK_N, 2); return (v < 2 || v > 4) ? 2 : v; }
static bool wkMaster() { return StoragePreferences::getInt(WK_ROLE, 1) != 0; }
static std::string wkGroup() { return StoragePreferences::getString(WK_GROUP, "zksw-wall"); }
static int wkSegMs() { return StoragePreferences::getInt(WK_SEGMS, 0); }

// v5：本页要的 playlist 统计（读 <组目录>/playlist.json；不改动播放状态，可对任意组名查询）
static bool wkPlaylist(WallLink::PlaylistInfo* pi) {
    return WallLink::queryPlaylist(wkGroup(), wkN(), pi);
}

// v5：组目录清单（/mnt/sdnand/wall 下真实存在的目录）
static std::vector<std::string> wkGroups() { return WallLink::listGroups(); }

// v5：盘上有没有组目录（扫不到 -> 状态行给提示文案）
static bool wkHasGroupDir() { return !wkGroups().empty(); }

// 行6「播放内容」的值：playlist -> `<K> 段 · 总 <X.X>s`；旧布局 -> `1 段 · <X.X>s`
static void wkPlayContentText(char* b, size_t n) {
    WallLink::PlaylistInfo pi;
    if (wkPlaylist(&pi)) {
        snprintf(b, n, "%d 段 · 总 %.1fs", pi.clips, pi.totalMs / 1000.0);
        return;
    }
    if (wkSegMs() > 0) snprintf(b, n, "1 段 · %.1fs", wkSegMs() / 1000.0);
    else snprintf(b, n, "1 段 · 未设置");
}

// v6（2026-10-01）：scrollwindow -> listview（部分设备 EasyUI<2.4 无 scrollwindow，listview 全版本可用）
//   行模板三子项：ItemBg(背景图) / ItemLabel(左标签) / ItemValue(右值)；数据由 obtainListItemData 逐行填充。
static const char* WK_LABELS[6] = {
    "拼接联动", "组名", "本机序号", "排布", "角色", "播放内容"
};

// 行6「播放内容」文案缓存（避免 obtainListItemData 在滚动时反复读盘）
static std::string gWkPlayContent = "1 段 · 未设置";

// 取第 row(1..6) 行的值文本（与旧 wkVal 逻辑等价）
static void wkRowValue(int row, char* b, size_t n) {
    switch (row) {
    case 1: snprintf(b, n, "%s", wkEn() ? "开启" : "关闭"); break;
    case 2: snprintf(b, n, "%s", wkGroup().c_str()); break;
    case 3: snprintf(b, n, "%d / %d", wkIdx(), wkN()); break;
    case 4: snprintf(b, n, "1x%d", wkN()); break;
    case 5: snprintf(b, n, "%s", wkMaster() ? "主机" : "从机"); break;
    case 6: snprintf(b, n, "%s", gWkPlayContent.c_str()); break;
    default: b[0] = '\0'; break;
    }
}

static void wkRefresh() {
    char b[128];
    wkPlayContentText(b, sizeof(b));                     // v5：行6 = 播放内容（K 段 · 总 X.Xs），先缓存
    gWkPlayContent = b;
    if (mLvWallPtr != NULL) mLvWallPtr->refreshListView();   // v6：行值改由 obtainListItemData 填充

    if (mTextWallStatusPtr != NULL) {
        WallLink::PlaylistInfo pi;
        const bool plOk = wkPlaylist(&pi);
        const int clips = plOk ? pi.clips : 1;
        if (!wkEn()) {
            mTextWallStatusPtr->setText("状态：未开启（屏保按原清单轮播）");
        } else if (!wkHasGroupDir()) {
            // 组目录扫不到：明确提示（别再让用户对着“拼接中”猜为什么没画面）
            mTextWallStatusPtr->setText("状态：未找到组目录（请先用切块工具生成）");
        } else {
            snprintf(b, sizeof(b), "状态：拼接中（本机 seg_%d/%d · %d 段轮播 · %s）",
                     wkIdx(), wkN(), clips, wkMaster() ? "主机" : "从机");
            mTextWallStatusPtr->setText(b);
        }
    }
}

// ── 生命周期 ───────────────────────────────────────────────────────
static void onUI_init() {
#ifdef FUN_BUILD
    INIT_UI_TIMERS
#endif // FUN_BUILD
    wkRefresh();
}

static void onUI_intent(const Intent* intentPtr) { (void)intentPtr; }

static void onUI_show() {
    EASYUICONTEXT->setScreensaverEnable(false);   // 子页不进屏保（钟工 09251751-1）
    wkRefresh();
    LOGD("wall: page shown (en=%d idx=%d/%d master=%d group=%s playlist=%d clips=%d)",
         wkEn() ? 1 : 0, wkIdx(), wkN(), wkMaster() ? 1 : 0, wkGroup().c_str(),
         WallLink::getInstance()->playlistMode() ? 1 : 0, WallLink::getInstance()->clipCount());
}

static void onUI_hide() {}
static void onUI_quit() {}
static bool onUI_Timer(int id) { (void)id; return true; }
static bool onwallActivityTouchEvent(const MotionEvent& ev) { (void)ev; return false; }

// v5：组名/序号/排布变了 -> 立刻重读该组 playlist（清单一变，行6/状态与播放器口径都要跟着变）。
//   WallLink::init() 幂等：UDP 通道已起就不重复 bind；主机侧重发一次 epoch 无害。
static void wkReloadLink(const char* why) {
    WallLink::getInstance()->init();
    LOGD("wall: link reloaded (%s) -> group=%s playlist=%d clips=%d",
         why, wkGroup().c_str(), WallLink::getInstance()->playlistMode() ? 1 : 0,
         WallLink::getInstance()->clipCount());
}

// ── 行点击：循环/切换取值（配置立即落盘，保存按钮只做确认与返回）──────
static bool wkRowTap(int row) {
    switch (row) {
    case 1:
        StoragePreferences::putBool(WK_EN, !wkEn());
        break;
    case 2: {
        // v5（2026-09-27）：组名自定义 —— 在 /mnt/sdnand/wall 下**真实存在**的组目录之间循环；
        //   当前值不在列表里则放首位（保证“至少能切走”）；一个目录都没有 -> 不改配置，只提示。
        const std::string cur = wkGroup();
        std::vector<std::string> gs = wkGroups();
        if (gs.empty()) {
            LOGW("wall: 组名切换失败 —— %s 下没有可用组目录（请先用切块工具生成再选）",
                 WallLink::wallRoot().c_str());
            break;
        }
        std::vector<std::string> cand;
        bool has = false;
        for (size_t i = 0; i < gs.size(); i++) if (gs[i] == cur) has = true;
        if (!has) cand.push_back(cur);                 // 当前值不在列表 -> 放首位
        for (size_t i = 0; i < gs.size(); i++) cand.push_back(gs[i]);
        size_t at = 0;
        for (size_t i = 0; i < cand.size(); i++) if (cand[i] == cur) at = i;
        const std::string next = cand[(at + 1) % cand.size()];
        StoragePreferences::putString(WK_GROUP, next);
        LOGD("wall: group cycled %s -> %s (candidates=%d, dirs=%d)",
             cur.c_str(), next.c_str(), (int)cand.size(), (int)gs.size());
        wkReloadLink("group changed");
        break;
    }
    case 3: {
        int n = wkN(), i = wkIdx() + 1;
        if (i > n) i = 1;
        StoragePreferences::putInt(WK_IDX, i);
        wkReloadLink("idx changed");                   // 本机序号决定播哪一段 -> 重载
        break;
    }
    case 4: {
        int n = wkN() + 1;
        if (n > 4) n = 2;
        StoragePreferences::putInt(WK_N, n);
        if (wkIdx() > n) StoragePreferences::putInt(WK_IDX, n);
        wkReloadLink("layout changed");
        break;
    }
    case 5:
        StoragePreferences::putInt(WK_ROLE, wkMaster() ? 0 : 1);
        break;
    case 6:
        // 行6「播放内容」：只读展示（K 段 · 总 X.Xs）——时长由清单决定，页面上不可改（工具侧控制）
        LOGD("wall: row6 (play content) is display-only (playlist-driven)");
        break;
    default:
        break;
    }
    wkRefresh();
    LOGD("wall: row %d tapped -> en=%d group=%s idx=%d/%d master=%d",
         row, wkEn() ? 1 : 0, wkGroup().c_str(), wkIdx(), wkN(), wkMaster() ? 1 : 0);
    return true;
}

// ── listview 三回调（替代原 6 个按钮行回调）────────────────────────
static int getListItemCount_LvWall(const ZKListView* pListView) {
    (void)pListView;
    return 6;
}

static void obtainListItemData_LvWall(ZKListView* pListView, ZKListView::ZKListItem* pListItem, int index) {
    (void)pListView;
    if (index < 0 || index > 5) return;
    ZKListView::ZKListSubItem* label = pListItem->findSubItemByID(ID_WALL_ItemLabel);
    ZKListView::ZKListSubItem* value = pListItem->findSubItemByID(ID_WALL_ItemValue);
    if (label) label->setText(WK_LABELS[index]);
    char b[128];
    wkRowValue(index + 1, b, sizeof(b));
    if (value) value->setText(b);
}

static void onListItemClick_LvWall(ZKListView* pListView, int index, int id) {
    (void)pListView;
    (void)id;   // 行内无独立可点子项，整行点击即 index 对应的配置项
    if (index < 0 || index > 5) return;
    wkRowTap(index + 1);   // 内部会改配置并 wkRefresh() -> refreshListView()
}

static bool onButtonClick_ButtonWallBack(ZKButton* p) { (void)p; EASYUICONTEXT->goBack(); return true; }
static bool onButtonClick_ButtonWallBack2(ZKButton* p) { (void)p; EASYUICONTEXT->goBack(); return true; }

static bool onButtonClick_ButtonWallSave(ZKButton* p) {
    (void)p;
    // v5：保存 = 按最终配置重载清单（组名/序号/排布/角色都可能在本次会话里改过）
    wkReloadLink("save");
    LOGD("wall: saved en=%d group=%s idx=%d/%d master=%d seg=%dms playlist=%d clips=%d total=%lldms",
         wkEn() ? 1 : 0, wkGroup().c_str(), wkIdx(), wkN(), wkMaster() ? 1 : 0, wkSegMs(),
         WallLink::getInstance()->playlistMode() ? 1 : 0, WallLink::getInstance()->clipCount(),
         WallLink::getInstance()->totalMs());
    EASYUICONTEXT->goBack();
    return true;
}
