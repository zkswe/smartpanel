#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
#pragma once

/*
 * sssetLogic.cc - 屏保显示子页（ssset.ftu）
 * 6 项显隐（时间/日期/温湿度/天气/设备状态条/黄历）存 /data：ss_item_<key> = 1/0。
 * 屏保页（mainLogic）按同一批键决定浮层显隐。
 */

#include "utils/Log.h"
#include "entry/EasyUIContext.h"
#include "storage/ConfigStore.h"
#include "storage/StoragePreferences.h"
#include "control/ZKButton.h"
#include "control/ZKTextView.h"

#include <stdio.h>
#include <string>

#define KEY_PREFIX "ss_item_"
#define SS_SET_N   6

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {0, 1000},
};

static const char* kKeys[SS_SET_N]   = { "time", "date", "temphum", "weather", "statusbar", "almanac" };
static const char* kLabels[SS_SET_N] = { "时间", "日期", "温湿度", "天气", "设备状态条", "黄历" };

static ZKTextView* chipBg(int i) {
    ZKTextView* a[SS_SET_N] = { mImageSsSetChip1Ptr, mImageSsSetChip2Ptr, mImageSsSetChip3Ptr,
                                mImageSsSetChip4Ptr, mImageSsSetChip5Ptr, mImageSsSetChip6Ptr };
    return (i >= 0 && i < SS_SET_N) ? a[i] : NULL;
}
static ZKTextView* chipTx(int i) {
    ZKTextView* a[SS_SET_N] = { mTextSsSetRowValue1Ptr, mTextSsSetRowValue2Ptr, mTextSsSetRowValue3Ptr,
                                mTextSsSetRowValue4Ptr, mTextSsSetRowValue5Ptr, mTextSsSetRowValue6Ptr };
    return (i >= 0 && i < SS_SET_N) ? a[i] : NULL;
}
static ZKButton* rowBtn(int i) {
    ZKButton* a[SS_SET_N] = { mButtonSsSetRow1Ptr, mButtonSsSetRow2Ptr, mButtonSsSetRow3Ptr,
                              mButtonSsSetRow4Ptr, mButtonSsSetRow5Ptr, mButtonSsSetRow6Ptr };
    return (i >= 0 && i < SS_SET_N) ? a[i] : NULL;
}

static bool itemVisible(int i) {
    std::string k = std::string(KEY_PREFIX) + kKeys[i];
    return StoragePreferences::getInt(k, 1) != 0;      // 默认全显示
}

static void refreshRows() {
    for (int i = 0; i < SS_SET_N; i++) {
        bool on = itemVisible(i);
        ZKTextView* bg = chipBg(i);
        ZKTextView* tx = chipTx(i);
        if (bg != NULL) bg->setBackgroundPic(on ? "images/chip68x44_hl.png" : "images/chip68x44.png");
        if (tx != NULL) {
            tx->setText(on ? "显示" : "隐藏");
            tx->setTextColor(on ? 0x10141A : 0xECECF0);
        }
    }
}

static void toggleRow(int i) {
    if (i < 0 || i >= SS_SET_N) return;
    std::string k = std::string(KEY_PREFIX) + kKeys[i];
    int next = itemVisible(i) ? 0 : 1;
    StoragePreferences::putInt(k, next);
    LOGD("ssset: %s -> %d", kKeys[i], next);
    refreshRows();
}

static void passDecorations() {
    ZKTextView* pass[] = {
        mTextSsSetTitlePtr, mTextSsSetTitleEnPtr,
        mImageSsSetRowBg1Ptr, mImageSsSetRowBg2Ptr, mImageSsSetRowBg3Ptr,
        mImageSsSetRowBg4Ptr, mImageSsSetRowBg5Ptr, mImageSsSetRowBg6Ptr,
        mTextSsSetRowLabel1Ptr, mTextSsSetRowLabel2Ptr, mTextSsSetRowLabel3Ptr,
        mTextSsSetRowLabel4Ptr, mTextSsSetRowLabel5Ptr, mTextSsSetRowLabel6Ptr,
        mImageSsSetChip1Ptr, mImageSsSetChip2Ptr, mImageSsSetChip3Ptr,
        mImageSsSetChip4Ptr, mImageSsSetChip5Ptr, mImageSsSetChip6Ptr,
        mTextSsSetRowValue1Ptr, mTextSsSetRowValue2Ptr, mTextSsSetRowValue3Ptr,
        mTextSsSetRowValue4Ptr, mTextSsSetRowValue5Ptr, mTextSsSetRowValue6Ptr,
        mTextSsSetHint1Ptr, mImageSsSetEditBgPtr,
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
    refreshRows();
}

static void onUI_intent(const Intent *intentPtr) { (void)intentPtr; }
static void onUI_show() {
    EASYUICONTEXT->setScreensaverEnable(false);   // 子页不进屏保（钟工 09251751-1）
    refreshRows();
}
static void onUI_hide() {}
static void onUI_quit() {}
static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }
static bool onUI_Timer(int id) { (void)id; return true; }
static bool onsssetActivityTouchEvent(const MotionEvent &ev) { (void)ev; return false; }

// ── 回调 ─────────────────────────────────────────────────
static bool onButtonClick_ButtonSsSetBack(ZKButton *p) {
    (void)p;
    // 回「设置」：用 openActivity 而不是 goBack()（钟工 2026-10-01，理由同 settingsLogic 的返回键）——
    //   「编辑位置」进的是**启动页** mainActivity，栈会被重排（官方 wiki：已在栈中的界面"移动到顶层"），
    //   goBack() 只能弹到启动界面为止 -> 会掉进屏保画面；
    //   openActivity 在"栈里还有设置页就移到顶层 / 已被弹掉就新建一个"两种情况下都对。
    EASYUICONTEXT->openActivity("settingsActivity");
    return true;
}

// 「编辑位置」：置编辑标志后跳屏保页（mainActivity），在那边拖动浮层，点空白/完成退出
static bool onButtonClick_ButtonSsSetEdit(ZKButton *p) {
    (void)p;
    ConfigStore::getInstance()->setSsEditMode(true);
    LOGD("ssset: enter ss edit mode -> mainActivity");
    EASYUICONTEXT->openActivity("mainActivity");
    return true;
}
static bool onButtonClick_ButtonSsSetRow1(ZKButton *p) { (void)p; toggleRow(0); return true; }
static bool onButtonClick_ButtonSsSetRow2(ZKButton *p) { (void)p; toggleRow(1); return true; }
static bool onButtonClick_ButtonSsSetRow3(ZKButton *p) { (void)p; toggleRow(2); return true; }
static bool onButtonClick_ButtonSsSetRow4(ZKButton *p) { (void)p; toggleRow(3); return true; }
static bool onButtonClick_ButtonSsSetRow5(ZKButton *p) { (void)p; toggleRow(4); return true; }
static bool onButtonClick_ButtonSsSetRow6(ZKButton *p) { (void)p; toggleRow(5); return true; }
