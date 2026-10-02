#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
#pragma once

/*
 * ssoundLogic.cc -- 屏保声音子页（ssound.ftu）
 *
 * 屏保的音频只有视频素材才有（图片没有音轨），所以这里配两件事：
 *   (1) 屏保声音开关：关 = 播放时静音；屏保页右上角的喇叭键写的就是这个开关
 *   (2) 屏保音量：0..100，实际下发时按 volume/100 归一化
 * 落盘走业务单例 ConfigStore（sp_ss_sound / sp_ss_vol），屏保页与喇叭键读同一份，
 * 跨页不互相操作控件。
 *
 * 音量实现路径：只有 ZKVideoView::setVolume(0.0~1.0)。SDK 的 mi-module 不暴露音频
 * API，设备上只有 /dev/mi_ao 这个 MI 音频节点（无 ALSA），所以没有别的手段。
 *
 * 文案用字：字库是裁剪的，新增文案已过 tools/check_glyphs.js（「开/关/喇叭/音量」均有字形）。
 */

#include "utils/Log.h"
#include "entry/EasyUIContext.h"
#include "control/ZKSeekBar.h"
#include "control/ZKTextView.h"
#include "control/ZKButton.h"
#include "storage/ConfigStore.h"
#include "storage/StoragePreferences.h"

#include <stdio.h>

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {0, 1000},
};

static bool sOn  = true;
static int  sVol = 60;

// 选中 = 绿底深字，未选中 = 深底浅字（与「屏保视频」页的 chips 同一套观感）
static void refreshSwitch() {
    if (mImageSndSwBgOnPtr != NULL) {
        mImageSndSwBgOnPtr->setBackgroundPic(sOn ? "images/chip68x44_hl.png" : "images/chip68x44.png");
    }
    if (mImageSndSwBgOffPtr != NULL) {
        mImageSndSwBgOffPtr->setBackgroundPic(sOn ? "images/chip68x44.png" : "images/chip68x44_hl.png");
    }
    if (mTextSndSwOnPtr != NULL)  mTextSndSwOnPtr->setTextColor(sOn ? 0x10141A : 0xECECF0);
    if (mTextSndSwOffPtr != NULL) mTextSndSwOffPtr->setTextColor(sOn ? 0xECECF0 : 0x10141A);
}

static void refreshVolume() {
    char b[16];
    snprintf(b, sizeof(b), "%d%%", sVol);
    if (mTextSndVolValuePtr != NULL) mTextSndVolValuePtr->setText(b);
    if (mSeekSndVolPtr != NULL) mSeekSndVolPtr->setProgress(sVol);
}

// 说明文案要说实话：屏保**画面**由 MI 内核（simple-player）出，**声音**由独立的
// ZKMediaPlayer(AUDIO) 出（见 src/logic/ss_audio.h），两条路解耦，开关与音量对它俩都生效。
// 前提：素材旁边要有同名 mp3（X.mp4 ↔ X.mp3）；没有音轨的素材静音播放，屏保页也不显示喇叭键。
static void refreshHints() {
    if (mTextSndHint1Ptr != NULL) {
        mTextSndHint1Ptr->setText("音量用于屏保视频播放");
    }
    if (mTextSndHint2Ptr != NULL) {
        mTextSndHint2Ptr->setText("屏保页右上角喇叭键可一键静音");
    }
}

// 滑条拖动：只更新数字（高频回调，别在这里写 flash）；松手才落盘
class SndVolListener : public ZKSeekBar::ISeekBarChangeListener {
public:
    virtual void onProgressChanged(ZKSeekBar *pSeekBar, int progress) {
        (void)pSeekBar;
        if (progress < 0) progress = 0;
        if (progress > 100) progress = 100;
        sVol = progress;
        char b[16];
        snprintf(b, sizeof(b), "%d%%", progress);
        if (mTextSndVolValuePtr != NULL) mTextSndVolValuePtr->setText(b);
    }
    virtual void onStopTrackingTouch(ZKSeekBar *pSeekBar) {
        (void)pSeekBar;
        ConfigStore::getInstance()->setSsVolume(sVol);
    }
};
static SndVolListener sVolListener;

// 装饰层触摸穿透（本页非交互的文本/底图）
static void passDecorations() {
    ZKTextView* pass[] = {
        mTextSndTitlePtr, mTextSndTitleEnPtr, mTextSndSwLabelPtr, mTextSndVolLabelPtr,
        mTextSndVolValuePtr, mTextSndHint1Ptr, mTextSndHint2Ptr,
        mImageSndSwBgOnPtr, mImageSndSwBgOffPtr, mImageSndSaveBgPtr,
        mTextSndSwOnPtr, mTextSndSwOffPtr,
    };
    for (size_t i = 0; i < sizeof(pass) / sizeof(pass[0]); i++) {
        if (pass[i] != NULL) {
            pass[i]->setTouchable(false);
            pass[i]->setTouchPass(true);
        }
    }
}

static void loadValues() {
    ConfigStore* cfg = ConfigStore::getInstance();
    sOn  = cfg->ssSoundEnabled();
    sVol = cfg->ssVolume();
}

// ── 系统回调 ─────────────────────────────────────────────
static void onUI_init() {
#ifdef FUN_BUILD
    INIT_UI_TIMERS
#endif // FUN_BUILD
    passDecorations();
    if (mSeekSndVolPtr != NULL) mSeekSndVolPtr->setSeekBarChangeListener(&sVolListener);
    loadValues();
    refreshSwitch();
    refreshVolume();
    refreshHints();
}

static void onUI_intent(const Intent *intentPtr) { (void)intentPtr; }

static void onUI_show() {
    EASYUICONTEXT->setScreensaverEnable(false);   // 子页不进屏保（钟工 09251751-1）
    // ⚠️ Activity 回到前台会被重建，控件文本/底图会回到 JSON 默认值 —— 必须无条件重刷，
    //    不能靠"上次值没变就跳过"（本项目已因这个坑修过主页天气，见 ui_text.h 的说明）。
    loadValues();
    refreshSwitch();
    refreshVolume();
    refreshHints();
    LOGI("ssoundLogic: show sound=%d vol=%d volScale=%.2f",
         sOn ? 1 : 0, sVol, ConfigStore::getInstance()->ssVolumeScale());
}

static void onUI_hide() {
}

static void onUI_quit() {
    if (mSeekSndVolPtr != NULL) mSeekSndVolPtr->setSeekBarChangeListener(NULL);
}

static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

static bool onUI_Timer(int id) { (void)id; return true; }

static bool onssoundActivityTouchEvent(const MotionEvent &ev) { (void)ev; return false; }

// ── 按钮回调 ─────────────────────────────────────────────
static bool onButtonClick_ButtonSndBack(ZKButton *pButton) {
    (void)pButton;
    EASYUICONTEXT->goBack();
    return true;
}

// 开关：立即生效 + 立即落盘（屏保页的喇叭键读的同一个键）
static void setSound(bool on) {
    sOn = on;
    ConfigStore::getInstance()->setSsSoundEnabled(on);
    refreshSwitch();
    LOGD("ssoundLogic: sound -> %s", on ? "ON" : "MUTE");
}

static bool onButtonClick_ButtonSndSwOn(ZKButton *pButton) {
    (void)pButton;
    setSound(true);
    return true;
}

static bool onButtonClick_ButtonSndSwOff(ZKButton *pButton) {
    (void)pButton;
    setSound(false);
    return true;
}

static bool onButtonClick_ButtonSndSave(ZKButton *pButton) {
    (void)pButton;
    ConfigStore* cfg = ConfigStore::getInstance();
    cfg->setSsSoundEnabled(sOn);
    cfg->setSsVolume(sVol);
    LOGD("ssoundLogic: saved sound=%d vol=%d", sOn ? 1 : 0, sVol);
    EASYUICONTEXT->goBack();
    return true;
}

// 兼容生成器可能派发的命名（与代码注册等价）
static void onProgressChanged_SeekSndVol(ZKSeekBar *pSeekBar, int progress) {
    sVolListener.onProgressChanged(pSeekBar, progress);
}
