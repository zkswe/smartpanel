#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
#pragma once

/*
 * settingsLogic.cc -- 设置主页（settings.ftu）
 *
 * 多 Activity 口径：本页只碰自己的控件；行点击 = openActivity(子页) / openActivity(系统 WiFi 页)。
 * 子页已接：运行模式(modeActivity)/屏幕亮度/关屏/屏保显示/屏保视频/相册上传/情景模式(scenesActivity)。
 *
 * 【2026-10-01 重构】原 scrollwindow + 逐行手工控件 -> 单一 listview（caption LvSet）。
 *   背景：部分在网设备 EasyUI < 2.4，scrollwindow 控件不可用（整页截图空白），改为更老更稳的 listview。
 *   数据模型：kSetRows[] 扁平数组（HEADER 分组标题 / ROW 设置项），行值按 ConfigStore 实时计算，
 *   数据变更后 refreshListView() 全量重绘。行模板 7 个子项，按 kind 显隐。
 */

#include "utils/Log.h"
#include "system/NetKeeper.h"
#include "system/DisplayFlip.h"
#include "utils/TimeHelper.h"
#include "entry/EasyUIContext.h"
#include "storage/ConfigStore.h"
#include "storage/StoragePreferences.h"
#include "wall/WallPlayer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "window/ZKWindow.h"
#include "control/ZKTextView.h"
#include "control/ZKButton.h"
#include "control/ZKListView.h"

#define IDLE_ENTER_SEC 30          // 空闲回主页（心跳 tick 计时；不用墙钟，理由同 homeLogic）

// ── 涂鸦模式（钟工 2026-10-01）────────────────────────────
// 默认态：/mnt/extsd/EasyUI.cfg 停用（改名 EasyUI.cfg.disable）→ 启动器跳过 SD，回落 /res = 我们的程序。
// 切换态：把那份停用的 cfg 复制成 /tmp/EasyUI.cfg（查找优先级 /tmp 最高）→ setprop ctl.restart zkswe
//         → 本进程退出，重启后加载 SD 卡上的涂鸦程序。
// 回来：/tmp 是 tmpfs，**整机重启即清空** → 自动落回默认（我们的程序），无需任何常驻进程。
#define TUYA_LIB        "/mnt/extsd/lib/libzkgui.so"
#define TUYA_RES        "/mnt/extsd/ui/"
#define TUYA_TMP_CFG    "/tmp/EasyUI.cfg"
#define RESTART_CMD     "/bin/setprop ctl.restart zkswe"

// 涂鸦 cfg 源：按优先级取第一个存在的（.disable 是标准停用名，.disabled 兼容历史写法）
static const char* kTuyaCfgSrc[] = {
    "/mnt/extsd/EasyUI.cfg.disable",
    "/mnt/extsd/EasyUI.cfg.disabled",
    "/mnt/extsd/EasyUI.cfg",
};

static bool fileExists(const char* p) {
    if (p == NULL || p[0] == '\0') return false;
    FILE* f = fopen(p, "rb");
    if (f == NULL) return false;
    fclose(f);
    return true;
}

static const char* tuyaCfgSrc() {
    for (size_t i = 0; i < sizeof(kTuyaCfgSrc) / sizeof(kTuyaCfgSrc[0]); i++) {
        if (fileExists(kTuyaCfgSrc[i])) return kTuyaCfgSrc[i];
    }
    return NULL;
}

static bool tuyaProgramPresent() { return fileExists(TUYA_LIB); }

// 源 cfg 缺失时的兜底：按 SD 卡涂鸦程序的实际参数手写一份（照出厂 cfg 抄）
static const char* kTuyaCfgFallback =
    "{\n"
    "\t\"baud\":\"115200\",\n"
    "\t\"rotateTouch\":0,\n"
    "\t\"rotateScreen\":0,\n"
    "\t\"startupLibPath\":\"" TUYA_LIB "\",\n"
    "\t\"dictPinyinPath\":\"" TUYA_RES "ime/dict_pinyin.dat\",\n"
    "\t\"languageCode\":\"zh_CN\",\n"
    "\t\"defBrightness\":-2,\n"
    "\t\"screensaverTimeOut\":-1,\n"
    "\t\"touchDev\":\"/dev/input/event0\",\n"
    "\t\"languagePath\":\"/mnt/extsd/tr/\",\n"
    "\t\"uart\":\"ttyS1\",\n"
    "\t\"startupTouchCalib\":false,\n"
    "\t\"zkdebug\":true,\n"
    "\t\"font\":\"" TUYA_RES "SourceHanSansCN-Normal.ttf\",\n"
    "\t\"resPath\":\"" TUYA_RES "\"\n"
    "}\n";

static bool copyFile(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (in == NULL) return false;
    FILE* out = fopen(dst, "wb");
    if (out == NULL) { fclose(in); return false; }
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}

static bool writeTextFile(const char* dst, const char* text) {
    FILE* out = fopen(dst, "wb");
    if (out == NULL) return false;
    size_t len = strlen(text);
    bool ok = (fwrite(text, 1, len, out) == len);
    fclose(out);
    return ok;
}


static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {0, 1000},
};

static long long sLastActivityMs = 0;
static int sIdleTicks = 0;      // 空闲心跳计数（触摸归零）
static long long nowMs() { return TimeHelper::getCurrentTime(); }
static void noteActivity() { sLastActivityMs = nowMs(); sIdleTicks = 0; }

// ── 列表数据模型 ─────────────────────────────────────────
enum { K_HEADER = 0, K_ROW = 1 };
enum {
    ACT_NONE = 0, ACT_WIFI, ACT_MODE, ACT_NAME, ACT_SCENES, ACT_BRIGHT,
    ACT_OFF, ACT_SSSET, ACT_VIDEO, ACT_SSOUND, ACT_ALBUM, ACT_KEYSET, ACT_VER, ACT_FLIP, ACT_WALL,
    ACT_TUYA
};

struct SetRowDef {
    int kind;             // K_HEADER / K_ROW
    const char* header;   // K_HEADER 用：分组标题
    const char* label;    // K_ROW 用：行标签
    const char* icon;     // K_ROW 用：行图标
    int action;           // 行点击动作
};

// 与重构前的页面顺序 / 分组完全一致（设备 / 屏保 / 系统）
static const SetRowDef kSetRows[] = {
    { K_HEADER, "设备", NULL, NULL, ACT_NONE },
    { K_ROW, NULL, "WiFi",     "images/ic_wifi_g20.png",    ACT_WIFI },
    { K_ROW, NULL, "运行模式", "images/ic_refresh_g20.png", ACT_MODE },
    { K_ROW, NULL, "设备名称", "images/ic_tag_g20.png",     ACT_NAME },
    { K_ROW, NULL, "情景模式", "images/ic_sliders_g20.png", ACT_SCENES },
    { K_ROW, NULL, "屏幕亮度", "images/ic_bright_g20.png",  ACT_BRIGHT },
    { K_HEADER, "屏保", NULL, NULL, ACT_NONE },
    { K_ROW, NULL, "关屏设置", "images/ic_moon_g20.png",    ACT_OFF },
    { K_ROW, NULL, "按键配置", "images/ic_sliders_g20.png", ACT_KEYSET },
    { K_ROW, NULL, "屏保显示", "images/ic_screens_g20.png", ACT_SSSET },
    { K_ROW, NULL, "屏保视频", "images/ic_film_g20.png",    ACT_VIDEO },
    { K_ROW, NULL, "屏保声音", "images/ic_sound_g20.png",   ACT_SSOUND },
    { K_ROW, NULL, "相册上传", "images/ic_image_g20.png",   ACT_ALBUM },
    { K_HEADER, "系统", NULL, NULL, ACT_NONE },
    { K_ROW, NULL, "设备倒装", "images/ic_refresh_g20.png", ACT_FLIP },
    { K_ROW, NULL, "版本信息", "images/ic_info_g20.png",    ACT_VER },
    { K_ROW, NULL, "多屏拼接", "images/ic_screens_g20.png", ACT_WALL },
    { K_ROW, NULL, "涂鸦模式", "images/ic_gear_g20.png",    ACT_TUYA },
};
static const int kSetRowCount = (int)(sizeof(kSetRows) / sizeof(kSetRows[0]));

// 行值统一按 ConfigStore / 落盘键实时计算（子页保存返回后即时可见）
static void setRowValue(int action, char* b, int n) {
    ConfigStore* cfg = ConfigStore::getInstance();
    switch (action) {
    case ACT_WIFI: {
        std::string ip = NetKeeper_localIp();
        snprintf(b, n, "%s", ip.empty() ? "未连接" : (std::string("已连接 - ") + ip).c_str());
        break;
    }
    case ACT_MODE: {
        int m = cfg->runMode();
        snprintf(b, n, "%s",
                 (m == ConfigStore::MODE_MASTER) ? "本地主机 - 内嵌 Broker"
                 : (m == ConfigStore::MODE_SLAVE) ? "本地从机 - 接入主机"
                                                  : "HA 模式 - 订阅 HA");
        break;
    }
    case ACT_NAME:
        snprintf(b, n, "%s", cfg->deviceName().c_str());
        break;
    case ACT_SCENES: {
        bool ha = (cfg->runMode() == ConfigStore::MODE_HA);
        snprintf(b, n, "%s", ha ? "HA 情景 · 由 HA 执行" : "本机情景 · 可增删");
        break;
    }
    case ACT_BRIGHT:
        snprintf(b, n, "工作 %d%% - 屏保 %d%%", cfg->workBrightness(), cfg->screensaverBrightness());
        break;
    case ACT_OFF:
        if (!cfg->offPeriodValid()) {
            snprintf(b, n, "时段无效 - 待设置");
        } else {
            snprintf(b, n, "%s %02d:00-%02d:00 息屏",
                     cfg->offEnabled() ? "每天" : "已关闭", cfg->offStartHour(), cfg->offEndHour());
        }
        break;
    case ACT_SSSET: {
        // 6 项：时间/日期/温湿度/天气/设备状态条/黄历（顺序与 sssetLogic 的 kKeys 一致）
        const char* keys[6] = { "time", "date", "temphum", "weather", "statusbar", "almanac" };
        int on = 0;
        for (int i = 0; i < 6; i++) if (cfg->ssItemVisible(keys[i], true)) on++;
        snprintf(b, n, "%d 项开启 - 可增删", on);
        break;
    }
    case ACT_VIDEO: {
        int iv = cfg->videoIntervalSec();
        const char* t = (iv == 0) ? "连续轮播" : "按间隔轮播";
        snprintf(b, n, "%s - 图片 %d s", t, cfg->photoIntervalSec());
        break;
    }
    case ACT_SSOUND:
        if (cfg->ssSoundEnabled()) {
            snprintf(b, n, "已开启 - 音量 %d%%", cfg->ssVolume());
        } else {
            snprintf(b, n, "已静音");
        }
        break;
    case ACT_ALBUM: {
        bool on = StoragePreferences::getInt("sp_album_mode", 1) != 0;
        snprintf(b, n, "%s", on ? "相册模式已开启 - 点按进入" : "已关闭 - 点按进入");
        break;
    }
    case ACT_KEYSET: {
        // 主页按键显隐计数（keysetLogic 落盘 sw_show_1..3，默认显示）
        int on = 0;
        for (int i = 1; i <= 3; i++) {
            char k[16];
            snprintf(k, sizeof(k), "sw_show_%d", i);
            if (StoragePreferences::getInt(k, 1) != 0) on++;
        }
        snprintf(b, n, "%d 个显示 - 可增删", on);
        break;
    }
    case ACT_VER:
        snprintf(b, n, "%s", "SmartPanel v1.0 · Z20");
        break;
    case ACT_FLIP:
        snprintf(b, n, "%s", DisplayFlip::enabled() ? "开启 · 整屏已转 180°" : "关闭");
        break;
    case ACT_WALL: {
        bool en = StoragePreferences::getBool("sp_wall_en", false);
        int idx = StoragePreferences::getInt("sp_wall_idx", 1);
        int tot = StoragePreferences::getInt("sp_wall_n", 2);
        if (en) snprintf(b, n, "已开启 %d/%d", idx, tot);
        else    snprintf(b, n, "已关闭");
        break;
    }
    case ACT_TUYA:
        // 只反映「切过去可不可用」；当前当然在本机模式（涂鸦模式下本程序根本不运行）
        if (!tuyaProgramPresent()) snprintf(b, n, "%s", "未找到涂鸦程序");
        else                       snprintf(b, n, "%s", "本机模式 · 点按切到涂鸦");
        break;
    default:
        b[0] = '\0';
        break;
    }
}

// 数据变更后强制列表重绘（obtainListItemData 里禁止耗时操作，值都在这里算完再交给回调）
static void refreshList() {
    if (mLvSetPtr != NULL) mLvSetPtr->refreshListView();
}

// ── 涂鸦模式：二次确认弹窗 ───────────────────────────────
// 装饰层一律 touchable=false + touchPass=true，让底层两个 button 收到点击（同 scenes 页模态框口径）
static void passTuyaDeco() {
    ZKTextView* pass[] = {
        mImageTuyaDimPtr, mImageTuyaBoxPtr, mTextTuyaTitlePtr,
        mTextTuyaSubPtr, mTextTuyaHintPtr,
        mImageTuyaOkBgPtr, mImageTuyaCancelBgPtr,
    };
    for (size_t i = 0; i < sizeof(pass) / sizeof(pass[0]); i++) {
        if (pass[i] != NULL) {
            pass[i]->setTouchable(false);
            pass[i]->setTouchPass(true);
        }
    }
}

static void hideTuyaDlg() {
    if (mWindowTuyaPtr != NULL) mWindowTuyaPtr->hideWnd();
}

static void showTuyaDlg() {
    if (tuyaProgramPresent()) {
        if (mTextTuyaSubPtr != NULL)  mTextTuyaSubPtr->setText("将重启面板并进入 SD 卡上的涂鸦程序");
        if (mTextTuyaHintPtr != NULL) mTextTuyaHintPtr->setText("重启设备即可回到本机模式");
    } else {
        if (mTextTuyaSubPtr != NULL)  mTextTuyaSubPtr->setText("未找到 SD 卡上的涂鸦程序");
        if (mTextTuyaHintPtr != NULL) mTextTuyaHintPtr->setText("请先在 SD 卡安装涂鸦程序");
    }
    if (mWindowTuyaPtr != NULL) mWindowTuyaPtr->showWnd();
}

// 执行切换：写 /tmp/EasyUI.cfg（查找优先级最高）后重启应用进程 → 起来的是 SD 卡涂鸦程序
static void enterTuyaMode() {
    const char* src = tuyaCfgSrc();
    bool ok = (src != NULL) ? copyFile(src, TUYA_TMP_CFG)
                            : writeTextFile(TUYA_TMP_CFG, kTuyaCfgFallback);
    LOGI("tuya: switch src=%s -> %s ok=%d", src ? src : "(builtin)", TUYA_TMP_CFG, ok ? 1 : 0);
    if (!ok) return;
    LOGI("tuya: restart app via %s", RESTART_CMD);
    int rc = system(RESTART_CMD);       // 紧随其后本进程会被 init 杀掉，rc 仅用于消 -Wunused-result
    LOGI("tuya: setprop returned %d", rc);
}

// ── 系统回调 ─────────────────────────────────────────────
static void onUI_init() {
#ifdef FUN_BUILD
    INIT_UI_TIMERS
#endif // FUN_BUILD
    passTuyaDeco();          // 模态框装饰层触控穿透
    refreshList();
    noteActivity();
}

static void onUI_intent(const Intent *intentPtr) {
    if (intentPtr != NULL) {
        // 从子页返回时可带参数刷新行值
    }
}

static void onUI_show() {
    EASYUICONTEXT->setScreensaverEnable(false);   // 子页不进屏保（钟工 09251751-1）
    refreshList();          // 子页（关屏/亮度/屏保/视频/模式）保存返回后即时同步行值
    noteActivity();
}

static void onUI_hide() {
}

static void onUI_quit() {
    hideTuyaDlg();           // 离开本页时收起模态框（避免下次进来残留）
}

static void onProtocolDataUpdate(const SProtocolData &data) {
    (void)data;
}

static bool onUI_Timer(int id) {
    // 钟工 2026-09-25（问题单 09251751-1）：设置页**不进屏保**，也不再空闲自动回主页。
    // 原来这里的 30s 空闲 -> goHome 已移除（与「只有主页进屏保」口径一致）。
    (void)id;
    return true;
}

static bool onsettingsActivityTouchEvent(const MotionEvent &ev) {
    if (ev.mActionStatus == MotionEvent::E_ACTION_DOWN || ev.mActionStatus == MotionEvent::E_ACTION_MOVE) {
        noteActivity();          // DOWN/MOVE 都算活动（滑动列表时不被抢回主页）
    }
    return false;
}

// ── 列表三回调（每行复用同一模板，全量覆盖）─────────────
static int getListItemCount_LvSet(const ZKListView* pListView) {
    (void)pListView;
    return kSetRowCount;
}

static void obtainListItemData_LvSet(ZKListView* pListView, ZKListView::ZKListItem* pListItem, int index) {
    (void)pListView;
    if (pListItem == NULL || index < 0 || index >= kSetRowCount) return;
    const SetRowDef& d = kSetRows[index];
    bool isHeader = (d.kind == K_HEADER);

    ZKListView::ZKListSubItem* header  = pListItem->findSubItemByID(ID_SETTINGS_ItemHeader);
    ZKListView::ZKListSubItem* bg      = pListItem->findSubItemByID(ID_SETTINGS_ItemBg);
    ZKListView::ZKListSubItem* iconBg  = pListItem->findSubItemByID(ID_SETTINGS_ItemIconBg);
    ZKListView::ZKListSubItem* icon    = pListItem->findSubItemByID(ID_SETTINGS_ItemIcon);
    ZKListView::ZKListSubItem* label   = pListItem->findSubItemByID(ID_SETTINGS_ItemLabel);
    ZKListView::ZKListSubItem* value   = pListItem->findSubItemByID(ID_SETTINGS_ItemValue);
    ZKListView::ZKListSubItem* chev    = pListItem->findSubItemByID(ID_SETTINGS_ItemChevron);

    // 分组标题项：只显标题文字；设置项：显整行（行底/图标/标签/值/箭头）
    if (header) { header->setVisible(isHeader); if (isHeader) header->setText(d.header); }
    if (bg)     bg->setVisible(!isHeader);
    if (iconBg) iconBg->setVisible(!isHeader);
    if (icon)   icon->setVisible(!isHeader);
    if (label)  { label->setVisible(!isHeader); if (!isHeader) label->setText(d.label); }
    if (value)  value->setVisible(!isHeader);
    if (chev)   chev->setVisible(!isHeader);

    if (!isHeader) {
        if (icon) icon->setBackgroundPic(d.icon);
        char b[128] = {0};
        setRowValue(d.action, b, sizeof(b));
        if (value) value->setText(b);
    }
}

static void onListItemClick_LvSet(ZKListView* pListView, int index, int id) {
    (void)pListView;
    (void)id;   // 行内子项均不可点，整行点击即 index 对应的设置项
    if (index < 0 || index >= kSetRowCount) return;
    const SetRowDef& d = kSetRows[index];
    if (d.kind == K_HEADER) return;
    noteActivity();
    LOGD("settings: row click idx=%d action=%d (%s)", index, d.action, d.label ? d.label : "-");
    switch (d.action) {
    case ACT_WIFI:
        // 本板固件里的系统网络设置页 activity = NetSettingActivity（钟工 2026-09-25 报告：原来写
        // zkwifisettingActivity 在本板不存在 -> 点击无效）
        EASYUICONTEXT->openActivity("NetSettingActivity");
        break;
    case ACT_MODE:   EASYUICONTEXT->openActivity("modeActivity"); break;
    case ACT_NAME:   EASYUICONTEXT->openActivity("devnameActivity"); break;
    case ACT_SCENES: EASYUICONTEXT->openActivity("scenesActivity"); break;
    case ACT_BRIGHT: EASYUICONTEXT->openActivity("brightnessActivity"); break;
    case ACT_OFF:    EASYUICONTEXT->openActivity("offActivity"); break;
    case ACT_SSSET:  EASYUICONTEXT->openActivity("sssetActivity"); break;
    case ACT_VIDEO:  EASYUICONTEXT->openActivity("videoActivity"); break;
    case ACT_SSOUND:
        LOGD("settings: row 屏保声音 -> ssoundActivity");
        EASYUICONTEXT->openActivity("ssoundActivity");
        break;
    case ACT_ALBUM:  EASYUICONTEXT->openActivity("albumActivity"); break;
    case ACT_KEYSET: EASYUICONTEXT->openActivity("keysetActivity"); break;
    case ACT_WALL:   EASYUICONTEXT->openActivity("wallActivity"); break;
    case ACT_VER:    LOGD("version row (no action)"); break;
    case ACT_TUYA:   showTuyaDlg(); break;
    case ACT_FLIP: {
        // 设备倒装（钟工 2026-09-27 10:43）：点击即切换 + 立即生效（UI 转屏 / 触摸同转 / 视频层转）
        //   ！顺序讲究：先落盘 -> 停播放器（DISP 层属性不能在播放中改）-> 转 UI/触摸 -> 转视频层；
        //     播放器由屏保页心跳自动重起（那时代码里已带新旋转）。
        bool on = !DisplayFlip::enabled();
        StoragePreferences::putBool("sp_flip180", on);
        WallPlayer* wp = WallPlayer::getInstance();
        if (wp->running()) {
            LOGD("flip180: wall player running -> stop before MI layer rotate change");
            wp->stop();
        }
        DisplayFlip::applyUi();
        int r = DisplayFlip::applyVideoLayer();
        LOGI("flip180 toggled -> %d (ui+touch+videoLayer ret=%d)", on ? 1 : 0, r);
        refreshList();
        break;
    }
    default:
        break;
    }
}

static bool onButtonClick_ButtonSetBack(ZKButton *pButton) {
    (void)pButton;
    // 回主页：用 openActivity 而不是 goBack()（钟工 2026-10-01）
    //   正常栈 [main, home, settings] 下两者等价（openActivity 把已在栈中的主页移到顶层，
    //   其上的设置页出栈）；但走过一次「屏保显示 -> 编辑位置」之后不同 ——
    //   那一步是 openActivity("mainActivity")，而 mainActivity 是**启动页**，
    //   引擎会把它与它上面的页面一起重排（官方 wiki interaction/activity_lifecycle.md：
    //   "界面栈中存在即将要打开的界面 -> 只是将界面栈中对应的界面移动到顶层"），
    //   于是「主页」很可能已经不在栈里了；goBack() 只能弹到启动页为止
    //   （wiki："会将顶层的界面弹出，直到启动界面"）-> 按「返回」直接掉进屏保画面。
    //   openActivity 在"栈里还有"与"已被弹掉"两种情况下都落到主页，是幂等的。
    EASYUICONTEXT->openActivity("homeActivity");
    return true;
}

// 涂鸦模式弹窗：确认后写 /tmp 覆盖层并重启应用（进 SD 卡涂鸦程序）；整机重启即自动回到本机模式
static bool onButtonClick_ButtonTuyaOk(ZKButton *pButton) {
    (void)pButton;
    hideTuyaDlg();
    enterTuyaMode();
    return true;
}

static bool onButtonClick_ButtonTuyaCancel(ZKButton *pButton) {
    (void)pButton;
    hideTuyaDlg();
    return true;
}
