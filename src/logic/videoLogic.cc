#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
#pragma once

/*
 * videoLogic.cc - 屏保视频子页（video.ftu）
 *
 * 素材 = 数据分区里的 **mp4 + 相册上传的图片**（/mnt/sdnand/video 优先，再 /mnt/sdnand/album，
 * 退 /mnt/extsd/video、/mnt/usb1/video）；图片也进列表（混播，设计说明书 3.4.5：图片每张 15 秒）。
 * 选中清单 + 轮播间隔**存 /data**（StoragePreferences，钟工 2026-09-24 口径）：
 *   sp_video_sel = 选中的绝对路径，'\n' 分隔（空 = 全选）
 *   sp_video_int = 轮播间隔秒（0=连续）
 * 屏保页（main.ftu）按同一份配置轮播。
 */

#include "utils/Log.h"
#include "entry/EasyUIContext.h"
#include "storage/ConfigStore.h"
#include "storage/StoragePreferences.h"
#include "system/MediaGuard.h"     // 已标记「不可播」素材的列出/取消标记
#include "logic/ui_text.h"         // setTextIfChanged（判据 = 控件实际文本）
#include "control/ZKButton.h"
#include "control/ZKTextView.h"
#include "control/ZKListView.h"

#include <stdio.h>
#include <ctype.h>
#include <string>
#include <vector>

#define KEY_VIDEO_SEL "sp_video_sel"
#define KEY_VIDEO_INT "sp_video_int"
// ★ 2026-10-01：原来是 12，且"可播"和"已标记 .bad"共用这个上限 —— 实测被撑爆：
//   /video 9 条可播 + 2 个 .bad，/album 第 1 条可播就凑满 12 → 手机新上传的第 2、3 条
//   **整条不进列表**（钟工："手机小程序上传的视频在屏保视频选择页面看不到"）。
//   listview 本身可滚动，不需要硬上限；改 64 只当兜底，并让可播项先占位。
#define MAX_ROWS      64

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {0, 1000},
};

static const int kIntervalSec[6] = { 0, 3600, 7200, 21600, 43200, 86400 };

static std::vector<std::string> sFiles;
static std::vector<std::string> sSel;      // 选中路径（空 = 全选）
// 已标记「不可播」的素材（*.bad）：由屏保侧的 MediaGuard 标记（编码不支持/容器损坏/播放失败）。
// 列表里照样列出来但置灰、不可选中，**点一下 = 取消标记**（改名还原后下次扫描回到可播行）。
static std::vector<std::string> sBad;
static int sInterval = 0;

// v6（2026-10-01）：scrollwindow -> listview（部分设备 EasyUI<2.4 无 scrollwindow，listview 全版本可用）
//   行控件改为 listview 子项（见 obtainListItemData_LvVid），不再持有逐行指针。

// 图片素材（与 mainLogic 的 isImagePath 同一套扩展名）
static bool isImageFile(const std::string& p) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = p.substr(dot);
    for (size_t i = 0; i < e.size(); i++) e[i] = (char)tolower((unsigned char)e[i]);
    return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".bmp" || e == ".gif";
}
static ZKTextView* intBg(int i) {
    ZKTextView* a[6] = { mImageVidIntBg1Ptr, mImageVidIntBg2Ptr, mImageVidIntBg3Ptr,
                         mImageVidIntBg4Ptr, mImageVidIntBg5Ptr, mImageVidIntBg6Ptr };
    return (i >= 0 && i < 6) ? a[i] : NULL;
}
static ZKTextView* intText(int i) {
    ZKTextView* a[6] = { mTextVidInt1Ptr, mTextVidInt2Ptr, mTextVidInt3Ptr,
                         mTextVidInt4Ptr, mTextVidInt5Ptr, mTextVidInt6Ptr };
    return (i >= 0 && i < 6) ? a[i] : NULL;
}

// 图片显示时长 chips（钟工 2026-09-25：可配，5s/10s/15s/30s/60s）
static const int kPhotoSec[5] = { 5, 10, 15, 30, 60 };
static int sPhotoSec = 15;

static ZKTextView* photoBg(int i) {
    ZKTextView* a[5] = { mImagePhotoDurBg1Ptr, mImagePhotoDurBg2Ptr, mImagePhotoDurBg3Ptr,
                         mImagePhotoDurBg4Ptr, mImagePhotoDurBg5Ptr };
    return (i >= 0 && i < 5) ? a[i] : NULL;
}
static ZKTextView* photoText(int i) {
    ZKTextView* a[5] = { mTextPhotoDur1Ptr, mTextPhotoDur2Ptr, mTextPhotoDur3Ptr,
                         mTextPhotoDur4Ptr, mTextPhotoDur5Ptr };
    return (i >= 0 && i < 5) ? a[i] : NULL;
}

static void scanVideos() {
    sFiles.clear();
    sBad.clear();
    const char* dirs[] = { "/mnt/sdnand/video", "/mnt/sdnand/album", "/mnt/extsd/video", "/mnt/usb1/video" };
    // ★ 两趟扫（2026-10-01）：**先收全部"可播"素材，再收"已标记 .bad"**。
    //   原来是一趟（每个目录里 可播→图片→.bad 依次收），/video 的 2 个 .bad 会先占掉名额，
    //   把 /album 里手机新上传的素材挤出去 —— 现场就是这么"看不到小程序上传的视频"的。
    const char* patsOk[]  = { "*.mp4", "*.avi", "*.mkv", "*.mov",
                              "*.jpg", "*.jpeg", "*.png", "*.bmp" };
    const char* patsBad[] = { "*.mp4.bad", "*.avi.bad", "*.mkv.bad", "*.mov.bad" };
    for (int pass = 0; pass < 2; pass++) {
        const char** pats = (pass == 0) ? patsOk : patsBad;
        const size_t pn = (pass == 0) ? (sizeof(patsOk) / sizeof(patsOk[0]))
                                      : (sizeof(patsBad) / sizeof(patsBad[0]));
        for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
            for (size_t q = 0; q < pn; q++) {
                std::string cmd = std::string("ls ") + dirs[d] + "/" + pats[q] + " 2>/dev/null";
                FILE* fp = popen(cmd.c_str(), "r");
                if (fp == NULL) continue;
                char line[512];
                while (fgets(line, sizeof(line), fp)) {
                    std::string p = line;
                    while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
                    if (p.empty()) continue;
                    if (sFiles.size() + sBad.size() >= (size_t)MAX_ROWS) continue;
                    if (pass == 1) sBad.push_back(p);
                    else           sFiles.push_back(p);
                }
                pclose(fp);
            }
        }
    }
    LOGD("videoLogic: found %d playable + %d marked-bad", (int)sFiles.size(), (int)sBad.size());
}

// ★ 素材目录签名（2026-10-01）：进页面时自动刷新用 —— 手机/小程序上传后不必退出重进。
//   只做**一次** popen（列目录 + 大小），比每 2s 全量重扫（十几个 popen）便宜得多。
static std::string mediaSig() {
    std::string sig;
    FILE* fp = popen("ls -l /mnt/sdnand/video /mnt/sdnand/album /mnt/extsd/video /mnt/usb1/video 2>/dev/null", "r");
    if (fp == NULL) return sig;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        for (const char* c = line; *c != '\0'; ++c) sig.push_back(*c);
    }
    pclose(fp);
    return sig;
}
static std::string sLastSig;
static int sSigTicks = 0;

// 列表总行数 = 可播 + 已标记（已标记排在最后）
static int rowCount() { return (int)(sFiles.size() + sBad.size()); }

static void loadConfig() {
    sSel.clear();
    std::string all = StoragePreferences::getString(KEY_VIDEO_SEL, "");
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        std::string one = all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!one.empty()) sSel.push_back(one);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    sInterval = StoragePreferences::getInt(KEY_VIDEO_INT, 0);
    sPhotoSec = ConfigStore::getInstance()->photoIntervalSec();   // 钟工：图片时长可配（默认 15s）
}

static bool isSelected(int i) {
    if (i < 0 || i >= (int)sFiles.size()) return false;
    if (sSel.empty()) return true;                 // 空 = 全选
    for (size_t k = 0; k < sSel.size(); k++) {
        if (sSel[k] == sFiles[i]) return true;
    }
    return false;
}

static void saveConfig() {
    std::string all;
    for (size_t k = 0; k < sSel.size(); k++) {
        all += sSel[k];
        all += '\n';
    }
    StoragePreferences::putString(KEY_VIDEO_SEL, all);
    LOGD("videoLogic: saved %d selected", (int)sSel.size());
}

static void refreshRows() {
    if (mLvVidPtr != NULL) mLvVidPtr->refreshListView();   // v6：行内容改由 obtainListItemData_LvVid 填充
}

// 列表上方提示行（TextVidListHint）：本来留空，这里让它说明「有几个素材 / 有几个被标记」
static void refreshHint() {
    if (mTextVidListHintPtr == NULL) return;
    char b[96];
    if (!sBad.empty()) {
        snprintf(b, sizeof(b), "已标记 %d 个不可播放，点击该行可恢复", (int)sBad.size());
        setTextIfChanged(mTextVidListHintPtr, b);
    } else if (!sFiles.empty()) {
        snprintf(b, sizeof(b), "共 %d 个素材，点一下选择或取消", (int)sFiles.size());
        setTextIfChanged(mTextVidListHintPtr, b);
    } else {
        setTextIfChanged(mTextVidListHintPtr, "没有找到素材");
    }
}

static void refreshInterval() {
    for (int i = 0; i < 6; i++) {
        bool on = (kIntervalSec[i] == sInterval);
        ZKTextView* bg = intBg(i);
        ZKTextView* tx = intText(i);
        if (bg != NULL) bg->setBackgroundPic(on ? "images/chip68x44_hl.png" : "images/chip68x44.png");
        if (tx != NULL) tx->setTextColor(on ? 0x10141A : 0xECECF0);
    }
}

// 图片显示时长 chip 高亮（选中 = 绿底深字）
static void refreshPhotoDur() {
    for (int i = 0; i < 5; i++) {
        bool on = (kPhotoSec[i] == sPhotoSec);
        ZKTextView* bg = photoBg(i);
        ZKTextView* tx = photoText(i);
        if (bg != NULL) bg->setBackgroundPic(on ? "images/chip68x44_hl.png" : "images/chip68x44.png");
        if (tx != NULL) tx->setTextColor(on ? 0x10141A : 0xECECF0);
    }
}

// 重新读配置 + 重扫素材 + 刷新全部 UI（onUI_init/onUI_show 与"页面内自动刷新"共用）
static void rescanAll() {
    loadConfig();
    scanVideos();
    refreshRows();
    refreshHint();
    refreshInterval();
    refreshPhotoDur();
}

static void passDecorations() {
    ZKTextView* pass[] = {
        mTextVidTitlePtr, mTextVidTitleEnPtr, mTextVidListHintPtr, mTextVidIntLabelPtr,
        mImageVidIntBg1Ptr, mImageVidIntBg2Ptr, mImageVidIntBg3Ptr,
        mImageVidIntBg4Ptr, mImageVidIntBg5Ptr, mImageVidIntBg6Ptr,
        mTextVidInt1Ptr, mTextVidInt2Ptr, mTextVidInt3Ptr,
        mTextVidInt4Ptr, mTextVidInt5Ptr, mTextVidInt6Ptr, mTextPhotoDurLabelPtr,
        mImagePhotoDurBg1Ptr, mImagePhotoDurBg2Ptr, mImagePhotoDurBg3Ptr,
        mImagePhotoDurBg4Ptr, mImagePhotoDurBg5Ptr,
        mTextPhotoDur1Ptr, mTextPhotoDur2Ptr, mTextPhotoDur3Ptr,
        mTextPhotoDur4Ptr, mTextPhotoDur5Ptr,
    };
    for (size_t i = 0; i < sizeof(pass) / sizeof(pass[0]); i++) {
        if (pass[i] != NULL) {
            pass[i]->setTouchable(false);
            pass[i]->setTouchPass(true);
        }
    }
}

// ── 系统回调 ─────────────────────────────────────────────
static void onUI_init() {
#ifdef FUN_BUILD
    INIT_UI_TIMERS
#endif // FUN_BUILD
    passDecorations();
    rescanAll();
    sLastSig = mediaSig();
    sSigTicks = 0;
}

static void onUI_intent(const Intent *intentPtr) { (void)intentPtr; }

static void onUI_show() {
    EASYUICONTEXT->setScreensaverEnable(false);   // 子页不进屏保（钟工 09251751-1）
    rescanAll();
    sLastSig = mediaSig();                        // 记签名：页面里自动刷新用（见 onUI_Timer）
    sSigTicks = 0;
}

static void onUI_hide() {
}

static void onUI_quit() {
}

static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

// ★ 页面里自动刷新（2026-10-01，钟工："手机小程序上传的视频看不到，应该是没有刷新到"）：
//   原来只在 onUI_show 扫一次 —— 用户上传完不退出页面就永远看不到（而且以前还有 12 行硬上限）。
//   这里每 2s 比一次目录签名，变了就重扫并重建列表。
static bool onUI_Timer(int id) {
    if (id != 0) return true;
    if (++sSigTicks < 2) return true;             // 2s 一次
    sSigTicks = 0;
    const std::string sig = mediaSig();
    if (sig == sLastSig) return true;
    sLastSig = sig;
    LOGD("videoLogic: media changed -> rescan（页面内自动刷新）");
    rescanAll();
    return true;
}

static bool onvideoActivityTouchEvent(const MotionEvent &ev) { (void)ev; return false; }

// ── 回调 ─────────────────────────────────────
static bool onButtonClick_ButtonVidBack(ZKButton *pButton) {
    (void)pButton;
    EASYUICONTEXT->goBack();
    return true;
}

static void toggleRow(int i) {
    if (i < 0 || i >= (int)sFiles.size()) return;
    if (sSel.empty()) {                    // 首次点击：把全选展开成显式清单
        for (size_t k = 0; k < sFiles.size(); k++) sSel.push_back(sFiles[k]);
    }
    for (size_t k = 0; k < sSel.size(); k++) {
        if (sSel[k] == sFiles[i]) {
            sSel.erase(sSel.begin() + k);
            saveConfig();
            refreshRows();
            return;
        }
    }
    sSel.push_back(sFiles[i]);
    saveConfig();
    refreshRows();
}

// ── listview 三回调（替代原 12 个按钮行回调）────────────────────────
static int getListItemCount_LvVid(const ZKListView* pListView) {
    (void)pListView;
    return rowCount();
}

static void obtainListItemData_LvVid(ZKListView* pListView, ZKListView::ZKListItem* pListItem, int index) {
    (void)pListView;
    if (index < 0 || index >= rowCount()) return;
    const bool bad = (index >= (int)sFiles.size());            // 未尾若干行 = 已标记不可播
    const std::string& f = bad ? sBad[index - (int)sFiles.size()] : sFiles[index];
    ZKListView::ZKListSubItem* name  = pListItem->findSubItemByID(ID_VIDEO_ItemName);
    ZKListView::ZKListSubItem* idx   = pListItem->findSubItemByID(ID_VIDEO_ItemIdx);
    ZKListView::ZKListSubItem* badge = pListItem->findSubItemByID(ID_VIDEO_ItemBadge);
    ZKListView::ZKListSubItem* chk   = pListItem->findSubItemByID(ID_VIDEO_ItemCheck);
    if (name) {
        size_t slash = f.find_last_of('/');
        name->setText((slash == std::string::npos ? f : f.substr(slash + 1)).c_str());
        // 已标记的置灰（次要文字色），可播的按是否选中区分
        name->setTextColor(bad ? 0x8A8A93 : (isSelected(index) ? 0x7BE0A3 : 0xECECF0));
    }
    if (idx) {
        char b[16];   // %02d 最坏情况需 12 字节（含结束符），8 会触发 -Werror=format-truncation
        snprintf(b, sizeof(b), "%02d", index + 1);
        idx->setText(b);
    }
    if (badge) {
        badge->setBackgroundPic(bad ? "images/badge_bad.png"
                                    : (isImageFile(f) ? "images/badge_image.png" : "images/badge_video.png"));
    }
    if (chk) chk->setVisible(!bad && isSelected(index));       // 已标记的不可选中
}

static void onListItemClick_LvVid(ZKListView* pListView, int index, int id) {
    (void)pListView;
    (void)id;
    if (index < 0 || index >= rowCount()) return;
    if (index >= (int)sFiles.size()) {                         // 已标记行：点击 = 取消标记（改名还原）
        const std::string p = sBad[index - (int)sFiles.size()];
        if (MediaGuard::unmark(p)) {
            LOGI("videoLogic: 取消标记 %s", p.c_str());
        } else {
            LOGW("videoLogic: 取消标记失败 %s", p.c_str());
        }
        loadConfig();
        scanVideos();
        refreshRows();
        refreshHint();
        return;
    }
    toggleRow(index);   // 内部 saveConfig + refreshRows() -> refreshListView
}

static bool onButtonClick_ButtonVidIntN(int idx) {
    if (idx < 0 || idx > 5) return true;
    sInterval = kIntervalSec[idx];
    StoragePreferences::putInt(KEY_VIDEO_INT, sInterval);
    refreshInterval();
    return true;
}
static bool onButtonClick_ButtonVidInt1(ZKButton *p) { (void)p; return onButtonClick_ButtonVidIntN(0); }
static bool onButtonClick_ButtonVidInt2(ZKButton *p) { (void)p; return onButtonClick_ButtonVidIntN(1); }
static bool onButtonClick_ButtonVidInt3(ZKButton *p) { (void)p; return onButtonClick_ButtonVidIntN(2); }
static bool onButtonClick_ButtonVidInt4(ZKButton *p) { (void)p; return onButtonClick_ButtonVidIntN(3); }
static bool onButtonClick_ButtonVidInt5(ZKButton *p) { (void)p; return onButtonClick_ButtonVidIntN(4); }
static bool onButtonClick_ButtonVidInt6(ZKButton *p) { (void)p; return onButtonClick_ButtonVidIntN(5); }

// 图片显示时长（钟工 2026-09-25：可配）
static bool onButtonClick_ButtonPhotoDurN(int idx) {
    if (idx < 0 || idx > 4) return true;
    sPhotoSec = kPhotoSec[idx];
    ConfigStore::getInstance()->setPhotoIntervalSec(sPhotoSec);
    LOGD("videoLogic: photo duration -> %d s", sPhotoSec);
    refreshPhotoDur();
    return true;
}
static bool onButtonClick_ButtonPhotoDur1(ZKButton *p) { (void)p; return onButtonClick_ButtonPhotoDurN(0); }
static bool onButtonClick_ButtonPhotoDur2(ZKButton *p) { (void)p; return onButtonClick_ButtonPhotoDurN(1); }
static bool onButtonClick_ButtonPhotoDur3(ZKButton *p) { (void)p; return onButtonClick_ButtonPhotoDurN(2); }
static bool onButtonClick_ButtonPhotoDur4(ZKButton *p) { (void)p; return onButtonClick_ButtonPhotoDurN(3); }
static bool onButtonClick_ButtonPhotoDur5(ZKButton *p) { (void)p; return onButtonClick_ButtonPhotoDurN(4); }
