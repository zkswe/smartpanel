#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
#pragma once

/*
 * homeLogic.cc -- 主页（home.ftu）
 *
 * 多 Activity 口径：本页只碰自己的控件；跨页跳转 openActivity/goBack；共享状态走业务单例。
 * 交互：
 *   - 设备卡：单击 = 开关（业务待接 RelayManager）；**长按 600ms = 自定义名称**（弹重命名模态框）
 *   - 情景 chip：单击 = 执行 + 选中；**按下/选中 = 高对比效果**（绿底 + 深字）
 *   - 空闲 15s 回入口 Activity（屏保）
 */

#include "utils/Log.h"
#include "utils/TimeHelper.h"
#include "entry/EasyUIContext.h"
#include "window/ZKWindow.h"
#include "control/ZKTextView.h"
#include "control/ZKButton.h"
#include "control/ZKEditText.h"
#include "storage/ConfigStore.h"
#include "storage/StoragePreferences.h"
#include "device/RelayManager.h"
#include "device/SensorManager.h"      // 真实天气 / 温湿度 / PM2.5
#include "logic/ui_text.h"             // setTextIfChanged（判据 = 控件实际文本）
#include "network/MqttBridge.h"
#include "scene/SceneManager.h"

#include <stdio.h>
#include <string>

#define IDLE_ENTER_SEC 15          // 空闲进屏保（心跳 tick 计时；不用墙钟）
#define CHIP_COUNT    8

// 高对比（选中/按下）：绿底 + 深字；常态：卡底 + 亮字
#define CLR_CHIP_ON   0x10141A
#define CLR_CHIP_OFF  0xECECF0
#define CLR_TX        0xECECF0
#define CLR_DIM       0x5F5F68
#define PIC_CHIP_ON   "images/chip_hl.png"
#define PIC_CHIP_OFF  "images/chip.png"

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {0, 1000},
};

static const char* kWeek[] = { "日", "一", "二", "三", "四", "五", "六" };
static long long sLastActivityMs = 0;
static int sIdleTicks = 0;      // 空闲心跳计数（触摸归零）——不用墙钟：TimeHelper::getCurrentTime()
                                // 会因 NTP/时间同步跳变，导致「操作中被屏保打断」（钟工 2026-09-25 报告 2/3）
static int sSelChip = -1;         // 当前选中情景索引（-1 = 无；由 SceneManager 当前情景决定）
static int sRenameCh = 1;         // 正在改名的通道
static std::string sChipNames[CHIP_COUNT];   // chip 索引 -> 情景名（每次刷新列表时同步）
// HA 模式：chip 对应 HA 情景的 entity_id（钟工 2026-09-25：情景由 HA 操作，面板拉取展示 + 可触发）
static std::string sChipHaId[CHIP_COUNT];
static std::string sLastHaActive;      // 上次刷新时的 HA 生效情景（变化才重刷，避免每秒重绘）
// 可见性门控（钟工 09251751-1 真根因）：EasyUI 隐藏活动后 **本页 1s 计时器仍在跑**，
// 原来的 15s 空闲 -> goHome() 会把用户从「设置」等子页拽回屏保。只在真正可见时判空闲。
static bool sVisible = false;

static long long nowMs() { return TimeHelper::getCurrentTime(); }
static void noteActivity() { sLastActivityMs = nowMs(); sIdleTicks = 0; }

static ZKTextView* chipBg(int i) {
    ZKTextView* a[CHIP_COUNT] = { mImageChipBg1Ptr, mImageChipBg2Ptr, mImageChipBg3Ptr, mImageChipBg4Ptr,
                                 mImageChipBg5Ptr, mImageChipBg6Ptr, mImageChipBg7Ptr, mImageChipBg8Ptr };
    return (i >= 0 && i < CHIP_COUNT) ? a[i] : NULL;
}
static ZKTextView* chipLabel(int i) {
    ZKTextView* a[CHIP_COUNT] = { mTextChip1Ptr, mTextChip2Ptr, mTextChip3Ptr, mTextChip4Ptr,
                                 mTextChip5Ptr, mTextChip6Ptr, mTextChip7Ptr, mTextChip8Ptr };
    return (i >= 0 && i < CHIP_COUNT) ? a[i] : NULL;
}
static ZKButton* chipBtn(int i) {
    ZKButton* a[CHIP_COUNT] = { mButtonSceneChip1Ptr, mButtonSceneChip2Ptr, mButtonSceneChip3Ptr, mButtonSceneChip4Ptr,
                                mButtonSceneChip5Ptr, mButtonSceneChip6Ptr, mButtonSceneChip7Ptr, mButtonSceneChip8Ptr };
    return (i >= 0 && i < CHIP_COUNT) ? a[i] : NULL;
}
static ZKTextView* devNameText(int i) {
    ZKTextView* a[3] = { mTextDevName1Ptr, mTextDevName2Ptr, mTextDevName3Ptr };
    return (i >= 0 && i < 3) ? a[i] : NULL;
}

// 选中/按下统一用高对比外观
static void applyChipVisual(int i, bool highlight) {
    ZKTextView* bg = chipBg(i);
    ZKTextView* lb = chipLabel(i);
    if (bg != NULL) bg->setBackgroundPic(highlight ? PIC_CHIP_ON : PIC_CHIP_OFF);
    if (lb != NULL) lb->setTextColor(highlight ? CLR_CHIP_ON : CLR_CHIP_OFF);
}

// 情景 chip 显示宽度（100px 盒子，20px 字）：汉字按 1.0、ASCII 按 0.55 估，最多 4 个汉字宽
static std::string fitChipText(const std::string& s) {
    std::string out;
    double used = 0.0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t len = (c < 0x80) ? 1 : ((c < 0xE0) ? 2 : ((c < 0xF0) ? 3 : 4));
        if (i + len > s.size()) break;
        double w = (c < 0x80) ? 0.55 : 1.0;
        if (used + w > 4.0) break;
        out.append(s, i, len);
        used += w;
        i += len;
    }
    return out;
}

// chip 显示名：优先用情景描述的中文名（"回家模式: ..." -> "回家"），回退 name（英文键）
// 注意：不能用 find_first_of(":：") -- 它按**单字节**匹配，全角冒号"："的字节会误切中
// 其他汉字的中间字节（实测把"回家模式"切成"回家模"+半个字节）；这里分别用 find() 整字节序列搜。
static std::string chipLabelText(const SceneDef& d) {
    std::string s = d.desc;
    size_t cut = s.find(':');
    size_t cut2 = s.find("：");          // 全角冒号（UTF-8 三字节）
    if (cut2 != std::string::npos && (cut == std::string::npos || cut2 < cut)) cut = cut2;
    if (cut != std::string::npos) s = s.substr(0, cut);
    const std::string kMode = "模式";
    if (s.size() >= kMode.size() && s.compare(s.size() - kMode.size(), kMode.size(), kMode) == 0)
        s = s.substr(0, s.size() - kMode.size());
    if (s.empty()) s = d.name;
    return s;
}

// chip 文案/显隐按数据源同步：HA 模式用 HA 情景清单（MqttBridge 从 MQTT 拉取），
// 其他模式用本机 SceneManager 情景集。
static void refreshChips() {
    SceneManager* sm = SceneManager::getInstance();
    std::vector<SceneDef> list = sm->scenes();
    const std::string& cur = sm->currentScene();
    bool ha = (ConfigStore::getInstance()->runMode() == ConfigStore::MODE_HA);
    int haN = ha ? MqttBridge::getInstance()->haSceneCount() : 0;
    // HA 模式：当前生效情景由 HA 回报（smartpanel/ha/active_scene）-> 对应 chip 高亮
    std::string haActive = ha ? MqttBridge::getInstance()->activeHaScene() : std::string();
    sSelChip = -1;
    for (int i = 0; i < CHIP_COUNT; i++) {
        bool fromHa = ha && haN > 0;
        bool has = fromHa ? (i < haN) : (i < (int)list.size());
        if (fromHa) {
            sChipNames[i] = has ? MqttBridge::getInstance()->haSceneName(i) : std::string();
            sChipHaId[i]  = has ? MqttBridge::getInstance()->haSceneId(i) : std::string();
            if (has && !haActive.empty() && sChipHaId[i] == haActive) sSelChip = i;
        } else {
            sChipNames[i] = has ? list[i].name : std::string();
            sChipHaId[i].clear();
            if (has && list[i].name == cur) sSelChip = i;
        }
        ZKTextView* bg = chipBg(i);
        ZKTextView* lb = chipLabel(i);
        ZKButton* bt = chipBtn(i);
        if (bg != NULL) bg->setVisible(has);
        if (bt != NULL) bt->setVisible(has);
        if (lb != NULL) {
            lb->setVisible(has);
            if (has) {
                std::string txt = fromHa ? sChipNames[i] : chipLabelText(list[i]);
                lb->setText(fitChipText(txt).c_str());
            }
        }
    }
    for (int i = 0; i < CHIP_COUNT; i++) {
        applyChipVisual(i, i == sSelChip);
    }
}


// 开关状态由业务单例 RelayManager 提供（src/device/RelayManager，过零 IO 索引 {3,1,2}）

static ZKTextView* devCardBg(int i) {
    ZKTextView* a[3] = { mImageCardBg1Ptr, mImageCardBg2Ptr, mImageCardBg3Ptr };
    return (i >= 0 && i < 3) ? a[i] : NULL;
}
static ZKTextView* devStateText(int i) {
    ZKTextView* a[3] = { mTextDevState1Ptr, mTextDevState2Ptr, mTextDevState3Ptr };
    return (i >= 0 && i < 3) ? a[i] : NULL;
}

// 高对比：开启 / 按下 = 绿卡 + 深字；关闭 = 常态卡 + 亮字
static void applyCardVisual(int i, bool highlight) {
    bool on = RelayManager::getInstance()->get(i + 1) || highlight;
    ZKTextView* bg = devCardBg(i);
    ZKTextView* nm = devNameText(i);
    ZKTextView* st = devStateText(i);
    if (bg != NULL) bg->setBackgroundPic(on ? "images/card_hl.9.png" : "images/card.9.png");
    if (nm != NULL) nm->setTextColor(on ? CLR_CHIP_ON : CLR_TX);
    if (st != NULL) {
        st->setText(RelayManager::getInstance()->get(i + 1) ? "开启" : "关闭");
        st->setTextColor(on ? CLR_CHIP_ON : CLR_DIM);
    }
}

static void refreshDevCards() {
    for (int i = 0; i < 3; i++) applyCardVisual(i, false);
}

class DevTouchListener : public ZKBase::ITouchListener {
public:
    explicit DevTouchListener(int idx) : mIdx(idx) {}
    virtual void onTouchEvent(ZKBase *pBase, const MotionEvent &ev) {
        (void)pBase;
        switch (ev.mActionStatus) {
        case MotionEvent::E_ACTION_DOWN:
            noteActivity();
            applyCardVisual(mIdx, true);
            break;
        case MotionEvent::E_ACTION_UP:
        case MotionEvent::E_ACTION_CANCEL:
            applyCardVisual(mIdx, false);
            break;
        default:
            break;
        }
    }
private:
    int mIdx;
};
static DevTouchListener sDevTouch1(0), sDevTouch2(1), sDevTouch3(2);

static void refreshDevNames() {
    for (int i = 0; i < 3; i++) {
        ZKTextView* t = devNameText(i);
        if (t != NULL) t->setText(ConfigStore::getInstance()->relayName(i + 1).c_str());
    }
}

static ZKTextView* devIcon(int i) {
    ZKTextView* a[3] = { mImageDev1Ptr, mImageDev2Ptr, mImageDev3Ptr };
    return (i >= 0 && i < 3) ? a[i] : NULL;
}
static ZKButton* devBtn(int i) {
    ZKButton* a[3] = { mButtonDev1Ptr, mButtonDev2Ptr, mButtonDev3Ptr };
    return (i >= 0 && i < 3) ? a[i] : NULL;
}

/* ── 按键显隐 + 自适应重排（钟工 2026-09-29）───────────────────────
 * 显隐：/data 的 sw_show_1..3（按键配置子页 keyset 里改；默认全显示）
 * 尺寸/布局：3 个 = 141×216（原布局）；2 个 = 216×216；1 个 = 260×216 居中
 *   —— 数量变了才重排（sLastMask 记忆），不会每帧 setPosition
 */
#define CARD_Y 116
#define CARD_H 216
#define ICON_S 84
static int sLastMask = -1;

static bool homeSwVisible(int i) {
    return ConfigStore::getInstance()->swVisible(i + 1);   // 统一走 ConfigStore（持久化口径）
}

// force=true：跳过"数量没变就不重排"的短路，无条件按当前 mask 重排一遍。
//   ⚠️ 为什么需要它（与 refreshWx 同一类坑，2026-10-01）：sLastMask 是函数内 static，
//   从屏保返回主页时若控件被重新实例化，卡片位置会回到 ui/home.json 的三卡默认坐标，
//   而 sLastMask 仍等于当前 mask -> 直接 return，于是"只有 2 个开关却按 3 卡摆"的错布局
//   会一直保持。所以页面初始化/每次 show 都 force 一次（幂等，重复设置同样坐标无副作用）。
static void applySwitchLayout(bool force = false) {
    int mask = 0;
    for (int i = 0; i < 3; i++) if (homeSwVisible(i)) mask |= (1 << i);
    if (mask == 0) mask = 1;                           // 兜底：至少一个（配置页也拦了）
    if (!force && mask == sLastMask) return;           // 没变化不动
    sLastMask = mask;

    int vis[3], n = 0;
    for (int i = 0; i < 3; i++) if (mask & (1 << i)) vis[n++] = i;
    // 尺寸口径（钟工 2026-09-29 14:58）：3 个 = 141（原布局）、2 个 = 216、
    //   1 个 = **460**（几乎满宽，画面更饱满）
    int w = (n >= 3) ? 141 : (n == 2 ? 216 : 460);
    int gap = (n >= 3) ? 12 : 16;
    int x0 = (480 - (n * w + (n - 1) * gap)) / 2;
    for (int j = 0; j < 3; j++) {
        ZKButton* b = devBtn(j);
        ZKTextView* bg = devCardBg(j);
        ZKTextView* ic = devIcon(j);
        ZKTextView* nm = devNameText(j);
        ZKTextView* st = devStateText(j);
        bool on = (mask & (1 << j)) != 0;
        if (b != NULL) b->setVisible(on);
        if (bg != NULL) bg->setVisible(on);
        if (ic != NULL) ic->setVisible(on);
        if (nm != NULL) nm->setVisible(on);
        if (st != NULL) st->setVisible(on);
        if (!on) continue;
        int idx = 0;
        while (idx < n && vis[idx] != j) idx++;
        int x = x0 + idx * (w + gap);
        if (bg != NULL) bg->setPosition(LayoutPosition(x, CARD_Y, w, CARD_H));
        if (b != NULL) b->setPosition(LayoutPosition(x, CARD_Y, w, CARD_H));
        if (ic != NULL) ic->setPosition(LayoutPosition(x + (w - ICON_S) / 2, CARD_Y + 36, ICON_S, ICON_S));
        if (nm != NULL) nm->setPosition(LayoutPosition(x, CARD_Y + 134, w, 26));
        if (st != NULL) st->setPosition(LayoutPosition(x, CARD_Y + 162, w, 24));
    }
    LOGD("home: switch layout -> mask=0x%x (n=%d, cardW=%d) sw_show=%d,%d,%d",
         mask, n, w, mask & 1 ? 1 : 0, mask & 2 ? 1 : 0, mask & 4 ? 1 : 0);
}

// 按下 -> 高对比；抬起 -> 回到"是否选中"的状态
class ChipTouchListener : public ZKBase::ITouchListener {
public:
    explicit ChipTouchListener(int idx) : mIdx(idx) {}
    virtual void onTouchEvent(ZKBase *pBase, const MotionEvent &ev) {
        (void)pBase;
        switch (ev.mActionStatus) {
        case MotionEvent::E_ACTION_DOWN:
            noteActivity();
            applyChipVisual(mIdx, true);
            break;
        case MotionEvent::E_ACTION_UP:
        case MotionEvent::E_ACTION_CANCEL:
            applyChipVisual(mIdx, mIdx == sSelChip);
            break;
        default:
            break;
        }
    }
private:
    int mIdx;
};
static ChipTouchListener sChipTouch[CHIP_COUNT] = {
    ChipTouchListener(0), ChipTouchListener(1), ChipTouchListener(2), ChipTouchListener(3),
    ChipTouchListener(4), ChipTouchListener(5), ChipTouchListener(6), ChipTouchListener(7),
};

// 长按设备卡 -> 重命名
class DevLongClickListener : public ZKBase::ILongClickListener {
public:
    explicit DevLongClickListener(int ch) : mCh(ch) {}
    virtual void onLongClick(ZKBase *pBase) {
        (void)pBase;
        sRenameCh = mCh;
        if (mWindowRenamePtr == NULL) return;
        std::string cur = ConfigStore::getInstance()->relayName(mCh);
        if (mEditRenamePtr != NULL) mEditRenamePtr->setText(cur.c_str());
        mWindowRenamePtr->showWnd();
        LOGD("long press ch%d -> rename dialog (cur=%s)", mCh, cur.c_str());
    }
private:
    int mCh;
};
static DevLongClickListener sDevLong1(1), sDevLong2(2), sDevLong3(3);

static void refreshHomeClock() {
    struct tm* t = TimeHelper::getDateTime();
    if (t == NULL) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    if (mTextClkPtr != NULL) mTextClkPtr->setText(buf);
    snprintf(buf, sizeof(buf), "%02d月%02d日 星期%s",
             t->tm_mon + 1, t->tm_mday, kWeek[t->tm_wday]);
    if (mTextDatePtr != NULL) mTextDatePtr->setText(buf);
}

// ── 主页顶栏：真实天气 / 温湿度 / PM2.5（2026-10-01 重做）────────
//   改前：TextWxTemp(「26.5°C」)、TextWxText(「晴 26°C」) 是 ui/home.json 里写死的静态文案，
//         这两个控件在 C++ 里**一次都没被引用过**（grep 零命中）—— 顶栏那两行字是贴图。
//   改后：由 SensorManager 的真实数据驱动；没有数据就显示「读取中 / 获取中」，不编数字。
//   文案用字：字库裁剪过，禁用 ℃ ° · μ ³（主字库无这些字形），温度用 "C"。见 tools/check_glyphs.js。
//
//   ⚠️ 2026-10-01 二次修正（钟工：「屏保返回主界面会变成获取中」）：
//   原实现用 static sLastT/sLastW/sLastA 记「上次写进去的值」，值没变就跳过 setText。
//   但从屏保返回时控件会被重新实例化，文本回到 ui/home.json 的静态占位（「读取中」「获取中」），
//   而 static 缓存仍以为"已经写过了" -> 永不刷新，界面永久停在占位文案上。
//   现改用 setTextIfChanged()（判据 = 控件当前实际文本，见 logic/ui_text.h），免疫重建/跨页。
static void refreshWx() {
    SensorManager* sm = SensorManager::getInstance();
    static bool sLogged = false;
    if (!sLogged) {
        sLogged = true;   // 只打一次：现场排查「顶栏天气不显示」用（控件是否绑上 + 真值是什么）
        LOGI("home: wx ptrs temp=%p text=%p air=%p | t=%.1f h=%.0f wx=%s pm25=%.1f aqi=%d has=%d",
             (void*)mTextWxTempPtr, (void*)mTextWxTextPtr, (void*)mTextWxAirPtr,
             sm->temp(), sm->hum(), sm->weather().c_str(), sm->pm25(), sm->aqi(),
             sm->hasData() ? 1 : 0);
    }
    const bool has = sm->hasData();
    char b[96];

    std::string t = "读取中";
    if (has) { snprintf(b, sizeof(b), "%.1fC", sm->temp()); t = b; }
    setTextIfChanged(mTextWxTempPtr, t);

    const std::string w = has ? sm->weather() : std::string("获取中");
    setTextIfChanged(mTextWxTextPtr, w);

    std::string a = "PM2.5 获取中";
    if (sm->pm25() >= 0.0f) {
        snprintf(b, sizeof(b), "PM2.5 %.0f %s", sm->pm25(), sm->aqiLevel().c_str());
        a = b;
    }
    setTextIfChanged(mTextWxAirPtr, a);
}

// 装饰层触摸穿透（本页控件）
static void passThroughDecorations() {
    ZKTextView* pass[] = {
        mImageGearPtr,
        mImageCardBg1Ptr, mImageDev1Ptr, mTextDevState1Ptr,
        mImageCardBg2Ptr, mImageDev2Ptr, mTextDevState2Ptr,
        mImageCardBg3Ptr, mImageDev3Ptr, mTextDevState3Ptr,
        mTextSceneLabelPtr, mImageDot1Ptr, mImageDot2Ptr,
        mImageChipBg1Ptr, mImageChipBg2Ptr, mImageChipBg3Ptr, mImageChipBg4Ptr,
        mImageChipBg5Ptr, mImageChipBg6Ptr, mImageChipBg7Ptr, mImageChipBg8Ptr,
        mTextChip1Ptr, mTextChip2Ptr, mTextChip3Ptr, mTextChip4Ptr,
        mTextChip5Ptr, mTextChip6Ptr, mTextChip7Ptr, mTextChip8Ptr,
        mImageDimPtr, mImageMboxPtr, mTextEditScenePtr, mTextEditSubPtr,
        mImageMswBg1Ptr, mImageMswBg2Ptr, mImageMswBg3Ptr, mImageEditSaveBgPtr,
        mImageRenameDimPtr, mImageRenameBoxPtr, mTextRenameTitlePtr, mTextRenameHintPtr,
        mImageRenameOkBgPtr, mImageRenameCancelBgPtr,
    };
    for (size_t i = 0; i < sizeof(pass) / sizeof(pass[0]); i++) {
        if (pass[i] != NULL) {
            pass[i]->setTouchable(false);
            pass[i]->setTouchPass(true);
        }
    }
    // 设备卡名称也可以长按改名：名称层同样放行 -> 交给下面的卡片按钮
    for (int i = 0; i < 3; i++) {
        ZKTextView* t = devNameText(i);
        if (t != NULL) {
            t->setTouchable(false);
            t->setTouchPass(true);
        }
    }
}

static void registerListeners() {
    for (int i = 0; i < CHIP_COUNT; i++) {
        ZKButton* b = chipBtn(i);
        if (b != NULL) b->setTouchListener(&sChipTouch[i]);
    }
    if (mButtonDev1Ptr != NULL) mButtonDev1Ptr->setTouchListener(&sDevTouch1);
    if (mButtonDev2Ptr != NULL) mButtonDev2Ptr->setTouchListener(&sDevTouch2);
    if (mButtonDev3Ptr != NULL) mButtonDev3Ptr->setTouchListener(&sDevTouch3);
    if (mButtonDev1Ptr != NULL) mButtonDev1Ptr->setLongClickListener(&sDevLong1);
    if (mButtonDev2Ptr != NULL) mButtonDev2Ptr->setLongClickListener(&sDevLong2);
    if (mButtonDev3Ptr != NULL) mButtonDev3Ptr->setLongClickListener(&sDevLong3);
}

static void unregisterListeners() {
    for (int i = 0; i < CHIP_COUNT; i++) {
        ZKButton* b = chipBtn(i);
        if (b != NULL) b->setTouchListener(NULL);
    }
    if (mButtonDev1Ptr != NULL) mButtonDev1Ptr->setTouchListener(NULL);
    if (mButtonDev2Ptr != NULL) mButtonDev2Ptr->setTouchListener(NULL);
    if (mButtonDev3Ptr != NULL) mButtonDev3Ptr->setTouchListener(NULL);
    if (mButtonDev1Ptr != NULL) mButtonDev1Ptr->setLongClickListener(NULL);
    if (mButtonDev2Ptr != NULL) mButtonDev2Ptr->setLongClickListener(NULL);
    if (mButtonDev3Ptr != NULL) mButtonDev3Ptr->setLongClickListener(NULL);
}

// ── 系统回调 ─────────────────────────────────────────────
static void onUI_init() {
#ifdef FUN_BUILD
    INIT_UI_TIMERS
#endif // FUN_BUILD
    ConfigStore::getInstance()->init();
    ConfigStore::getInstance()->init();
    RelayManager::getInstance()->init();          // 继电器：过零IO/GPIO/模拟 + 恢复断电前状态（幂等）
    // 卡片视觉刷新占「UI 单槽」（覆盖式，只影响 UI）。
    // 钟工 2026-09-27 事故：旧版 setListener 是整体 clear()，第一次进主页就把 MqttBridge
    // 注册的 HA 状态上报回调抹掉 -> 命令能到、状态不回发；现已分槽（RelayManager.h）。
    RelayManager::getInstance()->setListener([](int ch, bool on) {
        (void)on;
        applyCardVisual(ch - 1, false);
    });
    SceneManager::getInstance()->init();          // 情景定义（首次启动灌出厂情景；幂等）
    passThroughDecorations();
    registerListeners();
    refreshHomeClock();
    refreshWx();
    refreshDevNames();
    refreshDevCards();
    refreshChips();
    applySwitchLayout(true);     // 开机首次进主页：force 一次（控件可能是新建的，缓存不可信）
    noteActivity();
}

static void onUI_intent(const Intent *intentPtr) {
    if (intentPtr != NULL) {
        // 子页返回时按 Intent 参数刷新（例如改名后回来）
    }
}

static void onUI_show() {
    sVisible = true;                              // 可见才算活动（空闲才计）
    EASYUICONTEXT->setScreensaverEnable(true);    // 只有主页进屏保（钟工 09251751-1）
    EASYUICONTEXT->resetScreensaverTimeOut();
    refreshHomeClock();
    refreshWx();
    refreshDevNames();
    refreshDevCards();
    refreshChips();
    applySwitchLayout(true);      // 每次进主页都 force（从屏保返回时控件可能被重建，缓存不可信）
    noteActivity();
}

static void onUI_hide() {
    sVisible = false;      // 离开本页：不再判空闲（否则会抢子页）
}

static void onUI_quit() {
    unregisterListeners();
    sVisible = false;
}

static void onProtocolDataUpdate(const SProtocolData &data) {
    (void)data;
}

static bool onUI_Timer(int id) {
    switch (id) {
    case 0:
        refreshHomeClock();
        refreshWx();              // 真实天气/温湿度/PM2.5（值变化才重绘）
        applySwitchLayout();      // 按键配置（子页/外部）改过显隐 -> ≤1s 跟随重排
        // HA 模式：当前生效情景变化（HA 回报）-> 重刷 chip 高亮（≤1s 跟随）
        if (ConfigStore::getInstance()->runMode() == ConfigStore::MODE_HA) {
            std::string act = MqttBridge::getInstance()->activeHaScene();
            if (act != sLastHaActive) {
                sLastHaActive = act;
                LOGD("home: HA active scene -> '%s'", act.c_str());
                refreshChips();
            }
        }
        sIdleTicks++;
        if (sVisible && sIdleTicks >= IDLE_ENTER_SEC) {
            // 诊断日志：带上墙钟参考值，便于区分「真空闲」与「校时跳变」
            LOGD("home: idle -> screensaver (ticks=%d, wall_diff_ms=%lld)", sIdleTicks, nowMs() - sLastActivityMs);
            sIdleTicks = 0;
            EASYUICONTEXT->goHome();
        }
        break;
    default:
        break;
    }
    return true;
}

static bool onhomeActivityTouchEvent(const MotionEvent &ev) {
    if (ev.mActionStatus == MotionEvent::E_ACTION_DOWN || ev.mActionStatus == MotionEvent::E_ACTION_MOVE) {
        noteActivity();          // DOWN/MOVE 都算活动（拖动时不会被屏保打断）
    }
    return false;
}

// ── 按钮回调 ─────────────────────────────────────────────
static bool onButtonClick_ButtonGear(ZKButton *pButton) {
    (void)pButton;
    noteActivity();
    EASYUICONTEXT->openActivity("settingsActivity");
    return true;
}

static bool onButtonClick_ButtonDev1(ZKButton *pButton) {
    (void)pButton;
    noteActivity();
    RelayManager::getInstance()->toggle(1);
    applyCardVisual(0, false);
    return true;
}
static bool onButtonClick_ButtonDev2(ZKButton *pButton) {
    (void)pButton;
    noteActivity();
    RelayManager::getInstance()->toggle(2);
    applyCardVisual(1, false);
    return true;
}
static bool onButtonClick_ButtonDev3(ZKButton *pButton) {
    (void)pButton;
    noteActivity();
    RelayManager::getInstance()->toggle(3);
    applyCardVisual(2, false);
    return true;
}

// 情景 chip：单击 = 执行
//   HA 模式：发 ha_scene/set 让 HA 执行该情景（面板不自己跑联动）
//   其他模式：本机 SceneManager 执行（false = 本机发起 -> 走广播/上报）
static bool onButtonClick_ButtonSceneChipN(int idx) {
    noteActivity();
    if (idx < 0 || idx >= CHIP_COUNT || sChipNames[idx].empty()) return true;
    if (!sChipHaId[idx].empty()) {
        MqttBridge::getInstance()->triggerHaScene(sChipHaId[idx]);
        LOGD("ha scene chip %d -> '%s' (%s)", idx, sChipNames[idx].c_str(),
             sChipHaId[idx].c_str());
        refreshChips();
        return true;
    }
    SceneManager::getInstance()->activate(sChipNames[idx], false);
    LOGD("scene chip %d -> '%s'", idx, sChipNames[idx].c_str());
    refreshChips();
    refreshDevCards();
    return true;
}
static bool onButtonClick_ButtonSceneChip1(ZKButton *p) { (void)p; return onButtonClick_ButtonSceneChipN(0); }
static bool onButtonClick_ButtonSceneChip2(ZKButton *p) { (void)p; return onButtonClick_ButtonSceneChipN(1); }
static bool onButtonClick_ButtonSceneChip3(ZKButton *p) { (void)p; return onButtonClick_ButtonSceneChipN(2); }
static bool onButtonClick_ButtonSceneChip4(ZKButton *p) { (void)p; return onButtonClick_ButtonSceneChipN(3); }
static bool onButtonClick_ButtonSceneChip5(ZKButton *p) { (void)p; return onButtonClick_ButtonSceneChipN(4); }
static bool onButtonClick_ButtonSceneChip6(ZKButton *p) { (void)p; return onButtonClick_ButtonSceneChipN(5); }
static bool onButtonClick_ButtonSceneChip7(ZKButton *p) { (void)p; return onButtonClick_ButtonSceneChipN(6); }
static bool onButtonClick_ButtonSceneChip8(ZKButton *p) { (void)p; return onButtonClick_ButtonSceneChipN(7); }

// 情景编辑模态框
static bool onButtonClick_ButtonEditPrev(ZKButton *pButton) { (void)pButton; LOGD("scene edit prev"); return true; }
static bool onButtonClick_ButtonEditNext(ZKButton *pButton) { (void)pButton; LOGD("scene edit next"); return true; }
static bool onButtonClick_ButtonEditSw1(ZKButton *pButton) { (void)pButton; LOGD("edit switch 1 toggle"); return true; }
static bool onButtonClick_ButtonEditSw2(ZKButton *pButton) { (void)pButton; LOGD("edit switch 2 toggle"); return true; }
static bool onButtonClick_ButtonEditSw3(ZKButton *pButton) { (void)pButton; LOGD("edit switch 3 toggle"); return true; }
static bool onButtonClick_ButtonEditSave(ZKButton *pButton) {
    (void)pButton;
    LOGD("scene edit save");
    if (mWindowSceneEditPtr != NULL) mWindowSceneEditPtr->hideWnd();
    return true;
}

// 重命名模态框：确定 -> 落盘 + 刷新卡片名（业务单例，不碰别的页面控件）
static bool onButtonClick_ButtonRenameOk(ZKButton *pButton) {
    (void)pButton;
    if (mEditRenamePtr != NULL) {
        std::string name = mEditRenamePtr->getText();
        // 去首尾空白 + 限长（UTF-8 安全截断到 8 个汉字 / kMaxNameBytes 字节）
        size_t b = name.find_first_not_of(" \t\r\n");
        size_t e = name.find_last_not_of(" \t\r\n");
        name = (b == std::string::npos) ? "" : name.substr(b, e - b + 1);
        std::string out;
        for (size_t i = 0; i < name.size() && out.size() < (size_t)ConfigStore::kMaxNameBytes;) {
            unsigned char c = (unsigned char)name[i];
            size_t len = (c < 0x80) ? 1 : ((c < 0xE0) ? 2 : 3);
            if (i + len > name.size()) break;
            out.append(name, i, len);
            i += len;
        }
        if (!out.empty()) {
            ConfigStore::getInstance()->setRelayName(sRenameCh, out);
            refreshDevNames();
    refreshDevCards();
            // v7.4：改名后立刻重发 HA discovery（新名字随 retained 配置下发 -> HA 里显示同步）
            MqttBridge::getInstance()->republishDiscovery();
            LOGD("rename ch%d -> %s", sRenameCh, out.c_str());
        }
    }
    if (mWindowRenamePtr != NULL) mWindowRenamePtr->hideWnd();
    return true;
}

static bool onButtonClick_ButtonRenameCancel(ZKButton *pButton) {
    (void)pButton;
    if (mWindowRenamePtr != NULL) mWindowRenamePtr->hideWnd();
    return true;
}

static void onEditTextChanged_EditRename(const std::string &text) {
    LOGD("rename input: %s", text.c_str());
}
