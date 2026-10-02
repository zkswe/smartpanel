#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
#pragma once

/*
 * scenesLogic.cc - 情景模式子页（scenes.ftu）
 *
 * 已定义情景列表（最多 8 行，可滚动）+ 每行右侧删除（二次确认弹窗）
 * + 新增情景（输入名 + 添加，校验空名/重名/超 6 字/已满 8 个）。
 * 数据源 = 业务单例 SceneManager（定义集合落在 ConfigStore 的 JSON 里，跨页共享）。
 *
 * 删除做法（SceneManager 没有 remove/delete 接口，成员 load/save 是 private）：
 *   用公开的 importScenesJson(json, merge=true) -- merge 模式的既定语义就是
 *   「删掉清单里没有的情景」。所以把「剩余情景」按完整字段（relays/devs/desc）
 *   序列化成 JSON 传进去，等于按名字删除；内存、持久化、UI 通知一次到位。
 *   主机模式下再 publishScenesConfig() 把新定义 retained 分发给从机。
 */

#include "utils/Log.h"
#include "entry/EasyUIContext.h"
#include "control/ZKButton.h"
#include "control/ZKTextView.h"
#include "control/ZKListView.h"
#include "control/ZKEditText.h"
#include "window/ZKWindow.h"
#include "storage/ConfigStore.h"
#include "scene/SceneManager.h"
#include "network/MqttBridge.h"
#include "storage/ConfigStore.h"
#include "network/LocalLink.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <stdio.h>
#include <string>
#include <vector>

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {0, 1000},
};

#define SCENE_ROW_MAX  8
#define SCENE_NAME_UNITS_MAX 6      // 新增情景名最多 6 个汉字（1 个 ASCII 记 0.55）

static std::string sDelName;        // 待删除的情景名（名称快照，删除后列表会前移）
static bool sHaList = false;         // 当前列表是否 HA 只读模式
static int  sListCount = 0;          // 当前列表行数（listview 数据源）

// v6（2026-10-01）：scrollwindow -> listview（部分设备 EasyUI<2.4 无 scrollwindow，listview 全版本可用）
//   行控件改为 listview 子项（见 obtainListItemData_LvScenes），不再持有逐行指针。

// UTF-8 安全按「显示宽度」截断（汉字 = 1.0，ASCII = 0.55），超宽场景名列表里不撑破行
static std::string fitUnits(const std::string& s, double maxUnits) {
    std::string out;
    double used = 0.0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t len = (c < 0x80) ? 1 : ((c < 0xE0) ? 2 : ((c < 0xF0) ? 3 : 4));
        if (i + len > s.size()) break;
        double w = (c < 0x80) ? 0.55 : 1.0;
        if (used + w > maxUnits) break;
        out.append(s, i, len);
        used += w;
        i += len;
    }
    return out;
}

static double textUnits(const std::string& s) {
    double used = 0.0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t len = (c < 0x80) ? 1 : ((c < 0xE0) ? 2 : ((c < 0xF0) ? 3 : 4));
        if (i + len > s.size()) break;
        used += (c < 0x80) ? 0.55 : 1.0;
        i += len;
    }
    return used;
}

static std::string trimText(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static void setErr(const char* text) {
    if (mTextSceneErrPtr != NULL) mTextSceneErrPtr->setText(text);
}

// 一行 = 名称（desc 优先，人读得懂）+ 键名与 3 路开关摘要
// HA 模式：隐藏新增入口（情景由 HA 管理）。删除钮在 listview 子项里按 sHaList 控制可见性。
static void hideEditControls() {
    if (mImageSceneAddBgPtr != NULL) mImageSceneAddBgPtr->setVisible(false);
    if (mImageSceneAddBtnBgPtr != NULL) mImageSceneAddBtnBgPtr->setVisible(false);
    if (mTextScenesAddLabelPtr != NULL) mTextScenesAddLabelPtr->setVisible(false);
    // 输入框也要一起藏：只藏按钮会留一个可见但不可用的「死输入框」（钟工 2026-09-26 逐页核对发现）
    if (mEditSceneAddPtr != NULL) mEditSceneAddPtr->setVisible(false);
    if (mTextScenesHintPtr != NULL) mTextScenesHintPtr->setText("HA 模式：情景由 HA 管理（本页仅展示）");
}

// v6：行内容改由 obtainListItemData_LvScenes 填充；此处只算数据源 + 触发刷新
static void refreshList() {
    bool ha = (ConfigStore::getInstance()->runMode() == ConfigStore::MODE_HA);
    int haN = ha ? MqttBridge::getInstance()->haSceneCount() : 0;
    sHaList = (ha && haN > 0);
    if (sHaList) {
        sListCount = (haN > SCENE_ROW_MAX) ? SCENE_ROW_MAX : haN;
        hideEditControls();
        LOGD("scenes: HA list refreshed, count=%d", haN);
    } else {
        std::vector<SceneDef> list = SceneManager::getInstance()->scenes();
        sListCount = (int)list.size();
        if (sListCount > SCENE_ROW_MAX) sListCount = SCENE_ROW_MAX;
        LOGD("scenes: list refreshed, count=%d", (int)list.size());
    }
    if (mLvScenesPtr != NULL) mLvScenesPtr->refreshListView();
}

static void passDecorations() {
    ZKTextView* pass[] = {
        mTextScenesTitlePtr, mTextScenesTitleEnPtr, mTextScenesListLabelPtr,
        mTextScenesAddLabelPtr, mImageSceneAddBgPtr, mImageSceneAddBtnBgPtr,
        mTextSceneErrPtr, mTextScenesHintPtr,
        mImageSceneDelDimPtr, mImageSceneDelBoxPtr, mTextSceneDelTitlePtr,
        mTextSceneDelSubPtr, mTextSceneDelNamePtr,
        mImageSceneDelOkBgPtr, mImageSceneDelCancelBgPtr,
    };
    for (size_t i = 0; i < sizeof(pass) / sizeof(pass[0]); i++) {
        if (pass[i] != NULL) {
            pass[i]->setTouchable(false);
            pass[i]->setTouchPass(true);
        }
    }
}

// 剩余情景（去掉 name 指定的那条）序列化成 JSON，交给 importScenesJson(merge=true)
static void removeSceneByName(const std::string& name) {
    SceneManager* sm = SceneManager::getInstance();
    std::vector<SceneDef> list = sm->scenes();
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    int kept = 0;
    for (size_t i = 0; i < list.size(); i++) {
        const SceneDef& def = list[i];
        if (def.name == name) continue;
        w.Key(def.name.c_str());
        w.StartObject();
        w.Key("relays");
        w.StartArray();
        for (int c = 0; c < 3; c++) w.Bool(def.relays[c]);
        w.EndArray();
        if (!def.devs.empty()) {
            w.Key("devs");
            w.StartObject();
            for (std::map<std::string, std::vector<bool> >::const_iterator it = def.devs.begin();
                 it != def.devs.end(); ++it) {
                w.Key(it->first.c_str());
                w.StartArray();
                for (size_t c = 0; c < it->second.size(); c++) w.Bool(it->second[c]);
                w.EndArray();
            }
            w.EndObject();
        }
        w.Key("desc");
        w.String(def.desc.c_str());
        w.EndObject();
        kept++;
    }
    w.EndObject();
    std::string json(sb.GetString(), sb.GetSize());
    sm->importScenesJson(json, true);            // merge=true -> 清单外情景被删除
    if (ConfigStore::getInstance()->runMode() == ConfigStore::MODE_MASTER) {
        LocalLink::getInstance()->publishScenesConfig();     // 主机：retained 分发新定义
    }
    LOGD("scenes: removed '%s', kept=%d", name.c_str(), kept);
}

static bool sceneNameExists(const std::string& name) {
    std::vector<SceneDef> list = SceneManager::getInstance()->scenes();
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].name == name) return true;
        if (!list[i].desc.empty() && list[i].desc == name) return true;
    }
    return false;
}

// ── 系统回调 ─────────────────────────────────────────────
static void onUI_init() {
#ifdef FUN_BUILD
    INIT_UI_TIMERS
#endif // FUN_BUILD
    SceneManager::getInstance()->init();     // 首次启动灌出厂情景（幂等）
    passDecorations();
    refreshList();
    setErr("");
}

static void onUI_intent(const Intent *intentPtr) { (void)intentPtr; }

static void onUI_show() {
    EASYUICONTEXT->setScreensaverEnable(false);   // 子页不进屏保（钟工 09251751-1）
    SceneManager::getInstance()->init();
    refreshList();
    setErr("");
}

static void onUI_hide() {}
static void onUI_quit() {}
static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }
static bool onUI_Timer(int id) { (void)id; return true; }
static bool onscenesActivityTouchEvent(const MotionEvent &ev) { (void)ev; return false; }

// ── 回调 ─────────────────────────────────────────────────
static bool onButtonClick_ButtonScenesBack(ZKButton *p) {
    (void)p;
    EASYUICONTEXT->goBack();
    return true;
}

// 删除：先弹二次确认（同一页模态），确认后才真删
static void askDelete(int idx) {
    std::vector<SceneDef> list = SceneManager::getInstance()->scenes();
    if (idx < 0 || idx >= (int)list.size()) return;
    sDelName = list[idx].name;
    if (mTextSceneDelNamePtr != NULL) {
        std::string show = fitUnits(list[idx].desc.empty() ? list[idx].name : list[idx].desc, 14.0);
        mTextSceneDelNamePtr->setText(show.c_str());
    }
    if (mWindowSceneDelPtr != NULL) mWindowSceneDelPtr->showWnd();
}

// ── listview 三回调（替代原 8 个删除按钮回调）────────────────────────
static int getListItemCount_LvScenes(const ZKListView* pListView) {
    (void)pListView;
    return sListCount;
}

static void obtainListItemData_LvScenes(ZKListView* pListView, ZKListView::ZKListItem* pListItem, int index) {
    (void)pListView;
    if (index < 0 || index >= sListCount) return;
    ZKListView::ZKListSubItem* name = pListItem->findSubItemByID(ID_SCENES_ItemName);
    ZKListView::ZKListSubItem* desc = pListItem->findSubItemByID(ID_SCENES_ItemDesc);
    ZKListView::ZKListSubItem* del  = pListItem->findSubItemByID(ID_SCENES_ItemDel);
    std::string title, ds;
    if (sHaList) {
        std::string nm = MqttBridge::getInstance()->haSceneName(index);
        title = fitUnits(nm, 16.0);
        ds = "HA 情景 - 由 HA 执行";
    } else {
        std::vector<SceneDef> list = SceneManager::getInstance()->scenes();
        if (index >= (int)list.size()) return;
        const SceneDef& def = list[index];
        title = def.desc.empty() ? def.name : def.desc;
        title = fitUnits(title, 16.0);
        char b[128], s1[8], s2[8], s3[8];
        snprintf(s1, sizeof(s1), "%s", def.relays[0] ? "开" : "关");
        snprintf(s2, sizeof(s2), "%s", def.relays[1] ? "开" : "关");
        snprintf(s3, sizeof(s3), "%s", def.relays[2] ? "开" : "关");
        snprintf(b, sizeof(b), "%s - 灯1 %s - 灯2 %s - 灯3 %s", def.name.c_str(), s1, s2, s3);
        ds = b;
    }
    if (name) name->setText(title.c_str());
    if (desc) desc->setText(ds.c_str());
    if (del) del->setVisible(!sHaList);   // HA 模式隐藏删除钮
}

static void onListItemClick_LvScenes(ZKListView* pListView, int index, int id) {
    (void)pListView;
    if (index < 0 || index >= sListCount) return;
    if (id == ID_SCENES_ItemDel) {
        askDelete(index);   // 删除钮：二次确认（与原行为一致）
        return;
    }
    // 行主体点击无动作（原 scrollwindow 行主体也无 handler）
}

static bool onButtonClick_ButtonSceneDelOk(ZKButton *p) {
    (void)p;
    if (mWindowSceneDelPtr != NULL) mWindowSceneDelPtr->hideWnd();
    if (sDelName.empty()) return true;
    std::vector<SceneDef> list = SceneManager::getInstance()->scenes();
    if (list.size() <= 1) {                       // 至少留一个，否则重启会灌回出厂情景
        setErr("至少保留一个情景");
        LOGW("scenes: refuse to remove last scene");
        sDelName.clear();
        return true;
    }
    removeSceneByName(sDelName);
    sDelName.clear();
    refreshList();
    setErr("");
    return true;
}

static bool onButtonClick_ButtonSceneDelCancel(ZKButton *p) {
    (void)p;
    sDelName.clear();
    if (mWindowSceneDelPtr != NULL) mWindowSceneDelPtr->hideWnd();
    return true;
}

static bool onButtonClick_ButtonSceneAdd(ZKButton *p) {
    (void)p;
    std::string name = (mEditSceneAddPtr != NULL) ? trimText(mEditSceneAddPtr->getText())
                                                  : std::string();
    if (name.empty()) {
        setErr("情景名不能为空");
        return true;
    }
    if (textUnits(name) > (double)SCENE_NAME_UNITS_MAX) {
        setErr("情景名最多 6 个字");
        return true;
    }
    std::vector<SceneDef> list = SceneManager::getInstance()->scenes();
    if ((int)list.size() >= SCENE_ROW_MAX) {
        setErr("最多 8 个情景");
        return true;
    }
    if (sceneNameExists(name)) {
        setErr("情景名已存在");
        return true;
    }
    bool off[3] = { false, false, false };        // 新情景默认全关，之后按需编辑
    std::string desc = name + " - 自定义";
    SceneManager::getInstance()->defineScene(name, off, desc);   // 内部 save + 主机分发
    if (mEditSceneAddPtr != NULL) mEditSceneAddPtr->setText("");
    setErr("");
    refreshList();
    LOGD("scenes: defined '%s'", name.c_str());
    return true;
}

static void onEditTextChanged_EditSceneAdd(const std::string &text) {
    (void)text;
    setErr("");
}
