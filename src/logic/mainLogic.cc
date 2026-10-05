#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
#pragma once

/*
 * mainLogic.cc -- 屏保页（main.ftu = 应用入口 Activity）
 *
 * 职责：屏保视频轮播（ZKVideoView，最底层）+ 时钟/温湿度浮层 + 触摸进主页 +
 *       息屏时段判定（关屏设置子页配置，钟工 2026-09-24 23:40 口径）+
 *       屏保项显隐/位置自由拖动（设计说明书 3.4.3）。
 * 多 Activity 口径：本页只碰自己的控件；配置走业务单例 ConfigStore。
 */

#include "utils/Log.h"
#include "utils/TimeHelper.h"
#include "utils/BrightnessHelper.h"
#include "entry/EasyUIContext.h"
#include "os/SystemProperties.h"
#include "control/ZKBase.h"          // ZKBase（ssSetPos 的统一落地点）
#include "control/ZKTextView.h"
#include "control/ZKVideoView.h"
#include "control/ZKButton.h"
#include "storage/ConfigStore.h"
#include "storage/StoragePreferences.h"
#include "network/LocalLink.h"
#include "network/MqttBridge.h"
#include "network/PanelLink.h"
#include "scene/SceneManager.h"
#include "device/RelayManager.h"
#include "device/SensorManager.h"
#include "device/AlmanacManager.h"  // 屏保黄历（农历/干支/宜/忌）
#include "system/ClockManager.h"
#include "wall/WallLink.h"
#include "wall/WallPlayer.h"
#include "system/NetKeeper.h"
#include "system/DisplayFlip.h"
#include "system/MediaGuard.h"      // 素材可用性护栏（标记不可播素材，见 MediaGuard.h）
#include "logic/ui_common.h"        // applyWeatherIcon（天气图标映射）
#include "logic/ui_text.h"          // setTextIfChanged（判据 = 控件实际文本）
#include "logic/ss_audio.h"         // 屏保音频：ZKMediaPlayer(AUDIO)，与 MI 视频解耦

#include <stdio.h>
#include <stdlib.h>          // atoll（轮播停留锚点的持久化读取）
#include <ctype.h>
#include <time.h>            // clock_gettime（轮播停留计时：墙钟，见 ssEpochSec）
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>

#define SS_TIMEOUT_SEC   15          // 空闲进屏保 / 关屏时段内空闲熄屏（系统 Screensaver）
#define SS_DWELL_EOF_GRACE_SEC 15    // 「轮播间隔」到点后再等这么久还没收到完成回调 -> 强切（兜底，正常走不到）
#define SS_PHOTO_SEC     15          // 图片素材默认显示 15 秒（可在「屏保视频」页配 5/10/15/30/60s）
#define SS_SCR_W         480
#define SS_SCR_H         480
#define SS_GROUP_N       6           // 时间/日期/天气/温湿度/状态条/黄历
// ── 浮层位置：内部模型一律是「绝对坐标（可负）」（钟工 2026-10-01 新口径）──
//   旧口径（问题单 09251751-8）：左/上夹到绝对 0、右下至少留 40px 在屏内。
//   新口径：**支持负坐标、任意位置摆放**（允许整块摆出屏外）→ 引擎语义与编码见 ssEncLeft()。
#define SS_POS_UNSET     (-100000)   // "未设置"哨兵。⚠️ 不能再用 -1：负坐标现在是合法位置
#define SS_POS_LIMIT     20000       // 离谱值护栏（防脏数据/手指抖动把坐标写到天上去）
#define SS_RESET_HOLD_MS 800         // 编辑态长按空白处多少毫秒算"复位全部位置"

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {0, 1000},
    {1, 50},        // ① 拼墙起播踩点调度（主线程，别挪到线程里——会踩 MI 图层，见 09-29 21:15 事故）
};

// 全局触摸监听：任意页面触摸都复位系统屏保计时（钟工 2026-09-25 报告 2/3：
// 之前子页/编辑态的触摸没复位系统屏保，操作到一半就被屏保打断）
class GlobalTouchReset : public EasyUIContext::ITouchListener {
public:
    virtual bool onTouchEvent(const MotionEvent &ev) {
        if (ev.mActionStatus == MotionEvent::E_ACTION_DOWN || ev.mActionStatus == MotionEvent::E_ACTION_MOVE) {
            EASYUICONTEXT->resetScreensaverTimeOut();
        }
        return false;      // 不消费，继续正常分发
    }
};
static GlobalTouchReset sGlobalTouchReset;

// 注：nowMs() 用 ui_common.h 里的 inline 版本（本文件不再自带一份，避免重定义）

static const char* kWeek[] = { "日", "一", "二", "三", "四", "五", "六" };
static std::vector<std::string> sVideos;   // 混播清单（视频 + 相册上传的图片）
static int sVideoIdx = 0;

// 素材类型：图片（屏保直接铺图，每张 15s）/ 视频（走 ZKVideoView）
static bool isImagePath(const std::string& p) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = p.substr(dot);
    for (size_t i = 0; i < ext.size(); i++) ext[i] = (char)tolower((unsigned char)ext[i]);
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || ext == ".gif";
}

static bool isVideoPath(const std::string& p) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = p.substr(dot);
    for (size_t i = 0; i < ext.size(); i++) ext[i] = (char)tolower((unsigned char)ext[i]);
    return ext == ".mp4" || ext == ".avi" || ext == ".mkv" || ext == ".mov";
}
// ── 不可播素材：标记与跳过（钟工 2026-10-01）──────────────────────────
//   背景：本机 VDEC 通道被写死成 H.264（见 wall/mi_h264_player_port.cpp 的 eCodecType），
//        喂 HEVC/其它编码进去**不抛异常、也不报错**，只是没有画面 —— 「轮播照走 10 秒、
//        屏幕全黑」这种静默失败，靠"报错再兜底"永远抓不到。所以两道关：
//        ① 扫描清单时先用 MediaGuard 读容器头预检（编码/容器/大小），不支持的当场标记；
//        ② 播放真出错（SimplePlayer 抛异常 / EasyUI 报 PLAY_ERROR）再按**文件**计数补标。
//   标记 = 文件改名 <name>.<ext>.bad：可见、掉电保留、立刻退出扫描清单（「以后不要播放」）。
static std::vector<std::string> sBadRuntime;   // 本次运行内判定不可播（未改名时的会话级跳过）
static std::string sBadPath;                   // 当前在计失败数的文件
static int         sBadCount = 0;              // 该文件连续失败次数

static bool isBadSkipped(const std::string& p) {
    if (MediaGuard::isMarked(p)) return true;
    for (size_t i = 0; i < sBadRuntime.size(); i++) if (sBadRuntime[i] == p) return true;
    return false;
}

// 把已标记 / 本次判定不可播的素材移出清单，返回剔除数量
static int dropMarked(std::vector<std::string>& v) {
    std::vector<std::string> keep;
    int n = 0;
    for (size_t i = 0; i < v.size(); i++) {
        if (isBadSkipped(v[i])) { n++; continue; }
        keep.push_back(v[i]);
    }
    if (n > 0) v.swap(keep);
    return n;
}

static bool sScreenOff = false;         // true = 当前黑屏（关屏时段内空闲到点）
static long long sLastTouchMs = 0;      // 本页最后交互时刻（关屏时段内空闲判定）
static std::string sLastAppName;        // 看门狗用：上一次看到的当前界面名（变化才打日志）
static long long sVideoStartedMs = 0;   // 当前视频开播时刻（保留：仅供日志参考）
// 播放/空闲计时改用**心跳 tick 计数**：TimeHelper::getCurrentTime() 在部分会话返回 0
// （NTP 校时前后不一致）-> 用墙钟算 elapsed 会永不推进（2026-09-25 实测踩到）
static int sMediaTicks = 0;             // 当前素材已显示秒数（每秒 +1）
static int sIdleTicks = 0;              // 本页空闲秒数（触摸/onUI_show 归零）

static void loadVideoList() {
    // 多屏拼接（钟工 2026-09-25）：启用且未失联时，本机只播自己的段（失联自动落回下面的普通轮播）
    WallLink* wl = WallLink::getInstance();
    if (wl->enabled() && wl->readyToPlay()) {
        sVideos.clear();
        // v5：playlist 模式给 v4 兜底用第 1 个 clip 的段（simple 引擎自己会在内部切 clip，不读这个清单）
        sVideos.push_back(wl->playlistMode() ? wl->segPathForClip(0) : wl->segPath());
        LOGD("wall: local segment %s (playlist=%d clips=%d)",
             sVideos[0].c_str(), wl->playlistMode() ? 1 : 0, wl->clipCount());
        return;
    }
    sVideos.clear();
    // 优先用 /data 里的选中清单（屏保视频子页配置；空 = 未配置 -> 退扫目录）
    // 钟工 2026-09-25：选中清单里**已不存在的路径要跳过**（否则清单全是死路径 -> 播什么都不出来，
    // 表现为“选了文件但屏保不轮播”）；若全部无效则自动退扫目录。
    std::string all = StoragePreferences::getString("sp_video_sel", "");
    int dropped = 0;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        std::string one = all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!one.empty()) {
            struct stat st = {0};
            if (stat(one.c_str(), &st) == 0) {
                sVideos.push_back(one);
            } else {
                dropped++;
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (!sVideos.empty()) {
        const int bad = dropMarked(sVideos);
        if (!sVideos.empty()) {
            LOGD("screensaver videos(from /data sel): %d (dropped %d missing, %d marked-bad)",
                 (int)sVideos.size(), dropped, bad);
            return;
        }
        LOGW("screensaver: 选中清单 %d 项全部不可播 -> 退扫目录", bad);   // 清空后 sVideos 已空，落到下面重扫
    }
    if (dropped > 0) LOGW("screensaver: all %d selected item(s) missing -> fallback to dir scan", dropped);
    // 素材目录：视频 + 相册上传落盘目录（钟工 2026-09-24 口径）；图片也扫（混播，3.4.5）
    // 用 opendir/readdir（不用 popen+ls：省进程创建、目录不存在也不报错）
    const char* dirs[] = { "/mnt/sdnand/video", "/mnt/sdnand/album",
                           "/mnt/extsd/video", "/mnt/usb1/video" };
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        DIR* dp = opendir(dirs[d]);
        if (dp == NULL) continue;
        std::vector<std::string> found;
        struct dirent* ent = NULL;
        while ((ent = readdir(dp)) != NULL) {
            std::string nm = ent->d_name;
            if (nm == "." || nm == ".." || nm[0] == '.') continue;
            std::string full = std::string(dirs[d]) + "/" + nm;
            if (isImagePath(full) || isVideoPath(full)) found.push_back(full);
        }
        closedir(dp);
        std::sort(found.begin(), found.end());          // 名称序（稳定，便于对账）
        for (size_t i = 0; i < found.size(); i++) sVideos.push_back(found[i]);
    }
    const int badScan = dropMarked(sVideos);
    LOGD("screensaver media: %d (%d marked-bad skipped)", (int)sVideos.size(), badScan);
}

// ── 素材变化检测（钟工 2026-09-25：新上传的素材不进轮播）────────────────
static void playItem(int index, bool force);   // 前向声明（定义在下方，默认参数在原声明处）

// 签名 = 选中清单原文 + 各素材目录里的候选文件（名称序拼接）
static std::string mediaSignature() {
    std::string sig = StoragePreferences::getString("sp_video_sel", "");
    const char* dirs[] = { "/mnt/sdnand/video", "/mnt/sdnand/album",
                           "/mnt/extsd/video", "/mnt/usb1/video" };
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        DIR* dp = opendir(dirs[d]);
        if (dp == NULL) continue;
        std::vector<std::string> found;
        struct dirent* ent = NULL;
        while ((ent = readdir(dp)) != NULL) {
            std::string nm = ent->d_name;
            if (nm == "." || nm == ".." || nm[0] == '.') continue;
            std::string full = std::string(dirs[d]) + "/" + nm;
            if (isImagePath(full) || isVideoPath(full)) found.push_back(nm);
        }
        closedir(dp);
        std::sort(found.begin(), found.end());
        for (size_t i = 0; i < found.size(); i++) {
            sig += '|';
            sig += dirs[d];
            sig += '/';
            sig += found[i];
        }
    }
    return sig;
}

static std::string sListSig;          // 上次构建清单时的素材签名

// 素材可用性预检（只在素材签名**变化**时跑：新上传/删改那一刻）：
//   把编码不支持（如 H.265）、容器损坏、空文件当场标记，免得先黑屏 10 秒才发现播不了。
//   自动标记可用 prefs `sp_ss_autobad=0` 关掉（关掉后只统计、不动文件）。
static void mediaAudit(bool sigChanged) {
    if (!sigChanged) return;
    const bool apply = MediaGuard::autoMarkEnabled();
    MediaGuard::AuditResult ar = MediaGuard::audit(apply);
    if (ar.unplayable > 0) {
        LOGW("screensaver audit: %d/%d 个素材不可播 -> 标记 %d 个（sp_ss_autobad=%d）",
             ar.unplayable, ar.scanned, ar.marked, apply ? 1 : 0);
    } else if (ar.scanned > 0) {
        LOGD("screensaver audit: %d 个素材全部可播", ar.scanned);
    }
}

static int sListCheckTicks = 0;       // 变化检测节流（每 5s 一次）
static bool sWallPending = false;     // 拼接模式：正在等下一个墙钟边界起播
static long long sWallTargetMs = 0;    // v2：本次起播目标 = 整边界**绝对墙钟 ms**（0 = 未定标）
static bool sWallLateRetried = false;  // v2：本段是否已做过“迟起播兜底”（一周期一次）
static long long sWallFireAdj = 0;     // v2 自校准：把“起播调用返回”对齐到整边界的修正量
static long long sWallStopMs = 120;    // v2 自校准：上一轮实测的 stop()（播放器拆解）耗时

// ── 播放内核开关（钟工 2026-09-26 10:04 拍板：多屏拼接改走内网 SimplePlayer 路线）──
//   sp_wall_engine = 0  v4      easyui ZKVideoView + 整边界两段式起播（已实测 |Δ|≤19ms 通过）
//                  = 1  simple  自读包 + I 帧追赶 + 墙钟按 PTS 控速（src/wall/WallPlayer.*）
// 默认 0：v4 是已验证通过的状态；新引擎验证通过后再现场切 1，随时可回退。
#define WALL_ENGINE_V4     0
#define WALL_ENGINE_SIMPLE 1
// simple 引擎接棒前，等 easyui 播放器的 MI 图层拆解完（实测 stop 后立即 start 会踩坏 disp 图层）
#define WALL_V4_SETTLE_MS  400
static int wallEngine() {
    int v = StoragePreferences::getInt("sp_wall_engine", WALL_ENGINE_V4);
    // v5（2026-09-27）：playlist（一组多视频轮播）**只有 simple 引擎**（WallPlayer）支持多 clip 切换，
    //   v4（easyui ZKVideoView）是单文件循环 —— 检测到 playlist 就强制 simple，只提示一次。
    if (v != WALL_ENGINE_SIMPLE && WallLink::getInstance()->playlistMode()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOGW("wall: playlist（多 clip）需要 simple 引擎 -> 本次强制 sp_wall_engine=1；"
                 "如需回退 v4：删掉组目录的 playlist.json（回到旧布局单 clip）");
        }
        return WALL_ENGINE_SIMPLE;
    }
    return (v == WALL_ENGINE_SIMPLE) ? WALL_ENGINE_SIMPLE : WALL_ENGINE_V4;
}
static std::string sWallSpFile;        // simple 引擎当前在播的段（换段才重开）
static int sWallSpFailTicks = 0;       // simple 引擎起播失败重试节流（每 5s 一次）

// ── 屏保播放内核（2026-10-01）────────────────────────────────────────
//   为什么需要它：本机固件的 **EasyUI 媒体栈没有视频解码器** ——
//   `libeasyui` 的 `InternalMediaPlayer` 运行时 dlopen 的 `/lib/libzkmedia.so` 只导出了
//   音频实现（media::ZKAudioPlayer / Mp3AudioParser / WavAudioParser，PlayerFactory 只注册音频），
//   所以 `ZKMediaPlayer(E_MEDIA_TYPE_VIDEO)` 拿不到实现 → 任何素材都在 ~45ms 内 `media play error`
//   （实测：High/Main/Baseline、有音轨/无音轨、ASCII/中文名、自制/出厂素材，**全部**失败）。
//   厂商自己的 app 是把 H.264 播放器**静态链进 app** 走 MI 硬解（其 libzkgui.so 10.9MB vs 我们 5.2MB）。
//   本项目已有同等内核 = 拼接用的 SimplePlayer（src/wall/WallPlayer.*，基于官方 simple-player 包）。
//   → 屏保视频默认走 MI 内核；`sp_ss_player=0` 可回退到 ZKVideoView（老固件/其它机型）。
//   ⚠️ 代价：simple-player 包**只有视频、完全没有音频代码**（env 只有 LOG/SYNC/ROTATION，
//      也没有 volume/MI_AO 相关符号），所以这条路上"屏保声音/静音键"无法生效。
#define SS_PLAYER_MI     1
#define SS_PLAYER_EASYUI 0
static int ssPlayerEngine() {
    return (StoragePreferences::getInt("sp_ss_player", SS_PLAYER_MI) == SS_PLAYER_EASYUI)
           ? SS_PLAYER_EASYUI : SS_PLAYER_MI;
}
static std::string sSsMiFile;          // MI 内核当前节目（同文件不重开，避免反复拆建 MI 图层）
static int sSsMiCycles = 0;            // 上次观察到的完成轮数（自增 = 当前片播到 EOF）
static int sSsMiErrStreak = 0;         // MI 内核连续失败次数

// ── 屏保页是否在前台（★★ 2026-10-01 修"在设置界面声音先出来"）────────────────
//   现场证据（钟工 21:35）：点屏离开屏保后 `ss[audio]: stop (hide)` 已停，
//   **25 秒后**日志又出现 `screensaver audit: 11` → `screensaver play[mi]` → `ss[audio]: play`，
//   而当时人在设置页 —— 手机刚上传的新素材让"素材清单签名"变了，屏保的 1s 心跳
//   （离开页面后**仍在跑**）5s 一次的 `refreshListIfChanged()` 检测到变化就 `playItem()`，
//   于是在**别的页面上**把视频与声音又起了一遍。
//   ⇒ 口径：**所有"起播"的入口（playItem / 音频同步 tick）都必须先确认屏保页在前台**。
//   清单本身照旧重建（这样回到屏保时是最新的），只是不再"在后台起播"。
static bool sSsVisible = false;

// ── 音画同步（2026-10-01，钟工："画面还没进屏保，音频先开始播放了"）──────────────
//   视频是**异步**起播：WallPlayer::start() 只起线程，线程里要建 H264Parser（解析 moov）
//   + mi::H264Player（MI_VDEC_InitDev / 2MB 码流缓冲 / DISP 图层 enable / bind）→ 几百 ms
//   才有第一帧。而 mp3 走另一个播放器（ss_audio → ZKMediaPlayer），立刻 play() 就出声
//   → 听感上"声音先出、画面后到"。
//   所以：**音频不立刻起播，先挂起**；等 `WallPlayer::firstFeedMs() != 0`
//   （= mi::H264Player::stream() 第一次被调用，解码器已经在吃第一包）**并且**距挂起已过
//   `SS_AUDIO_SYNC_MIN_MS` 再起播，误差 ≤50ms（本 tick 周期）。
//   ⚠️ 为什么还要"再等一段"（钟工 2026-10-01 二轮反馈"界面还在开关界面音频就出来了"）：
//      MI 视频层 ~77ms 就出画了，但**屏保页面（UI 层）比它画得慢** —— 只等喂流的话，声音会
//      在"屏幕还是上一页（开关界面）"时响。多等 MIN 毫秒保证屏保页面已经画出来。
//      代价 = 音频比画面晚 ~150ms（自然音效场景无感），换来"声音绝不抢在画面前"。
//      prefs `sp_ss_async_ms` 可现场调（0 = 只等喂流，不等页面）。
//   超时兜底 `SS_AUDIO_SYNC_MAX_MS`：画面始终没起（坏素材/建通道失败）就放弃本次音轨，不"只闻其声"。
#define SS_AUDIO_SYNC_MAX_MS  1500
#define SS_AUDIO_SYNC_MIN_MS  250
static std::string sSsAudioPending;    // 挂着等画面起来的音轨（空 = 无）
static long long sSsAudioArmedMs = 0;  // 挂起时刻（WallPlayer::monoMs()，单调钟）
static int sSsAudioMinMs = SS_AUDIO_SYNC_MIN_MS;   // 本次挂起的最小等待（挂起时读一次 prefs）

// 50ms 心跳：**单素材循环**时给每一轮重新对齐音轨（2026-10-01）
//   背景：WallPlayer::run() 内部 `while(!stopReq)` 自我循环同一文件，**不经过 playItem()**
//   → 音轨只在进入屏保时起播一次，"只有第一遍有声音"，后面几轮全静音。
//   放 1s 心跳里做会对不上（最多晚 1s，变成另一种音画错位），所以放 50ms tick。
//   只处理"单素材 + 无错"：多素材的切条、以及失败记账/退避仍由 1s 心跳负责（避免两处都消费边界）。
//   ★ 2026-10-01：多素材也有一类"本条自我循环"的情况 —— 被「轮播间隔」按住时（见 ssDwellHolding()），
//     此时同样不经过 playItem()，音轨也得每轮重对齐，所以这里的守卫也要放它过。
static bool ssDwellHolding();      // 定义在 videoIntervalSec() 之后
static void ssSingleLoopAudioTick() {
    if (!sSsVisible) return;                       // 屏保不在前台 -> 不起播
    if (ssPlayerEngine() != SS_PLAYER_MI) return;
    if (sSsMiFile.empty()) return;
    if (sVideos.size() > 1 && !ssDwellHolding()) return;   // 多素材正常切条：交给 1s 心跳（含起播）
    if (ConfigStore::getInstance()->ssEditMode() || sScreenOff) return;
    WallPlayer* wp = WallPlayer::getInstance();
    if (!wp->lastError().empty()) return;          // 有错 -> 留给 1s 心跳做失败记账/退避
    const int cyc = wp->cycles();
    if (cyc == sSsMiCycles) return;                // 没换轮
    sSsMiCycles = cyc;
    playItem(sVideoIdx, true);                     // 同片循环分支 -> 挂起音轨，等这一轮喂流
}

// 50ms 心跳里调：画面一开始喂流就把挂起的 mp3 起播（由主线程执行，StoragePreferences/播放器都归主线程）
static void ssAudioSyncTick() {
    if (!sSsVisible) return;        // 屏保不在前台 -> 一律不起播（见 sSsVisible）
    if (sSsAudioPending.empty()) return;
    const long long waited = WallPlayer::monoMs() - sSsAudioArmedMs;
    if (WallPlayer::firstFeedMs() == 0) {
        if (waited < SS_AUDIO_SYNC_MAX_MS) return;           // 画面还没起 -> 继续等
        LOGW("ss[audio]: sync 等待 %lldms 画面仍未起 -> 放弃本次音轨 %s",
             waited, sSsAudioPending.c_str());
        sSsAudioPending.clear();
        return;
    }
    if (waited < sSsAudioMinMs) return;                      // 画面已喂流，但屏保页面还没画稳 -> 再等
    const std::string p = sSsAudioPending;
    sSsAudioPending.clear();
    ss_audio::playForVideo(p);                               // 此刻起播，音画同步
    LOGD("ss[audio]: sync start %s（等到 %lldms，画面已喂流且页面已画）", p.c_str(), waited);
}

// 音轨挂起（统一入口；顺带把 prefs 读一次，别在 50ms tick 里反复读）
static void ssAudioArm(const std::string& path) {
    ss_audio::stop("switch");        // 先停掉上一片/上一轮的声音，避免它响到新画面出来
    sSsAudioPending = path;
    sSsAudioArmedMs = WallPlayer::monoMs();
    sSsAudioMinMs = StoragePreferences::getInt("sp_ss_async_ms", SS_AUDIO_SYNC_MIN_MS);
    if (sSsAudioMinMs < 0) sSsAudioMinMs = 0;
}

// 停 MI 内核（图片 / 熄屏 / 编辑态 都要先让出 MI 图层）
//   音频是**独立播放器**（ZKMediaPlayer），所以这里一并停掉：所有"停视频"的路径都经过本函数，
//   放这里等于一次性覆盖 图片/熄屏/编辑态/离开屏保/切内核 五种情况，不会出现"画面停了声音还在"。
static void ssMiStop(const char* why) {
    ss_audio::stop(why);
    sSsAudioPending.clear();          // 别再等画面了（本次要停）
    WallPlayer* wp = WallPlayer::getInstance();
    if (wp->running() || !sSsMiFile.empty()) {
        LOGD("ss[mi]: stop (%s) cycles=%d err=%s", why, wp->cycles(), wp->lastError().c_str());
    }
    wp->stop();
    sSsMiFile.clear();
    sSsMiCycles = 0;
}

// ── ① 边界预热踩点（钟工 2026-09-29 21:03）──────────────────────────────
//   起播不再"发现空闲就拉"，而是等「下一个整边界 − lead」再拉，让**第一帧正好落在整边界**
//   → 三屏同刻出画（不用 seek；官方 simple-player 包没有 seek API）。
//   lead 口径：sp_wall_start_lead_ms（默认 150ms）；也兼容已有的 sp_wall_lead_ms。
#define WALL_START_LEAD_MS  150
static bool sWallStartPending = false;          // 已排队等踩点
static std::string sWallStartPendingSeg;        // 排队的段（换段即取消）

static long long sWallStartTargetMs = 0;      // 踩点目标时刻（ms）
// 到点后由**主线程**执行起播：与 tick 同一条 MI 图层序列（stop v4 + 沉降 + start）
static void wallStartDo(const std::string& seg) {
    if (mVideoSsPtr != NULL && mVideoSsPtr->isPlaying()) {
        mVideoSsPtr->stop();
        LOGD("wall[simple]: (踩点) v4 player stopped -> settle %dms", WALL_V4_SETTLE_MS);
        usleep(WALL_V4_SETTLE_MS * 1000);
    }
    if (mImageSsBgPtr != NULL) mImageSsBgPtr->setVisible(false);
    sWallStartPending = false;
    LOGI("wall[simple]: 边界踩点起播 %s（第一帧落整边界）", seg.c_str());
    WallPlayer::getInstance()->start(seg, 0, 0, SS_SCR_W, SS_SCR_H);
}

static void wallSimpleStop(const char* why) {
    WallPlayer* wp = WallPlayer::getInstance();
    if (sWallStartPending) {                 // 取消未执行的踩点起播
        sWallStartPending = false;
        sWallStartPendingSeg.clear();
    }
    if (wp->running() || !sWallSpFile.empty()) {
        LOGD("wall[simple]: stop (%s) cyc=%d secs=%d", why, wp->cycles(), wp->secs());
    }
    wp->stop();
    sWallSpFile.clear();
    sWallSpFailTicks = 0;
}

// 设备倒装（钟工 2026-09-27）：
//   UI/触摸的旋转由 DisplayFlip::applyStored() 下发（幂等）；视频层**只能在没播放器跑的时候**
//   下发（MI DISP 层属性播放中改会踩坏通道）。开机与每次进屏保页各查一次。
static void applyFlipWhenIdle(const char* why) {
    const int mode = DisplayFlip::videoRotateMode();
    bool busy = WallPlayer::getInstance()->running()
                || (mVideoSsPtr != NULL && mVideoSsPtr->isPlaying());
    if (busy) {
        LOGD("flip180[%s]: player busy -> video layer keep mode=%d (下次起播自动带上)", why, mode);
        return;
    }
    const int r = DisplayFlip::applyVideoLayer();
    LOGI("flip180[%s]: video layer mode=%d ret=%d", why, mode, r);
}

// simple 引擎的 1s 心跳：相位由播放器自己锁（不用每轮 stop/play），
// 主线程只负责“该起就起 / 该停就停 / 每 5s 打一条自证日志”。
static void wallSimpleTick(WallLink* wl) {
    WallPlayer* wp = WallPlayer::getInstance();
    // v5：播放器“当前节目”键 —— playlist 模式 = playlist.json（**稳定**，播放器内部自己切 clip）；
    //     旧布局 = seg_<idx>.mp4。只用它判断“要不要重开播放器”，不能拿“当前 clip 文件”当键，
    //     否则每切一个 clip 都会被外层当成换节目 -> 反复拆重建 MI 图层。
    const std::string seg = wl->playKey();

    // 编辑态/熄屏态不该出画面（v4 里是靠 ZKVideoView 自己停播，simple 引擎要显式停）
    if (ConfigStore::getInstance()->ssEditMode() || sScreenOff) {
        wallSimpleStop("edit/off");
        return;
    }

    // 两个内核不能同时在显：simple 模式要 ZKVideoView 让位（MI 图层/解码通道）
    // 注意 2026-09-26 实测（71 = 从机）：easyui 播放器 stop() 的 MI 拆解是**异步**的，紧接着
    //    wp->start() 起 simple 引擎会踩在正在拆的图层上 -> disp chn0 留下一个卡死的 workingTask
    //    + 142 帧陈旧输入（/proc/mi_modules/mi_disp：UsrInjectQ_cnt=1、workingTask_cnt=1、
    //    FinishCnt 冻在 142，108 同版本没踩到 = 竞态）-> 表现就是「屏保页画面冻住不动」。
    //    所以 stop 之后留一段沉降时间，等 MI 拆完再建新图层。
    bool stoppedV4 = false;
    if (mVideoSsPtr != NULL && mVideoSsPtr->isPlaying()) {
        mVideoSsPtr->stop();
        stoppedV4 = true;
    }
    if (mImageSsBgPtr != NULL) mImageSsBgPtr->setVisible(false);
    if (stoppedV4) {
        LOGD("wall[simple]: v4 player stopped -> settle %dms before MI start", WALL_V4_SETTLE_MS);
        usleep(WALL_V4_SETTLE_MS * 1000);
    }

    if (!wp->running() || sWallSpFile != seg) {
        if (sWallStartPending) {
            if (sWallStartPendingSeg == seg) return;               // 已排队，等踩点线程
            sWallStartPending = false;                             // 换段 -> 取消旧排队
        }
        if (wp->running() || !sWallSpFile.empty()) wp->stop();
        if (sWallSpFailTicks > 0) { sWallSpFailTicks--; return; }   // 失败后歇 5s 再试
        // ① 边界预热踩点：有 epoch 且离整边界还早 -> 排队（等到边界前 lead 再拉播放器）
        // 逃生开关：sp_wall_start_lead_ms <= 0 -> 不踩点（立即起播，等价老行为）
        const int lead = StoragePreferences::getInt("sp_wall_start_lead_ms", wl->leadMs());
        const long long wait = wl->waitToBoundaryMs();
        if (lead > 0 && wl->hasEpoch() && wait > (long long)lead + 100) {
            sWallSpFile = seg;                                     // 占位：别让下一拍重复进这里
            sWallStartPending = true;
            sWallStartPendingSeg = seg;
            sWallStartTargetMs = wl->nextBoundaryMs() - lead;
            LOGI("wall[simple]: 边界踩点排队 %s 距边界=%lldms lead=%dms -> %.1fs 后起播（主线程 50ms 调度）",
                 seg.c_str(), wait, lead, (wait - lead) / 1000.0);
            return;
        }
        sWallSpFile = seg;
        wp->start(seg, 0, 0, SS_SCR_W, SS_SCR_H);
    }

    if (!wp->lastError().empty() && sWallSpFailTicks == 0) {
        LOGW("wall[simple]: start error -> %s (静默 5s 后重试)", wp->lastError().c_str());
        sWallSpFailTicks = 5;
    }

    if ((sMediaTicks % 5) == 0) {
        LOGD("wall[simple]: pkg=1 cyc=%d secs=%d period=%lldms anchor=%lldms begin=%lldms locked=%d catchup=%d pts=%lldms want=%lldms now=%lldms err=%lldms running=%d state=%s",
             wp->cycles(), wp->secs(), wp->periodMs(), wp->anchorMs(), wp->beginMs(),
             wp->locked() ? 1 : 0, wp->catchupFrames(),
             wp->lastPtsMs(), wp->lastWantMs(), wp->lastNowMs(), wp->lastErrMs(),
             wp->running() ? 1 : 0, wl->stateText().c_str());
        // v5：playlist 诊断（clips / 当前 clip / 当前文件）—— 现场对账用
        if (wl->playlistMode()) {
            LOGD("wall[simple]: playlist clips=%d total=%lldms clip=%d file=%s (local seg_%d/%d)",
                 wl->clipCount(), wl->totalMs(), wp->clipIndex(), wp->clipFile().c_str(),
                 wl->segIndex(), wl->panelCount());
        }
    }
}

// 精等到 deadline（毫秒级；只允许在临近起播时调用，单次阻塞 ≤1600ms）
// 为什么不能只靠 1s 心跳：两台心跳相位各不相同，靠“心跳里判断接近边界”只能做到 ±1s，
// 而目标是 ≤40ms（1 帧）——必须本地把“到点”这个瞬间等到毫秒级。
#define WALL_WAIT_MAX_MS 1600
static void wallWaitUntil(long long deadlineMs) {
    // 分片 ≤4ms：实测单次 usleep(几百 ms) 可超时 10~30ms，直接变成起播相位抖动
    for (int i = 0; i < 40000; i++) {             // 硬上限：防时钟跳变导致死等
        long long n = WallLink::wallNowMs();
        if (n <= 0) return;
        long long d = deadlineMs - n;
        if (d <= 0) return;
        if (d > WALL_WAIT_MAX_MS) return;         // 不该发生；宁可不等也不卡 UI
        if (d > 4) usleep(4000);
        else if (d > 1) usleep((useconds_t)((d - 1) * 1000));
        else usleep(200);
    }
}

// 素材变了就重建清单；保持当前项（越界则回第 0 项），非编辑/非息屏时立即切到当前项
static void refreshListIfChanged(bool force) {
    std::string sig = mediaSignature();
    if (!force && sig == sListSig) return;
    mediaAudit(sig != sListSig);        // 素材变了 -> 先预检并标记不可播项（再建清单）
    sListSig = sig;
    loadVideoList();
    if (sVideos.empty()) {
        LOGW("screensaver: media list is empty after refresh");
        return;
    }
    if (sVideoIdx >= (int)sVideos.size()) sVideoIdx = 0;
    LOGD("screensaver list refreshed -> %d items (idx=%d)", (int)sVideos.size(), sVideoIdx);
    if (!ConfigStore::getInstance()->ssEditMode() && !sScreenOff) {
        WallLink* wl = WallLink::getInstance();
        if (wl->enabled() && wl->readyToPlay()) {
            // v2：拼接模式下改清单也不抢跑（两台进页/改清单时刻不同步）-> 重新落到整边界
            sWallTargetMs = 0;
            LOGD("wall: list refreshed -> re-arm next boundary");
        } else {
            playItem(sVideoIdx, true);      // 立即切到最新清单里的当前项
        }
    }
}

// 播放失败按**文件**计数：同一文件连续失败达阈值（prefs `sp_ss_bad_after`，默认 2）才标记，
//   避免把偶发抖动（MI 图层交接、通道忙）误判成坏素材。返回 true = 本次已判定它不可播。
static bool noteVideoFail(const std::string& path, const std::string& why) {
    if (path.empty()) return false;
    if (isBadSkipped(path)) return true;
    if (path != sBadPath) { sBadPath = path; sBadCount = 0; }
    int need = StoragePreferences::getInt("sp_ss_bad_after", 2);
    if (need < 1) need = 1;
    if (++sBadCount < need) {
        LOGW("ss video: 播放失败 %d/%d %s（%s）", sBadCount, need, path.c_str(), why.c_str());
        return false;
    }
    sBadCount = 0;
    sBadPath.clear();
    if (MediaGuard::autoMarkEnabled() && MediaGuard::mark(path, why)) {
        LOGW("ss video: %s 已标记不可播并移出轮播（%s）", path.c_str(), why.c_str());
    } else {
        sBadRuntime.push_back(path);       // 关开关或改名失败 -> 至少本次运行不再播
        LOGW("ss video: %s 判定不可播（%s）-> 本次运行跳过（未改名，sp_ss_autobad=0）",
             path.c_str(), why.c_str());
    }
    return true;
}

static void playVideo(int index, bool force = false) {
    if (mVideoSsPtr == NULL) return;
    if (sVideos.empty()) return;                      // 无视频时露出静态底图（ImageSsBg）
    // 回退内核：音轨由 ZKVideoView 自己从 mp4 里出，**不要**再叠我们的 mp3（否则两路声音）
    ss_audio::stop("easyui-video");
    if (force && mVideoSsPtr->isPlaying()) mVideoSsPtr->stop();
    if (mVideoSsPtr->isPlaying()) return;             // 已在播就不重开（跨页连续）
    sVideoIdx = (index < 0 ? 0 : index) % (int)sVideos.size();
    mVideoSsPtr->setVolume(ConfigStore::getInstance()->ssVolumeScale());   // 屏保声音（默认开/60）
    mVideoSsPtr->play(sVideos[sVideoIdx].c_str(), 0);
    mVideoSsPtr->setVolume(ConfigStore::getInstance()->ssVolumeScale());   // 部分实现要 play 后才吃音量
    sVideoStartedMs = TimeHelper::getCurrentTime();
    if (mImageSsBgPtr != NULL) mImageSsBgPtr->setVisible(false);
    LOGD("screensaver play[%d/%d] %s", sVideoIdx, (int)sVideos.size(), sVideos[sVideoIdx].c_str());
}

static int videoIntervalSec() {
    return ConfigStore::getInstance()->videoIntervalSec();   // 0 = 连续（播完即切）
}

// ── 「轮播间隔」= 当前素材**停留多久才换下一条**（0 = 连续 = 播完即切）──────────────
//   面板上的档位：连续 / 1h / 2h / 6h / 12h / 每天 —— 都是"本条素材停留多久"，
//   所以「每天」的语义是"一个素材循环播一整天，第二天换下一个"（面板始终有画面）。
//   ✅ EasyUI 那条路一直是对的：播完回调里 `if (interval==0) 切下一条 else 重播本条`。
//   ❌ **MI 内核（本机默认）漏了这道门禁**：素材一播完（约 10s，WallPlayer 每轮报一次完成）
//      就无条件 `playItem(idx+1)` -> 「轮播间隔」在真机上**完全不生效**
//      （钟工 2026-10-01 现场：间隔设"每天"、选 3 条素材，仍然每 10 秒换一个）。
//
// ★★ 2026-10-03 二次修复（钟工现场：101.2 设「12 小时」+ 选中 3 条素材，却一直只播同一个）★★
//   上一版门禁用的是 `sMediaTicks` —— 1s 心跳的**秒数计数器**，而它在 **每次 onUI_show 都被清零**
//   （进屏保页走一次；而"触摸点亮 -> 面板跳主页 -> 15s 空闲 -> 回屏保"这条**必然路径**也走一次）。
//   现场用法下这个计数**永远从 0 重数** -> 1h/6h/12h/每天 这类长档位**永远到不了**
//   -> 观众看到的就是"一直播同一个文件"（素材本身没毛病：3 条都好好地在 sp_video_sel 里）。
//   新口径：**停留时长 = 本条素材的锚定时刻到现在的墙钟秒数**，锚点落 /data：
//     `sp_ss_hold_file`  = 正在计时的素材路径
//     `sp_ss_hold_start` = 锚定时刻（epoch 秒，字符串存，避开 32 位 int）
//     `sp_ss_hold_int`   = 锚定时生效的档位（改档位 -> 立刻按新档位重新计时）
//   ⇒ 页面来回来去 / 息屏 / 甚至重启都不会把计时打回 0；只有"真的换了素材"或"改了档位"才重锚。
//   ⚠️ 时钟未校时（epoch < 1e9）时锚点记 0 = "无时钟" -> 不切（由 1s 心跳里的补锚兜底）。
#define KEY_SS_HOLD_FILE  "sp_ss_hold_file"
#define KEY_SS_HOLD_START "sp_ss_hold_start"
#define KEY_SS_HOLD_INT   "sp_ss_hold_int"
static std::string sHoldFile;            // 停留计时对应的素材（空 = 还没锚定）
static long long   sHoldStartSec = 0;    // 该素材的锚定时刻（epoch 秒；0 = 无有效时钟）
static int         sHoldIv = -1;         // 锚定时生效的档位（-1 = 还没读过）

// 墙钟秒；**时钟未校时（1970/未同步）一律返回 0**，避免把脏时间当成"已停留很久"
static long long ssEpochSec() {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    if (ts.tv_sec < 1000000000L) return 0;
    return (long long)ts.tv_sec;
}
// 把停留计时锚定到 path（唯一的重锚动作）
static void ssHoldAnchor(const std::string& path) {
    sHoldFile = path;
    sHoldStartSec = ssEpochSec();
    sHoldIv = videoIntervalSec();
    StoragePreferences::putString(KEY_SS_HOLD_FILE, path);
    char b[32];
    snprintf(b, sizeof(b), "%lld", sHoldStartSec);
    StoragePreferences::putString(KEY_SS_HOLD_START, b);
    StoragePreferences::putInt(KEY_SS_HOLD_INT, sHoldIv);
    LOGI("ss hold: anchor %s -> start=%lld iv=%ds", path.c_str(), sHoldStartSec, sHoldIv);
}
// 开机/进页时从 /data 恢复（重启不打断 12 小时这种长档位）
static void ssHoldLoad() {
    sHoldFile = StoragePreferences::getString(KEY_SS_HOLD_FILE, "");
    sHoldStartSec = atoll(StoragePreferences::getString(KEY_SS_HOLD_START, "0").c_str());
    if (sHoldStartSec < 0) sHoldStartSec = 0;
    sHoldIv = StoragePreferences::getInt(KEY_SS_HOLD_INT, -1);
    LOGD("ss hold: load file=%s start=%lld iv=%d", sHoldFile.c_str(), sHoldStartSec, sHoldIv);
}
// 唯一的调用点 = playItem() 定下"真正要播的素材"处：换素材 / 改档位 / 首次 才重锚
static void ssHoldSyncTo(const std::string& path) {
    if (path.empty()) return;
    if (ssEpochSec() <= 0) return;      // 时钟还没校好（开机头几秒）-> 不写盘，等下一轮补锚
    if (sHoldStartSec <= 0 || path != sHoldFile || videoIntervalSec() != sHoldIv) {
        ssHoldAnchor(path);
    }
}
// 本条素材已停留多少秒（未锚定 / 无有效时钟 -> 0）
static long long ssHoldElapsedSec() {
    if (sHoldStartSec <= 0) return 0;
    const long long now = ssEpochSec();
    if (now <= 0 || now <= sHoldStartSec) return 0;
    return now - sHoldStartSec;
}
static bool ssDwellHolding() {
    const int iv = videoIntervalSec();
    if (iv <= 0) return false;                       // 「连续」档：播完即切
    if (sHoldStartSec <= 0) return true;             // 未锚定/无时钟 -> 这一拍先不切
    const long long now = ssEpochSec();
    if (now <= 0) return true;
    if (now < sHoldStartSec) {                       // 时钟回跳 -> 重锚，别把计时算成负数
        LOGW("ss hold: 时钟回跳 now=%lld < start=%lld -> 重锚", now, sHoldStartSec);
        ssHoldAnchor(sHoldFile);
        return true;
    }
    return ssHoldElapsedSec() < (long long)iv;       // 停留未到点 -> 按住
}

// ── 屏保声音 / 静音键（2026-10-01）──────────────────────────────
//   音量与开关存 ConfigStore（sp_ss_sound 默认开 / sp_ss_vol 默认 60）；
//   屏保页右上角喇叭键一键静音，触摸它**只切声音、不解屏保、不进主页**（见触摸处理）。
//   音量入口只有 ZKVideoView::setVolume(0.0~1.0)：SDK 的 mi-module 不暴露音频 API，
//   设备上只有 /dev/mi_ao 这个 MI 音频节点在（无 ALSA），所以这是唯一可行路径。
//   图标不用 emoji/字符（裁剪字库没有），是 tools/gen_mute_icons.js 生成的 PNG。
#define SS_MUTE_X 424
#define SS_MUTE_Y 20
#define SS_MUTE_W 40
#define SS_MUTE_H 40

static bool sMutePressed = false;       // 静音键按下中（抬起才切换，避免误触）

static void applySsVolume() {
    ss_audio::applyVolume();          // ★ 音频走 ZKMediaPlayer（MI 内核那条路上只有它出声）
    if (mVideoSsPtr == NULL) return;
    mVideoSsPtr->setVolume(ConfigStore::getInstance()->ssVolumeScale());
}

// 图标两态：声音开 = 绿色喇叭 + 声波；静音 = 灰喇叭 + 斜杠
// （背景图没有读回接口，所以无脑写一次；进页/切换时都会调用，成本可忽略）
static void refreshSsMuteIcon() {
    if (mImageSsMutePtr == NULL) return;
    const bool on = ConfigStore::getInstance()->ssSoundEnabled();
    mImageSsMutePtr->setBackgroundPic(on ? "images/ss_snd_on40.png" : "images/ss_snd_off40.png");
}

// 只在「当前素材是视频」且非编辑态/非熄屏时显示（图片素材没有声音，不需要按键）
static void refreshSsMuteVisible() {
    bool show = false;
    if (!ConfigStore::getInstance()->ssEditMode() && !sScreenOff && !sVideos.empty()) {
        const int i = sVideoIdx;
        if (i >= 0 && i < (int)sVideos.size()) show = !isImagePath(sVideos[i]);
    }
    // 2026-10-01：MI 内核原来**完全没有音频**（simple-player 包只有视频），喇叭键只能藏着；
    // 现在声音由独立的 ZKMediaPlayer(AUDIO) 出（见 ss_audio.h），所以这条路上也能静音了。
    // 判据改成"这条素材有没有同名 mp3"：没有音轨就没声音可静，显示出来反而误导。
    if (show && ssPlayerEngine() == SS_PLAYER_MI) {
        const int i = sVideoIdx;
        show = (i >= 0 && i < (int)sVideos.size()) && ss_audio::hasTrack(sVideos[i]);
    }
    setVisibleIfChanged(mImageSsMutePtr, show);
}

static void toggleSsMute() {
    ConfigStore* cfg = ConfigStore::getInstance();
    const bool on = !cfg->ssSoundEnabled();
    cfg->setSsSoundEnabled(on);
    applySsVolume();               // 正在播时立即生效
    refreshSsMuteIcon();
    LOGI("ss mute key -> sound %s (vol=%d)", on ? "ON" : "OFF", cfg->ssVolume());
}

static bool ssMuteHit(int x, int y) {
    if (mImageSsMutePtr == NULL || !mImageSsMutePtr->isVisible()) return false;
    return x >= SS_MUTE_X && x < SS_MUTE_X + SS_MUTE_W &&
           y >= SS_MUTE_Y && y < SS_MUTE_Y + SS_MUTE_H;
}

// 混播入口：图片 -> 铺图 15s；视频 -> 走 ZKVideoView（原逻辑）
static void playItem(int index, bool force = false);

// 坏素材兜底（钟工 2026-10-01）：本机解码器只吃 H.264，遇到 H.265/损坏文件会连续报
// E_MSGTYPE_VIDEO_PLAY_ERROR。原逻辑失败即切下一项，**列表只有 1 项时等于原地立刻重试**，
// 形成无延时死循环把 media 线程跑满一核（现场：load 2.98 / CPU 50% / 整机卡顿）。
// 这里加「整轮都失败就退避」：退回静态底图，30s 内不再试视频。
static int sVideoErrStreak = 0;              // 连续失败计数（成功播完清零）
// 坏素材退避剩余秒数（1s 心跳递减）。
// ⚠️ 2026-10-01 修：原来用墙钟 `sVideoFailUntilMs`（初值 0）+ 判断
//   `if (TimeHelper::getCurrentTime() < sVideoFailUntilMs) return;`。
//   而 TimeHelper::getCurrentTime() 在本平台返回的是**被截断成 32 位的 epoch 毫秒**
//   （实测是负值 ≈ -1.5e8），于是 `负数 < 0` 恒成立 -> playItem() **永远提前返回**，
//   屏保视频一律不播（日志里只有 screensaver media: N，永远没有 screensaver play[...]）。
//   改用本项目自己的惯用法：1s 心跳计数（同 sMediaTicks / sListCheckTicks），不依赖墙钟。
static int sVideoFailTicks = 0;

class SsVideoListener : public ZKVideoView::IVideoPlayerMessageListener {
public:
    virtual void onVideoPlayerMessage(ZKVideoView *pVideoView, int msg) {
        (void)pVideoView;
        switch (msg) {
        case ZKVideoView::E_MSGTYPE_VIDEO_PLAY_COMPLETED:
            sVideoErrStreak = 0;                // 能播完 -> 视为正常素材，清零失败计数
            LOGD("ss video completed idx=%d interval=%d", sVideoIdx, videoIntervalSec());
            // 拼接模式：**不立即再播**，而是等下一个墙钟整边界（实测“播完即播”每循环多 ~1.22s）
            if (WallLink::getInstance()->enabled() && WallLink::getInstance()->readyToPlay()) {
                if (sWallTargetMs == 0) sWallTargetMs = WallLink::getInstance()->nextBoundaryMs();
                LOGD("wall: segment done -> wait boundary (target=%lld)", sWallTargetMs);
                break;
            }
            if (videoIntervalSec() == 0) playItem(sVideoIdx + 1, true);    // 连续：播完即切（force 绕开守卫）
            else playItem(sVideoIdx, true);                               // 有间隔：本条循环
            break;
        case ZKVideoView::E_MSGTYPE_VIDEO_PLAY_ERROR:
            LOGW("ss video error, next");
            // v2 坑（2026-09-26 实测）：本机播放器播完后会**紧跟着**再报一条 ERROR（+112ms），
            // 上一版在这里直接 playItem -> 抢跑起播，把整边界对齐打乱（实测就是它把两台拆到相差 30s）。
            // 拼接模式下 ERROR 同样只等整边界（链式 target 已在起播时推进，无需额外处理）。
            if (WallLink::getInstance()->enabled() && WallLink::getInstance()->readyToPlay()) {
                if (sWallTargetMs == 0) sWallTargetMs = WallLink::getInstance()->nextBoundaryMs();
                LOGD("wall: segment error -> keep grid (target=%lld)", sWallTargetMs);
                break;
            }
            // 先按文件记账：同一文件连续失败达阈值 -> 标记不可播 + 移出轮播（钟工 2026-10-01）
            {
                const std::string cur = (sVideoIdx >= 0 && sVideoIdx < (int)sVideos.size())
                                        ? sVideos[sVideoIdx] : std::string();
                if (noteVideoFail(cur, "EasyUI 播放器报错")) {
                    refreshListIfChanged(true);       // 重建清单（内部会接着起播下一部）
                    break;
                }
            }
            // 坏素材兜底：整轮素材都起不来（多为 H.265/损坏）-> 退避回静态底图，别原地死循环
            if (!sVideos.empty() && ++sVideoErrStreak >= (int)sVideos.size()) {
                sVideoErrStreak = 0;
                sVideoFailTicks = 30;                                       // 30s 内不再试视频
                if (mVideoSsPtr != NULL && mVideoSsPtr->isPlaying()) mVideoSsPtr->stop();
                if (mImageSsBgPtr != NULL) {
                    mImageSsBgPtr->setBackgroundPic("images/ss_bg.png");
                    mImageSsBgPtr->setVisible(true);
                }
                sVideoStartedMs = TimeHelper::getCurrentTime();
                LOGW("ss video: %d item(s) unplayable -> back off 30s to still bg", (int)sVideos.size());
                break;
            }
            playItem(sVideoIdx + 1, true);
            break;
        default:
            break;
        }
    }
};
static SsVideoListener sSsVideoListener;

// 混播入口：图片 -> UI 层铺图；视频 -> MI 内核（默认）或 EasyUI ZKVideoView（回退）
static void playItem(int index, bool force) {
    if (!sSsVisible) {                     // ★ 屏保不在前台 -> 绝不在别的页面上起播（见 sSsVisible）
        LOGD("screensaver playItem 忽略（屏保不在前台）idx=%d", index);
        return;
    }
    // 拼接 + simple 引擎：easyui 播放器（ZKVideoView）**一律不起** —— 它和 simple 引擎抢 MI 图层，
    // 交接时实测会把 disp 图层任务卡死（屏保画面冻住）。屏保画面全部交给 WallPlayer。
    // 这里做成总闸：开机 onUI_init、编辑态退出、素材轮播兜底等所有起播路径都被它拦住。
    if (WallLink::getInstance()->enabled() && wallEngine() == WALL_ENGINE_SIMPLE) {
        if (mVideoSsPtr != NULL && mVideoSsPtr->isPlaying()) mVideoSsPtr->stop();
        return;
    }
    if (sVideos.empty()) return;
    const int prevIdx = sVideoIdx;
    sVideoIdx = (index < 0 ? 0 : index) % (int)sVideos.size();
    const std::string& path = sVideos[sVideoIdx];
    // ★ 计时只在**真的换了素材**时归零（2026-10-01，钟工「轮播间隔不生效」的另一半根因）：
    //   原来是无条件 `sMediaTicks = 0`，可是"同片循环"（走下面的早退分支）也调 playItem(sVideoIdx)——
    //   50ms 心跳每轮重对齐音轨、EasyUI 播完回调重播本条，都会把计数清零
    //   -> `sMediaTicks` 永远到不了「轮播间隔」的秒数，间隔形同不存在。
    if (sVideoIdx != prevIdx) sMediaTicks = 0;
    // ★ 2026-10-03：「轮播间隔」的停留计时锚点跟"真正要播的素材"走 —— 换素材/改档位才重锚，
    //   同片循环（同片早退分支、50ms 心跳重对齐音轨）走的是同一路径 -> path 不变 -> 不重锚，
    //   所以 12 小时这种长档位不会被页面来回切换打断。
    ssHoldSyncTo(path);
    refreshSsMuteVisible();                // 视频才显示静音键（图片素材无声音）

    if (isImagePath(path)) {
        ssMiStop("image");                 // 图片走 UI 层，MI 图层必须让位
        if (mVideoSsPtr != NULL && mVideoSsPtr->isPlaying()) mVideoSsPtr->stop();
        // 复用最底层垫层（ImageSsBg）铺图：它在视频层之上、时钟等浮层之下，不会盖住屏保信息
        if (mImageSsBgPtr != NULL) {
            mImageSsBgPtr->setBackgroundPic(path.c_str());
            mImageSsBgPtr->setVisible(true);
        }
        sVideoStartedMs = TimeHelper::getCurrentTime();
        LOGD("screensaver photo[%d] %s", sVideoIdx, path.c_str());
        return;
    }

    // ── 视频 · MI 硬解内核（本固件的唯一可用路径）──────────────
    if (ssPlayerEngine() == SS_PLAYER_MI) {
        if (sVideoFailTicks > 0) {                                  // 坏素材退避期：保持静态底图
            if (mImageSsBgPtr != NULL) mImageSsBgPtr->setVisible(true);
            return;
        }
        if (mVideoSsPtr != NULL && mVideoSsPtr->isPlaying()) {       // easyui 播放器让位 + 沉降
            mVideoSsPtr->stop();
            usleep(WALL_V4_SETTLE_MS * 1000);
        }
        if (path == sSsMiFile && WallPlayer::getInstance()->running()) {
            // 同片循环：画面不重开；但音轨可能已经放完（时长略有差异）→ 这里补一次起播（幂等）
            // ⚠️ 音画同步（2026-10-01）：**别在这里无条件起播音频** —— onUI_show/心跳会再调一次
            //    playItem()，那时 start() 刚把 running() 置真、画面还在建 MI 通道，
            //    音频会从这条早退分支"提前"响出来（实测 bypassed 一次：06.936 就起了）。
            //    判据用 firstFeedMs()：非 0 说明画面**已经开始喂流**，这时起播才算同步。
            if (WallPlayer::firstFeedMs() != 0) {
                ss_audio::restartForVideo(path);   // ★ 新一轮已开始喂流 -> 硬重开音轨（同轨"在播"也要重开）
            } else {
                ssAudioArm(path);                   // 画面还没喂流 -> 挂起，交给 sync tick
            }
            return;
        }
        if (mImageSsBgPtr != NULL) mImageSsBgPtr->setVisible(false);
        WallPlayer* wp = WallPlayer::getInstance();
        sSsMiFile = path;
        wp->start(path, 0, 0, SS_SCR_W, SS_SCR_H);
        // ⚠️ 基线必须在 start() **之后**取：start() 内部会 `im->cycles = 0` 起新线程，
        //    先取会拿到上一轮的值 -> 心跳里 `cyc != sSsMiCycles` 立刻成立 -> 每秒重开一次
        //    （实测：VDEC `initSeqCnt=21`、`DecFrmCnt=0`、DISP 收不到帧）。
        sSsMiCycles = wp->cycles();
        sVideoStartedMs = TimeHelper::getCurrentTime();
        // ★ 音画同步（2026-10-01）：**不在这里立刻起播 mp3**。画面此刻还在建 MI 通道/解码器，
        //   起播了就是"声音先出、画面后到"。统一挂起（ssAudioArm 里会先停掉上一片的声音），
        //   由 50ms 心跳（ssAudioSyncTick）在"画面已喂流 + 页面已画"之后起播。
        ssAudioArm(path);
        LOGD("screensaver play[mi %d/%d] %s（音频挂起等画面）", sVideoIdx, (int)sVideos.size(), path.c_str());
        return;
    }

    // ── 视频 · EasyUI ZKVideoView（sp_ss_player=0 回退路径）──────
    if (mVideoSsPtr == NULL) return;
    if (mImageSsBgPtr != NULL) mImageSsBgPtr->setBackgroundPic("images/ss_bg.png");   // 回退渐变底图
    if (sVideoFailTicks > 0) return;       // 坏素材退避期内不再尝试（见 sVideoFailTicks）
    playVideo(index, force);
}

// ── 屏保项：显隐 + 自由拖动（统一 5 组）────────────────────────
static ZKTextView* ssChip(int k) {
    ZKTextView* a[3] = { mImageSsChip1Ptr, mImageSsChip2Ptr, mImageSsChip3Ptr };
    return (k >= 0 && k < 3) ? a[k] : NULL;
}
static ZKTextView* ssIcon(int k) {
    ZKTextView* a[3] = { mImageSsDevIcon1Ptr, mImageSsDevIcon2Ptr, mImageSsDevIcon3Ptr };
    return (k >= 0 && k < 3) ? a[k] : NULL;
}
static ZKTextView* ssName(int k) {
    ZKTextView* a[3] = { mTextSsDevName1Ptr, mTextSsDevName2Ptr, mTextSsDevName3Ptr };
    return (k >= 0 && k < 3) ? a[k] : NULL;
}
static ZKTextView* ssState(int k) {
    ZKTextView* a[3] = { mTextSsDevState1Ptr, mTextSsDevState2Ptr, mTextSsDevState3Ptr };
    return (k >= 0 && k < 3) ? a[k] : NULL;
}

// ── 屏保状态条：三格「开/关」随继电器真实状态刷新（钟工 2026-09-28 问题单）──
//   改前：这三格只有 setPosition/setVisible，**全工程没有任何 setText** → 一直显示
//         ui/main.json 的静态文案（开/开/关），所以第三格永远显示"关"
//         （实测：三路全开 sp_relay_state=7 时，状态条第三格仍显示"关"）。
//   改法：真值只从 RelayManager 取；1s 轮询 + 值变了才 setText（避免每秒重绘）。
#define SS_STATE_ON   0x7BE0A3      // 开：绿（与首页/相册高亮同色）
#define SS_STATE_OFF  0x5F5F68      // 关：暗灰
static void refreshSsDevStates(bool force) {
    static int sLast[3] = { -1, -1, -1 };
    for (int k = 0; k < 3; k++) {
        ZKTextView* p = ssState(k);
        if (p == NULL) continue;
        int on = RelayManager::getInstance()->get(k + 1) ? 1 : 0;
        if (!force && sLast[k] == on) continue;
        sLast[k] = on;
        p->setText(on ? "开" : "关");
        p->setTextColor(0xFF000000 | (on ? SS_STATE_ON : SS_STATE_OFF));   // 0xAARRGGBB (alpha != 0)
        LOGD("ss state bar: dev%d -> %s", k + 1, on ? "ON" : "OFF");
    }
}

struct SsGroup {
    const char* key;
    ZKTextView* frame;      // 编辑模式虚线框垫层
    int dl, dt, dw, dh;     // 默认矩形（拖动夹取基准 + 位置落盘基准）
    int dx, dy;             // 当前偏移（相对默认位置）
    bool vis;               // 该组当前是否显示
};
static SsGroup sGroups[SS_GROUP_N];
static bool sGroupsReady = false;
static int sDragIdx = -1;
static int sDownX = 0, sDownY = 0, sDragBaseX = 0, sDragBaseY = 0;
static bool sDragMoved = false;
static long long sEditDownMs = 0;   // 编辑态本次按下的时刻（判"长按空白处复位"）
static bool sEditOnBtn = false;     // 本次按下是否落在「完成」按钮上（见 onmainActivityTouchEvent）

static void ssGroupsInit() {
    if (sGroupsReady) return;
    SsGroup g[SS_GROUP_N] = {
        { "time",      mImageEdTimePtr,    0,   86,  480, 140, 0, 0, true },
        { "date",      mImageEdDatePtr,    0,   236, 480, 28,  0, 0, true },
        { "weather",   mImageEdWeatherPtr, 188, 282, 168, 24,  0, 0, true },
        { "temphum",   mImageEdThPtr,      0,   326, 480, 28,  0, 0, true },
        { "statusbar", mImageEdBarPtr,     16,  396, 452, 40,  0, 0, true },
        // 黄历（2026-10-01）：顶部整条。宽 400 而不是 480 —— 右上角 (424,20,40,40) 是喇叭键，
        //   居中内容限制在 x24..424 内就不会被它压住（见 ui/_gen/add_ss_almanac.py）。
        { "almanac",   mImageEdAlmanacPtr, 24,  4,   400, 70,  0, 0, true },
    };
    for (int i = 0; i < SS_GROUP_N; i++) sGroups[i] = g[i];
    sGroupsReady = true;
}

// ── 绝对坐标 → 引擎坐标：**直通**（钟工 2026-10-01 真机标定结论）──────────
//   背景：官方 KB `knowledge/uicontrols/json-layer-rules.md` 写「运行期 setPosition 的负 left/top
//   会被当成从右/下算」（源自问题单 09251751-8），旧代码因此禁止负值（左/上夹到 0）。
//   本次要放开"任意位置摆动"，先做了两轮**真机标定**（改 prefs 注入坐标 → 重启 → 像素带量落点）：
//     · 480 宽控件（=父宽）传 x=-100 → 左边界落在 -101        （字面量；从右算也恰好同值，不可分辨）
//     · 400 宽控件    传 x=-216 → 左边界落在 -216            （像素带 0..152 vs 按 -216 算的 0..153）
//       ⇒ 若是"从右算"应落在 480-400-216 = -136（像素带应到 233），实测不是 ⇒ **负值是字面量**
//   结论：**本版本（Z20 / easyui 2.6.0）负坐标就是负坐标**，不需要任何编码。
//   保留这两个 choke point 的原因：万一以后换 SDK/平台又变回"从右算"，只改这两个函数即可，
//   不必去动 ssApplyGroup 里十几处调用点。
static inline int ssEncLeft(int absL, int /*w*/) { return absL; }
static inline int ssEncTop(int absT, int /*h*/) { return absT; }
// 所有浮层控件的位置都从这里出去（别再直接 setPosition）
static void ssSetPos(ZKBase* p, int absL, int absT, int w, int h) {
    if (p == NULL) return;
    p->setPosition(LayoutPosition(ssEncLeft(absL, w), ssEncTop(absT, h), w, h));
}

// 偏移应用到组内控件
static void ssApplyGroup(int i) {
    if (i < 0 || i >= SS_GROUP_N) return;
    const int ox = sGroups[i].dx, oy = sGroups[i].dy;
    switch (i) {
    case 0:
        ssSetPos(mTextSsClkPtr, 0 + ox, 86 + oy, 480, 140);
        break;
    case 1:
        ssSetPos(mTextSsDatePtr, 0 + ox, 236 + oy, 480, 28);
        break;
    case 2:
        ssSetPos(mImageSsWxIconPtr, 188 + ox, 282 + oy, 22, 22);
        ssSetPos(mTextSsWxPtr, 216 + ox, 280 + oy, 140, 26);
        break;
    case 3:
        ssSetPos(mTextSsThPtr, 0 + ox, 326 + oy, 480, 28);
        // PM2.5 行贴在温湿度行下面（同一组，拖动/显隐跟随温湿度）
        ssSetPos(mTextSsAqiPtr, 0 + ox, 356 + oy, 480, 24);
        break;
    case 4:
        for (int k = 0; k < 3; k++) {
            const int x = 16 + k * 153;
            ssSetPos(ssChip(k), x + ox, 396 + oy, 141, 40);
            ssSetPos(ssIcon(k), x + 12 + ox, 406 + oy, 20, 20);
            ssSetPos(ssName(k), x + 34 + ox, 404 + oy, 56, 24);
            ssSetPos(ssState(k), x + 91 + ox, 404 + oy, 40, 24);
        }
        break;
    case 5:
        // 黄历三行（农历+干支 / 宜 / 忌）
        ssSetPos(mTextSsAlmDatePtr, 24 + ox, 4 + oy, 400, 26);
        ssSetPos(mTextSsAlmYiPtr, 24 + ox, 30 + oy, 400, 22);
        ssSetPos(mTextSsAlmJiPtr, 24 + ox, 52 + oy, 400, 22);
        break;
    default:
        break;
    }
    if (sGroups[i].frame != NULL) {
        ssSetPos(sGroups[i].frame, sGroups[i].dl + ox, sGroups[i].dt + oy,
                 sGroups[i].dw, sGroups[i].dh);
    }
}

// 位置落盘：/data 键 sp_ss_pos_<key>_x/_y（SS_POS_UNSET = 默认位置）。
// 存的就是**绝对坐标**，可以是负数（= 摆到屏幕外）。
static void ssSaveGroupPos(int i) {
    if (i < 0 || i >= SS_GROUP_N) return;
    ConfigStore::getInstance()->setSsItemPos(sGroups[i].key,
                                             sGroups[i].dl + sGroups[i].dx,
                                             sGroups[i].dt + sGroups[i].dy);
}

static void ssLoadLayout() {
    ssGroupsInit();
    ConfigStore* cfg = ConfigStore::getInstance();
    int rawx[SS_GROUP_N], rawy[SS_GROUP_N];
    for (int i = 0; i < SS_GROUP_N; i++) {
        int x = SS_POS_UNSET, y = SS_POS_UNSET;
        cfg->ssItemPos(sGroups[i].key, x, y);
        rawx[i] = x;
        rawy[i] = y;
        // 只有哨兵 / 离谱值才算「未设置」→ 用默认位置。
        // ⚠️ 钟工 2026-10-01 改口径：**负值不再是脏数据**，是合法的"摆到屏外"，不能吃掉。
        if (x == SS_POS_UNSET || x > SS_POS_LIMIT || x < -SS_POS_LIMIT) x = SS_POS_UNSET;
        if (y == SS_POS_UNSET || y > SS_POS_LIMIT || y < -SS_POS_LIMIT) y = SS_POS_UNSET;
        sGroups[i].dx = (x == SS_POS_UNSET) ? 0 : (x - sGroups[i].dl);
        sGroups[i].dy = (y == SS_POS_UNSET) ? 0 : (y - sGroups[i].dt);
        ssApplyGroup(i);
    }
    // 布局自证（现场排"控件摆到哪儿去了"最省事的证据）：每组一行
    //   raw = prefs 里读到的原始值；abs = 我们模型里的绝对坐标；enc = 真正传给 setPosition 的编码值
    for (int i = 0; i < SS_GROUP_N; i++) {
        const int absL = sGroups[i].dl + sGroups[i].dx, absT = sGroups[i].dt + sGroups[i].dy;
        LOGD("ss layout[%d] %-9s raw=(%d,%d) abs=(%d,%d) box=(%dx%d) enc=(%d,%d)",
             i, sGroups[i].key, rawx[i], rawy[i], absL, absT,
             sGroups[i].dw, sGroups[i].dh, ssEncLeft(absL, sGroups[i].dw), ssEncTop(absT, sGroups[i].dh));
    }
    LOGD("ss layout loaded (edit=%d)", cfg->ssEditMode() ? 1 : 0);
}

// 复位：所有浮层回默认位置（编辑态**长按空白处**触发）
//   为什么需要：新口径允许把浮层摆到屏外（甚至整块出屏），没有复位口子就"找不回来"了。
static void ssResetAllPos() {
    ssGroupsInit();
    ConfigStore* cfg = ConfigStore::getInstance();
    for (int i = 0; i < SS_GROUP_N; i++) {
        cfg->resetSsItemPos(sGroups[i].key);      // 删键 -> 下次读回即"未设置" = 默认位置
        sGroups[i].dx = 0;
        sGroups[i].dy = 0;
        ssApplyGroup(i);
    }
    LOGD("ss layout: reset all -> default positions");
}

// 拖动范围：**不夹取**（钟工 2026-10-01：任意位置摆动，允许负坐标/整块出屏）。
//   旧口径是「左上夹到 0、右下至少留 40px」（问题单 09251751-8），现按新要求放开；
//   只留一个离谱值护栏，免得脏数据/手指抖动把坐标写到 ±SS_POS_LIMIT 之外。
//   摆出屏外的兜底 = 编辑态长按空白处复位（ssResetAllPos）。
static int ssGuardPos(int v) {
    if (v >  SS_POS_LIMIT) return  SS_POS_LIMIT;
    if (v < -SS_POS_LIMIT) return -SS_POS_LIMIT;
    return v;
}

static int ssHitTest(int x, int y) {
    for (int i = 0; i < SS_GROUP_N; i++) {
        if (!sGroups[i].vis) continue;
        int l = sGroups[i].dl + sGroups[i].dx, t = sGroups[i].dt + sGroups[i].dy;
        if (x >= l && x < l + sGroups[i].dw && y >= t && y < t + sGroups[i].dh) return i;
    }
    return -1;
}

static void ssApplyVisible() {
    ConfigStore* cfg = ConfigStore::getInstance();
    bool v[SS_GROUP_N];
    v[0] = cfg->ssItemVisible("time", true);
    v[1] = cfg->ssItemVisible("date", true);
    v[2] = cfg->ssItemVisible("weather", true);
    v[3] = cfg->ssItemVisible("temphum", true);
    v[4] = cfg->ssItemVisible("statusbar", true);
    v[5] = cfg->ssItemVisible("almanac", true);     // 黄历（2026-10-01）
    if (mTextSsClkPtr != NULL) mTextSsClkPtr->setVisible(v[0]);
    if (mTextSsDatePtr != NULL) mTextSsDatePtr->setVisible(v[1]);
    if (mImageSsWxIconPtr != NULL) mImageSsWxIconPtr->setVisible(v[2]);
    if (mTextSsWxPtr != NULL) mTextSsWxPtr->setVisible(v[2]);
    if (mTextSsThPtr != NULL) mTextSsThPtr->setVisible(v[3]);
    if (mTextSsAqiPtr != NULL) mTextSsAqiPtr->setVisible(v[3]);   // PM2.5 跟随「温湿度」开关
    for (int k = 0; k < 3; k++) {
        if (ssChip(k) != NULL) ssChip(k)->setVisible(v[4]);
        if (ssIcon(k) != NULL) ssIcon(k)->setVisible(v[4]);
        if (ssName(k) != NULL) ssName(k)->setVisible(v[4]);
        if (ssState(k) != NULL) ssState(k)->setVisible(v[4]);
    }
    if (mTextSsAlmDatePtr != NULL) mTextSsAlmDatePtr->setVisible(v[5]);
    if (mTextSsAlmYiPtr != NULL) mTextSsAlmYiPtr->setVisible(v[5]);
    if (mTextSsAlmJiPtr != NULL) mTextSsAlmJiPtr->setVisible(v[5]);
    for (int i = 0; i < SS_GROUP_N; i++) sGroups[i].vis = v[i];
}

// 编辑模式开/关（虚线框 + 「完成」按钮；帧只在编辑态显示）
static void ssEnterEdit(bool on) {
    ConfigStore::getInstance()->setSsEditMode(on);
    ssGroupsInit();
    ssApplyVisible();
    for (int i = 0; i < SS_GROUP_N; i++) {
        if (sGroups[i].frame != NULL) sGroups[i].frame->setVisible(on && sGroups[i].vis);
    }
    if (mImageSsEditDoneBgPtr != NULL) mImageSsEditDoneBgPtr->setVisible(on);
    if (mButtonSsEditDonePtr != NULL) mButtonSsEditDonePtr->setVisible(on);
    if (on) {
        if (mVideoSsPtr != NULL && mVideoSsPtr->isPlaying()) mVideoSsPtr->stop();
        ssMiStop("edit-on");               // MI 内核也要让出图层（编辑态用干净底图）
        if (mImageSsBgPtr != NULL) {
            mImageSsBgPtr->setBackgroundPic("images/ss_bg.png");   // 编辑态用干净底图
            mImageSsBgPtr->setVisible(true);
        }
        EASYUICONTEXT->setScreensaverEnable(false);   // 编辑期间禁止进屏保（钟工报告 2）
    } else {
        EASYUICONTEXT->setScreensaverEnable(true);
        EASYUICONTEXT->resetScreensaverTimeOut();
    }
    sIdleTicks = 0;
    // 底部提示行在编辑态换成操作说明（原来编辑态还显示「触摸任意处进入面板」，那是误导）。
    // 文案用字已过 tools/check_glyphs.js（裁剪字库缺字会静默空白）。
    if (mTextSsHintPtr != NULL) {
        mTextSsHintPtr->setText(on ? "拖动摆放 · 长按空白处恢复默认位置"
                                   : "触摸任意处进入面板");
    }
    refreshSsMuteVisible();        // 编辑态隐藏喇叭键（内部按实际可见性判断）
    LOGD("ss edit mode -> %d", on ? 1 : 0);
}

// 退出编辑态后的去向（钟工 2026-10-01）：**回「屏保显示」子页**，不再原地掉进屏保画面。
//   ⚠️ 这里不能只用 goBack()：`sssetLogic` 是用 `openActivity("mainActivity")` 进来的，
//      而引擎对"已在界面栈中的界面"的做法是**把它移到顶层**（官方 wiki interaction/activity_lifecycle.md：
//      "界面栈中存在即将要打开的界面 -> 只是将界面栈中对应的界面移动到顶层"），
//      也就是说进来的路上那串「设置 / 屏保显示」很可能已经被弹出；而本页又是**启动界面**，
//      goBack() 只会弹到启动界面为止（wiki："会将顶层的界面弹出，直到启动界面"）
//      -> 人会在屏保画面上原地不动，这正是"退出编辑后马上进入屏保"的现象。
//   改成显式打开屏保显示页：栈里还有它 -> 被移到顶层（等价 goBack）；已被弹掉 -> 新建一个（还是同一页）。
static void ssLeaveEditor() {
    sIdleTicks = 0;
    sLastTouchMs = nowMs();
    LOGD("ss edit done -> back to sssetActivity");
    EASYUICONTEXT->openActivity("sssetActivity");
}

// 「完成」/ 点空白 共用：退出编辑态并回「屏保显示」
static void ssExitEditToSsset() {
    ssEnterEdit(false);
    ssLeaveEditor();
}

// 编辑态：「完成」按钮的命中判定（取控件自己的坐标，不硬编码尺寸 —— 改 json 后仍然对）
static bool ssHitDoneBtn(float x, float y) {
    if (mButtonSsEditDonePtr == NULL) return false;
    const LayoutPosition& p = mButtonSsEditDonePtr->getPosition();
    const int w = (p.mWidth > 0) ? p.mWidth : 152;
    const int h = (p.mHeight > 0) ? p.mHeight : 44;
    return (x >= p.mLeft && x < p.mLeft + w && y >= p.mTop && y < p.mTop + h);
}

// 屏保底部状态芯片的设备名：从业务单例（ConfigStore /data）读，主页改名后同步显示
static void refreshSsDevNames() {
    ConfigStore* cfg = ConfigStore::getInstance();
    for (int i = 0; i < 3; i++) {
        ZKTextView* p = ssName(i);
        if (p == NULL) continue;
        std::string n = cfg->relayName(i + 1);
        std::string out;                     // 芯片宽 141：名字最多 3 个汉字（UTF-8 安全截断）
        int chars = 0;
        for (size_t k = 0; k < n.size() && chars < 3;) {
            unsigned char c = (unsigned char)n[k];
            size_t len = (c < 0x80) ? 1 : ((c < 0xE0) ? 2 : 3);
            if (k + len > n.size()) break;
            out.append(n, k, len);
            k += len;
            ++chars;
        }
        p->setText(out.c_str());
    }
}

static void refreshSsClock() {
    struct tm* t = TimeHelper::getDateTime();
    if (t == NULL) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    if (mTextSsClkPtr != NULL) mTextSsClkPtr->setText(buf);
    snprintf(buf, sizeof(buf), "%04d年%02d月%02d日 星期%s",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, kWeek[t->tm_wday]);
    if (mTextSsDatePtr != NULL) mTextSsDatePtr->setText(buf);
}

// ── 屏保：真实天气 / 温湿度 / PM2.5（2026-10-01 重做）────────────
//   改前：这三行字是 ui/main.json 里**写死的静态文案**（「晴 26」「26.5°C / 58 %」），
//         TextSsWx / TextSsTh 这两个控件在 C++ 里 0 次 setText —— 不是"演示数据"，是贴图。
//   改后：全部由 SensorManager（Open-Meteo，真实数据）驱动；没有真实数据时显示「获取中」，
//         **不编造数值**。这个定时器 1s 一次，值没变不 setText（避免每秒重绘）。
//   文案用字注意：字库是裁剪的，不能用 ℃ ° · μ ³（主字库无这些字形）—— 见 tools/check_glyphs.js。
//   注：文本行用 setTextIfChanged()（判据 = 控件当前实际文本，见 logic/ui_text.h），
//       不依赖本地 static 缓存 —— 从别的页回到本页时控件若被重建，缓存会失效导致永不刷新。
//       force 只保留给「图标」用（背景图没有读回接口，只能靠缓存 + 进页强制刷一次）。
static void refreshSsWeather(bool force) {
    SensorManager* sm = SensorManager::getInstance();
    static int sLastIconCode = -2;
    static bool sLastHas = false;

    const bool has = sm->hasData();
    char b[96];

    // ① 天气行：文案 + 温度（如「小雨 30C」）
    std::string wx;
    if (!has) {
        wx = "获取中";
    } else {
        snprintf(b, sizeof(b), "%s %.0fC", sm->weather().c_str(), sm->temp());
        wx = b;
    }
    setTextIfChanged(mTextSsWxPtr, wx);

    // ② 温湿度行（如「30.0C / 72 %」）
    std::string th;
    if (!has) {
        th = "温湿度获取中";
    } else {
        snprintf(b, sizeof(b), "%.1fC / %.0f %%", sm->temp(), sm->hum());
        th = b;
    }
    setTextIfChanged(mTextSsThPtr, th);

    // ③ PM2.5 行（如「PM2.5 28 良 AQI 75」）
    std::string air;
    if (sm->pm25() < 0.0f) {
        air = "PM2.5 获取中";
    } else if (sm->aqi() >= 0) {
        snprintf(b, sizeof(b), "PM2.5 %.0f %s AQI %d",
                 sm->pm25(), sm->aqiLevel().c_str(), sm->aqi());
        air = b;
    } else {
        snprintf(b, sizeof(b), "PM2.5 %.0f %s", sm->pm25(), sm->aqiLevel().c_str());
        air = b;
    }
    setTextIfChanged(mTextSsAqiPtr, air);

    // ④ 天气图标（晴/多云/阴/雾/雨/雪）
    const int code = sm->wxCode();
    if (force || code != sLastIconCode || has != sLastHas) {
        sLastIconCode = code;
        sLastHas = has;
        applyWeatherIcon(mImageSsWxIconPtr, has ? sm->weather() : std::string(), "22");
    }
}

// ── 屏保：黄历（农历/干支/宜/忌；钟工 2026-10-01）───────────────
//   数据源与排版都在 AlmanacManager（后台线程拉 → /data 缓存 → 主线程 tick 落盘）。
//   没有数据时第一行给「黄历获取中」，宜/忌留空 —— 不编造内容。
//   三行都用 setTextIfChanged（判据 = 控件当前实际文本，见 logic/ui_text.h），
//   所以从别的页回到屏保时也能正确刷新。
static void refreshSsAlmanac() {
    AlmanacManager* am = AlmanacManager::getInstance();
    setTextIfChanged(mTextSsAlmDatePtr, am->line1());
    setTextIfChanged(mTextSsAlmYiPtr,   am->line2());
    setTextIfChanged(mTextSsAlmJiPtr,   am->line3());
}

// ── 息屏链路（钟工 2026-09-24 23:40 口径；2026-10-01 二轮修）
//   (1) 熄屏状态点屏幕 -> 立即解除熄屏**并直接进主界面**（不再先停在屏保画面上等第二下触摸）
//   (2) 关屏时段内空闲 15s -> 熄屏（黑屏，不显屏保画面）；时段外空闲 15s -> 系统屏保（本页）
//   ★ 2026-10-01 钟工口径：「如果检测到非主界面窗口，一直重置进入屏保和关屏的定时器」：
//     本页的 1s 心跳在**被 EasyUI 隐藏后照样在跑**（homeLogic 当年踩过同一个坑），
//     所以空闲计数与熄屏判定都必须先确认**本页真的在前台**（sSsVisible）。
//     否则：人在设置子页待着，屏保页的空闲计时照样累加 -> 15s 后把屏"关掉"，
//     或者把人从子页拽进屏保（同 homeLogic 的 sVisible 门控）。
// ── 屏保播放：界面生命周期是唯一事实来源 ──────────────────────────
//   onShow 起播 / onHide·onQuit·熄屏 停播，全部走下面两个函数，不在各处手抄分支
//   （之前 leaveScreenOff 里抄了一份 onShow 分支 → 改一处漏一处，正是本次"屏保不恢复"的根因风格）。
static void ssStartPlayback() {
    if (!sSsVisible) return;                                   // 屏保不在前台不起播（playItem 也会拦）
    if (ConfigStore::getInstance()->ssEditMode()) return;      // 编辑态只预览布局，不起播
    if (sVideos.empty()) loadVideoList();
    WallLink* wl = WallLink::getInstance();
    if (wl->enabled() && wallEngine() == WALL_ENGINE_SIMPLE) {
        wallSimpleTick(wl);                                   // simple 引擎：立即起播（播放器内部拉齐+直跳）
    } else if (wl->enabled() && wl->readyToPlay()) {
        sWallTargetMs = 0;                                    // v2 拼墙：重定标到下个整边界
    } else {
        playItem(sVideoIdx);                                  // 单机：普通起播
    }
}

static void ssStopPlayback() {
    ssMiStop("lifecycle");                                    // 音频 + MI 图层（覆盖图片/熄屏/编辑/离开/切内核）
    wallSimpleStop("lifecycle");                              // simple 引擎：取消踩点 + 停 WallPlayer
    WallPlayer::getInstance()->stop();
    if (mVideoSsPtr != NULL && mVideoSsPtr->isPlaying()) mVideoSsPtr->stop();
}

// ★ 2026-10-02 临时验收埋点（ssDbgMark + /data/ss_dbg.log 标记）已删除：本板 logcat 不可用、
//   视频在 MI 叠加层截屏看不到，原用于不依赖 logcat 验证"恢复起播"；分钟级窗口复测通过后移除。

static void enterScreenOff() {
    if (sScreenOff) return;
    LOGD("enter screen-off (off period)");
    ssStopPlayback();                                         // ★ 熄屏即停播（背光关了播视频没意义）；本页不 hide -> sSsVisible 仍 true
    BRIGHTNESSHELPER->screenOff();
    sScreenOff = true;
}

static void leaveScreenOff(bool resume);
static void leaveScreenOff() { leaveScreenOff(true); }   // 缺省 = 恢复起播（tick 自动出熄屏路径）

// resume=false：触摸唤醒 -> 马上要 openActivity 进主页，别起播（会被 onUI_hide 立刻停掉）；
// resume=true ：关屏时段结束/子页顶黑屏的自动恢复，本页还在前台时必须把视频重新拉起来。
static void leaveScreenOff(bool resume) {
    if (!sScreenOff) return;
    LOGD("leave screen-off -> screen on");
    BRIGHTNESSHELPER->screenOn();
    BRIGHTNESSHELPER->setBrightness(ConfigStore::getInstance()->screensaverBrightness());
    sScreenOff = false;
    if (!resume) return;                   // 触摸唤醒/退出：马上要进主页或已销毁，别起播（onShow 回来再起）
    // ★ 屏保页熄屏期间从未 hide（只关背光），onUI_show 不会重跑；复用生命周期的起播函数。
    sMediaTicks = 0;                       // 熄屏期间不算"停留时长"
    sIdleTicks = 0;
    ssStartPlayback();
}

static void tickScreenOff() {
    struct tm* t = TimeHelper::getDateTime();
    if (t == NULL) return;
    ConfigStore* cfg = ConfigStore::getInstance();
    const bool inOff = cfg->inOffPeriod(t->tm_hour, t->tm_min);
    if (sScreenOff) {
        // 已熄屏：出关屏时段 **或** 本页已不在前台 -> 立刻恢复（别让子页顶着黑屏）
        if (!inOff || !sSsVisible) leaveScreenOff();
        return;
    }
    // 熄屏三条件齐了才熄：关屏时段内 + 本页在前台 + 非编辑态 + 空闲到点
    if (inOff && sSsVisible && !cfg->ssEditMode() && sIdleTicks >= SS_TIMEOUT_SEC) {
        enterScreenOff();
    }
}

// ── 「非主界面窗口不进屏保」看门狗（钟工 2026-10-01 口径）──────────────────────
//   规则：只有**主界面（homeActivity）**允许系统屏保计时自然走完；当前窗口一旦不是主界面
//   （设置 / 屏保显示 / 相册 / 屏保编辑态 …），每秒把系统屏保计时复位 -> 绝不自动跳进屏保。
//   ⚠️ 主界面自己空闲 15s 照旧会进屏保：那是 homeLogic 的 `goHome()`（本页是启动页），
//      与本看门狗无关 —— 所以即使 currentAppName() 拿到的名字和预期不同（最坏是永远复位），
//      屏保链路也不会被破坏，只是退化回 homeLogic 那条路。
//   与本文件里其它"是否前台"判据的区别：sSsVisible 是**本页**的前台标志，
//   本条问的是**全应用**当前是哪个界面（EASYUICONTEXT->currentAppName()）。
static void tickScreensaverGate() {
    const char* cur = EASYUICONTEXT->currentAppName();
    if (cur != NULL && sLastAppName != cur) {
        sLastAppName = cur;
        LOGD("ss watchdog: currentAppName=%s -> %s", cur,
             (std::string(cur) == "homeActivity" || std::string(cur) == "home.ftu")
                 ? "主界面（屏保计时放行）" : "非主界面（屏保计时持续复位）");
    }
    if (cur == NULL) return;
    const std::string n(cur);
    if (n != "homeActivity" && n != "home.ftu") EASYUICONTEXT->resetScreensaverTimeOut();
}

// ── 系统回调 ─────────────────────────────────────────────
static void onUI_init() {
#ifdef FUN_BUILD
    INIT_UI_TIMERS
#endif // FUN_BUILD
    sSsVisible = true;          // ★ 屏保在前台（onUI_init 也会直接起播，先置位，见 sSsVisible）
    NetKeeper_start();          // 只要程序在跑就保活 WiFi（钟工 2026-09-24 口径）
    // Z20/zkdisplay 板：不设 sys.zkapp.state=running 会永停开机 logo
    SystemProperties::setString("sys.zkapp.state", "running");
    SystemProperties::setString("sys.zkapp.dbg", "main:onUI_init");
    ConfigStore::getInstance()->init();        // /data 参数（名称/亮度/关屏时段/屏保项）
    RelayManager::getInstance()->init();      // 继电器先就绪（恢复断电前状态；HA 上电上报才是真值）
    SceneManager::getInstance()->init();      // 情景定义就绪（HA discovery 要发情景按钮）
    // 环境数据（真实天气/温湿度/PM2.5）：先读上次缓存显示，再起后台拉取线程
    //（curl_global_init 在里面做，必须在任何 HTTP 之前、由主线程完成）
    SensorManager::getInstance()->init();
    SensorManager::getInstance()->startAuto();
    // 黄历（屏保顶部）：同样"先读上次缓存、再起后台拉取"；首拉延迟 15s（等 WiFi/NTP）
    AlmanacManager::getInstance()->init();
    AlmanacManager::getInstance()->startAuto();
    LocalLink::getInstance()->start();        // 按已存运行模式拉起链路（HA 模式内部只记日志，不起 broker）
    MqttBridge::getInstance()->init();        // HA 模式：连外接 broker + HA discovery（内部自判模式/开关）
    // 局域网外部控制 API（UDP 5050，小程序/其他设备发现与控制）+ 状态变化广播。
    // 钟工 2026-09-27：本函数以前全工程无调用点 -> PanelLink 内部 mRelay 恒 nullptr，
    // prefix/status 的 relays 恒为 false（HA 开关状态自相矛盾）。
    PanelLink::getInstance()->init(RelayManager::getInstance(),
                                   SceneManager::getInstance(),
                                   SensorManager::getInstance());
    ClockManager::getInstance()->init();      // 上电即校时（之后每日自动刷新）
    // 息屏链路：空闲 15s -> 系统屏保（本页）；关屏时段内空闲 15s -> 黑屏（tickScreenOff）
    EASYUICONTEXT->setScreensaverTimeOut(SS_TIMEOUT_SEC);
    EASYUICONTEXT->registerGlobalTouchListener(&sGlobalTouchReset);   // 任意页面触摸都复位屏保计时
    WallLink::getInstance()->init();          // 多屏拼接：读配置 + 起 UDP epoch 通道
    // 设备倒装（钟工 2026-09-27）：开机按 /data 的 sp_flip180 上屏（UI+触摸），并下发视频层旋转。
    //   为何不放在 Main.cpp 的 onEasyUIInit：那一阶段 disp/fbdev 还没 init，libeasyui 里
    //   _fbdev_set_rotate 会直接 return（旋转值被丢掉），必须等本页（首页）真的起画面后再调用。
    DisplayFlip::applyStored();
    applyFlipWhenIdle("init");
    mediaAudit(true);                         // 开机先预检素材：不可播的当场标记（编码不支持等）
    loadVideoList();
    ssHoldLoad();                             // ★ 轮播停留计时：从 /data 恢复锚点（重启不打断长档位）
    refreshSsDevNames();
    refreshSsDevStates(true);
    if (mVideoSsPtr != NULL) mVideoSsPtr->setVideoPlayerMessageListener(&sSsVideoListener);
    refreshSsClock();
    refreshSsWeather(true);
    refreshSsAlmanac();
    refreshSsMuteIcon();
    refreshSsMuteVisible();
    ssLoadLayout();
    ssApplyVisible();
    ssEnterEdit(ConfigStore::getInstance()->ssEditMode());   // 从「屏保显示 -> 编辑位置」进来时直接进编辑态
    sLastTouchMs = nowMs();
    sIdleTicks = 0;
    sMediaTicks = 0;
    if (!ConfigStore::getInstance()->ssEditMode()) playItem(0);
}

static void onUI_intent(const Intent *intentPtr) {
    if (intentPtr != NULL) {
        // 子页返回参数按需扩展
    }
}

static void onUI_show() {
    sSsVisible = true;               // ★ 屏保在前台（playItem/音画同步都以它为准，见 sSsVisible 注释）
    refreshSsClock();
    refreshSsWeather(true);
    refreshSsAlmanac();
    refreshSsMuteIcon();
    refreshSsMuteVisible();
    refreshSsDevNames();
    refreshSsDevStates(true);
    DisplayFlip::applyStored();      // 倒装：从设置页回来也立即对齐（幂等，已经是目标角就不动）
    applyFlipWhenIdle("show");
    BRIGHTNESSHELPER->setBrightness(ConfigStore::getInstance()->screensaverBrightness());
    ssLoadLayout();
    ssApplyVisible();
    ssEnterEdit(ConfigStore::getInstance()->ssEditMode());
    sLastTouchMs = nowMs();
    sIdleTicks = 0;
    sMediaTicks = 0;
    ssStartPlayback();                      // ★ 进屏保页即起播（生命周期：onShow 起播）
}

static void onUI_hide() {
    sSsVisible = false;      // ★ 先置位再停播放器：停的过程中任何 tick 都不许再起播
    BRIGHTNESSHELPER->setBrightness(ConfigStore::getInstance()->workBrightness());
    ssStopPlayback();        // 离开屏保页：放开 MI 视频图层 + 停音频/解码（生命周期：onHide 停播）
}

static void onUI_quit() {
    sSsVisible = false;                     // ★ 同上：退出即视为不在前台
    leaveScreenOff(false);                  // 本页被销毁后 1s 心跳不再跑 -> 别把屏留在黑屏态（无需起播）
    EASYUICONTEXT->unregisterGlobalTouchListener(&sGlobalTouchReset);
    ssStopPlayback();                       // ★ 退出屏保页：停音频/视频/解码（生命周期：onQuit 停播）
    sWallSpFile.clear();
    sWallSpFailTicks = 0;
    sSsMiFile.clear();
    sSsMiCycles = 0;
    sSsMiErrStreak = 0;
    if (mVideoSsPtr != NULL) {
        mVideoSsPtr->setVideoPlayerMessageListener(NULL);
    }
}

static void onProtocolDataUpdate(const SProtocolData &data) {
    (void)data;
}

static bool onUI_Timer(int id) {
    if (id == 1) {                                   // 50ms：拼墙踩点调度（到点后主线程起播）
        if (sWallStartPending && nowMs() >= sWallStartTargetMs) {
            wallStartDo(sWallStartPendingSeg);
        }
        ssSingleLoopAudioTick();                     // 50ms：单素材循环 -> 每轮重新对齐音轨
        ssAudioSyncTick();                           // 50ms：音画同步——画面一起喂流就起播挂起的 mp3
        return true;
    }
    switch (id) {
    case 0: {
        refreshSsClock();
        // 环境数据：先把后台线程刚拉到的真实值提交落盘并生效（StoragePreferences 只由主线程写），
        // 再刷新屏保文案 —— 顺序反过来会晚 1 秒才显示新值。
        SensorManager::getInstance()->tick();
        refreshSsWeather(false);
        // 黄历：先提交后台线程刚拉到的结果并落盘，再刷新文案（顺序同天气）
        AlmanacManager::getInstance()->tick();
        refreshSsAlmanac();
        refreshSsMuteVisible();     // 当前素材是视频才显示静音键（内部按实际可见性判断，无变化不写）
        refreshSsDevNames();
        refreshSsDevStates(false);
        ClockManager::getInstance()->tick();   // 未同步则重试；已同步则每日刷新
        MqttBridge::getInstance()->tick();     // HA 链路看门狗（断线重连）
        // 多屏拼接：epoch 收发/失联判定/漂移统计
        WallLink::getInstance()->tick();
        {
            WallLink* wl = WallLink::getInstance();
            if (wl->enabled()) {
                // v7.13（方案B）：simple 引擎**不再要求 readyToPlay（= 有主机 epoch）**——
                //   相位与本机钟对齐完全靠 NTP；断网也能播、也不影响两屏一致。
                if (wallEngine() == WALL_ENGINE_SIMPLE) {
                    // ── simple 引擎（钟工 2026-09-26 拍板）：相位由播放器自己锁 ──
                    // 不参与 v4 的每轮 stop/play；主线程只做“该起就起/该停就停”与自证日志。
                    sWallPending = false;
                    sWallTargetMs = 0;
                    wallSimpleTick(wl);
                } else if (wl->readyToPlay()) {
                    // ── v2 起播链（2026-09-26）：目标 = 下一整边界绝对墙钟，本地精等到点起播 ──
                    // 两台各自算同一个时刻（同 epoch + 同公式），因此**不靠互发开始信令**，
                    // UDP 抖动/心跳相位差都不进相位；起播后 target += seg 链式推进，仍恒落整边界。
                    if (sWallTargetMs == 0) {
                        sWallTargetMs = wl->nextBoundaryMs();
                        sWallFireAdj = 0;
                        LOGD("wall: arm target=%lld now=%lld in=%lldms lead=%dms",
                             sWallTargetMs, WallLink::wallNowMs(),
                             sWallTargetMs - WallLink::wallNowMs(), wl->leadMs());
                    }
                    if (sWallTargetMs > 0) {
                        long long noww = WallLink::wallNowMs();
                        long long lead = wl->leadMs();
                        long long fire = sWallTargetMs - lead + sWallFireAdj;  // 自校准后的实际发起时刻
                        if (noww > sWallTargetMs + 250) {
                            // 迟到 >250ms（本边界已错过/时钟跳变）-> 跳到下一个整边界，不让误差累积
                            long long nb = wl->nextBoundaryMs();
                            LOGW("wall: miss boundary target=%lld now=%lld -> re-arm %lld",
                                 sWallTargetMs, noww, nb);
                            if (nb > sWallTargetMs) sWallTargetMs = nb;
                            sWallFireAdj = 0;
                        } else if (noww >= fire - WALL_WAIT_MAX_MS) {
                            // (1) 提前停：上段还在播 -> 先 stop()，把播放器“拆解”（实测 110~150ms、
                            //    两台快慢不一）移出关键路径；起播就退化成“冷启动 play”（实测 ~6ms）。
                            //    这是收敛到 1 帧的关键：v1 把 stop+play 串在临界点上，拆解耗时差
                            //    直接变成两台的相位差（实测 25~41ms 散布）。
                            if (mVideoSsPtr != NULL && mVideoSsPtr->isPlaying()) {
                                long long ps = fire - sWallStopMs - 40;      // 留 40ms 余量
                                if (ps < fire - WALL_WAIT_MAX_MS) ps = fire - WALL_WAIT_MAX_MS;
                                wallWaitUntil(ps);
                                long long ts0 = WallLink::wallNowMs();
                                mVideoSsPtr->stop();
                                long long ts1 = WallLink::wallNowMs();
                                sWallStopMs = ts1 - ts0;
                                if (sWallStopMs < 0 || sWallStopMs > 800) sWallStopMs = 120;
                                LOGD("wall: pre-stop now=%lld (target-%lldms) stop_cost=%lldms",
                                     ts1, sWallTargetMs - ts1, sWallStopMs);
                            }
                            // (2) 精等到点起播（拆解已做完 -> 此处只剩冷启动开销）
                            wallWaitUntil(fire);
                            if (sVideos.size() != 1 ||
                                sVideos[0] != (wl->playlistMode() ? wl->segPathForClip(0) : wl->segPath()))
                                loadVideoList();
                            sVideoIdx = 0;
                            long long treq = WallLink::wallNowMs();
                            playItem(0, true);
                            long long tret = WallLink::wallNowMs();
                            wl->markPlayStarted(sWallTargetMs);
                            long long d = tret - sWallTargetMs;
                            sWallFireAdj = (d > 10 || d < -10) ? -(d / 2) : 0;   // 只修系统性偏，小抖动不参与
                            if (sWallFireAdj > 30) sWallFireAdj = 30;
                            if (sWallFireAdj < -30) sWallFireAdj = -30;
                            LOGD("wall: start target=%lld t_req=%lld t_ret=%lld delta=%lldms ovh=%lldms lead=%lldms adj=%lldms",
                                 sWallTargetMs, treq, tret, d, tret - treq, lead, sWallFireAdj);
                            sWallTargetMs += wl->segMs();      // 链式推进：下一段仍落在整边界
                            sWallLateRetried = false;
                        } else if (!sWallLateRetried && mVideoSsPtr != NULL &&
                                   !mVideoSsPtr->isPlaying() &&
                                   noww > sWallTargetMs - wl->segMs() + 3000 &&
                                   noww < sWallTargetMs - 3000) {
                            // 兜底：本段应播却没在播（起播失败/被系统打断）-> 迟起播一次保画面，
                            // 下个整边界自动归位（一周期只兜一次，避免与整边界机制对打）
                            LOGW("wall: no video mid-cycle -> late restart (grid re-aligns next boundary)");
                            sWallLateRetried = true;
                            playItem(0, true);
                        }
                    }
                    if ((sMediaTicks % 5) == 0) {
                        long long noww = WallLink::wallNowMs();
                        bool playing = (mVideoSsPtr != NULL) && mVideoSsPtr->isPlaying();
                        int pos = (mVideoSsPtr != NULL) ? mVideoSsPtr->getCurrentPosition() : -1;
                        int dur = (mVideoSsPtr != NULL) ? mVideoSsPtr->getDuration() : -1;
                        // 注：getCurrentPosition() 实测是 1s 量化、且与墙钟线性相关（不是精度源），
                        //     仅作辅助记录；主判据看 wall: start 的 delta（起播调用返回 - 整边界）。
                        LOGD("wall: phase=%lldms wait=%lldms drift=%dms pos=%d dur=%dms playing=%d state=%s",
                             wl->phaseMs(), wl->waitToBoundaryMs(), wl->driftMs(),
                             pos, dur, playing ? 1 : 0, wl->stateText().c_str());
                    }
                } else {
                    if (wallEngine() == WALL_ENGINE_SIMPLE) wallSimpleStop("not-ready");
                    sWallPending = true;      // 未就绪（等 epoch/失联）-> 保持等待
                    sWallTargetMs = 0;        // 解除定标；恢复就绪后重新落到整边界
                }
            } else {
                if (wallEngine() == WALL_ENGINE_SIMPLE) wallSimpleStop("wall-off");
                sWallPending = false;
                sWallTargetMs = 0;
            }
        }
        // 空闲秒数：**只有本页真的在前台、且不在编辑态时**才累加，否则一直归零
        //   （钟工 2026-10-01：非主界面窗口不许被"关屏/进屏保"的计时器拽走；
        //     本页隐藏后心跳仍在跑，见 tickScreenOff 上方注释）
        if (sSsVisible && !ConfigStore::getInstance()->ssEditMode()) sIdleTicks++;
        else sIdleTicks = 0;
        tickScreensaverGate();                 // 非主界面窗口 -> 持续复位系统屏保计时
        // 坏素材退避倒计时（1s 心跳）；归零那一刻补一次重试（否则 MI 内核没人再叫它起播）
        if (sVideoFailTicks > 0 && --sVideoFailTicks == 0 && !sVideos.empty()) {
            playItem(sVideoIdx, true);
        }
        // 素材变化检测：上传/删除后 5s 内自动重建清单（钟工 2026-09-25）
        sListCheckTicks++;
        if (sListCheckTicks >= 5) {
            sListCheckTicks = 0;
            refreshListIfChanged(false);
        }
        tickScreenOff();                       // 关屏时段内空闲 15s -> 黑屏
        if (ConfigStore::getInstance()->ssEditMode() || sScreenOff) break;

        // ── MI 内核进度 / 轮播（屏保视频默认路径）────────────────────
        //   SimplePlayer::play() 阻塞到 EOF 才返回，WallPlayer 每返回一次 cycles 自增 ——
        //   借此判「本片播完」，**不需要知道视频时长**。
        if (ssPlayerEngine() == SS_PLAYER_MI && !sSsMiFile.empty()) {
            WallPlayer* wp = WallPlayer::getInstance();
            const int cyc = wp->cycles();
            if (cyc != sSsMiCycles) {                     // 本片结束（正常 EOF 或异常）
                const std::string err = wp->lastError();
                if (!err.empty()) {
                    sSsMiCycles = cyc;                    // 出错：由本心跳消费边界（50ms 心跳遇错会主动让路）
                    // 先按文件记账：同一文件连续失败达阈值 -> 标记不可播 + 移出轮播
                    if (noteVideoFail(sSsMiFile, std::string("MI 内核异常：") + err)) {
                        refreshListIfChanged(true);       // 重建清单（非空时内部已起播下一部）
                        if (sVideos.empty()) {            // 素材全被标记 -> 停在静态底图
                            ssMiStop("all-bad");
                            if (mImageSsBgPtr != NULL) {
                                mImageSsBgPtr->setBackgroundPic("images/ss_bg.png");
                                mImageSsBgPtr->setVisible(true);
                            }
                            LOGW("screensaver: 已无可播素材 -> 停在静态底图（素材均已标记 .bad）");
                        }
                        break;
                    }
                    if (++sSsMiErrStreak >= 3) {          // 连续失败 -> 退避回静态底图
                        sSsMiErrStreak = 0;
                        sVideoFailTicks = 30;
                        LOGW("ss[mi]: play error x3 -> back off 30s to still bg (last=%s)", err.c_str());
                        ssMiStop("mi-error");
                        if (mImageSsBgPtr != NULL) {
                            mImageSsBgPtr->setBackgroundPic("images/ss_bg.png");
                            mImageSsBgPtr->setVisible(true);
                        }
                    } else {
                        LOGW("ss[mi]: play error (%s) -> next", err.c_str());
                        playItem(sVideoIdx + 1, true);
                    }
                } else {
                    sSsMiErrStreak = 0;
                    // ★ 轮播间隔门禁（钟工 2026-10-01）：本条还没到"停留时长"就**不切** ——
                    //   WallPlayer 会继续循环本条；到点后等本轮到自然结束（下面那次完成）再切，
                    //   避免半路硬切。`连续`(iv=0) 时 ssDwellHolding() 恒为假 -> 行为不变。
                    if (ssDwellHolding()) {
                        // 按住期间**不消费 cycles 边界**：这一轮的音轨对齐留给 50ms 心跳
                        // （ssSingleLoopAudioTick）。若这里也消费，两边各抢走一半边界 ——
                        // 实测「每天」档下面板每两轮才重新对齐一次音轨 = 隔一轮静音 10 秒。
                        // 现场可观测性（2026-10-03）：每 60s 打一条"本条已停留 / 还剩多少"，
                        //   验证 12 小时档确实在按墙钟走（而不是被 onUI_show 反复清零）。
                        if (sVideos.size() > 1 && videoIntervalSec() > 0) {
                            const long long el = ssHoldElapsedSec();
                            const long long rem = (long long)videoIntervalSec() - el;
                            if (rem >= 0 && (rem % 60) == 0) {
                                LOGD("ss dwell: idx=%d 已停留 %llds / %ds（还剩 %llds，锚点=%s）",
                                     sVideoIdx, el, videoIntervalSec(), rem, sHoldFile.c_str());
                            } else if (rem < 0 && (el % 60) == 0) {
                                // 已过点但还在等本轮自然结束（见 50ms 心跳消费边界）
                                LOGD("ss dwell: idx=%d 已停留 %llds 已过 %ds 档位 -> 等本轮播完即切",
                                     sVideoIdx, el, videoIntervalSec());
                            }
                        }
                    } else {
                        sSsMiCycles = cyc;
                        if (sVideos.size() > 1) playItem(sVideoIdx + 1, true);   // 播完即切下一项
                    }
                }
            } else if (sVideos.size() > 1 && videoIntervalSec() > 0 &&
                       ssHoldElapsedSec() >= (long long)videoIntervalSec() + SS_DWELL_EOF_GRACE_SEC) {
                // 兜底（正常走不到）：间隔已到、又多等了 GRACE 秒仍没等到本轮的完成回调
                //   （播放器没报完成/卡住）-> 强切下一项，别把面板钉死在一个素材上。
                LOGW("ss[mi]: dwell=%ds 已过 +%ds 仍未收到完成回调 -> 强制切下一条",
                     videoIntervalSec(), SS_DWELL_EOF_GRACE_SEC);
                playItem(sVideoIdx + 1, true);
            }
        }

        // 播放流程自控（**EasyUI 内核专用**；MI 内核的切换在上面）
        int iv = videoIntervalSec();
        if (!sVideos.empty()) sMediaTicks++;      // 素材显示秒数（心跳计数，不依赖墙钟）
        if (sVideos.size() > 1 && sMediaTicks > 0) {
            int el = sMediaTicks;
            if (isImagePath(sVideos[sVideoIdx])) {
                int ph = ConfigStore::getInstance()->photoIntervalSec();   // 图片时长可配（钟工 2026-09-25）
                if (el >= ph) playItem(sVideoIdx + 1, true);
            } else if (ssPlayerEngine() == SS_PLAYER_EASYUI) {
                if (iv > 0) {
                    // ★ 2026-10-03：这里也改成"墙钟停留时长"（原来用 sMediaTicks 会被 onUI_show 清零，
                    //   长档位永远切不动）。sMediaTicks 仍只作为"页面在前台跑了多久"的日志量。
                    if (ssHoldElapsedSec() >= (long long)iv) playItem(sVideoIdx + 1, true);
                } else {
                    int dur = (mVideoSsPtr != NULL) ? mVideoSsPtr->getDuration() : 0;
                    if (dur > 0 && el >= dur / 1000 + 1) playItem(sVideoIdx + 1, true);
                    else if (dur <= 0 && el >= 60) playItem(sVideoIdx + 1, true);   // 取不到时长 -> 60s 兜底
                }
            }
        }
        break; }
    default:
        break;
    }
    return true;
}
// 屏保：编辑模式 = 拖动/点空白退出；否则触摸任意处唤屏 + 进主页
static bool onmainActivityTouchEvent(const MotionEvent &ev) {
    ConfigStore* cfg = ConfigStore::getInstance();

    if (cfg->ssEditMode()) {
        // ★「完成」按钮必须点得动（钟工 2026-10-01 现场："置顶了但还是点不动"）：
        //   本回调返回 true 会**吃掉**事件、控件根本收不到点击（同文件里
        //   "不再下发（避免直接触达继电器）"就是靠这条语义）。所以编辑态下自己判定按钮命中，
        //   并直接走与按钮回调**同一条**退出路径 —— 不依赖框架的派发顺序。
        const bool onDone = ssHitDoneBtn(ev.mX, ev.mY);
        int hit = ssHitTest(ev.mX, ev.mY);
        switch (ev.mActionStatus) {
        case MotionEvent::E_ACTION_DOWN:
            sEditOnBtn = onDone;
            if (onDone) {                                    // 按在「完成」上 -> 不进入拖动
                sIdleTicks = 0;
                EASYUICONTEXT->resetScreensaverTimeOut();
                return true;
            }
            sDragIdx = hit;
            sDownX = ev.mX;
            sDownY = ev.mY;
            sDragMoved = false;
            sEditDownMs = nowMs();                           // 长按空白处 = 复位（见 UP 分支）
            if (hit >= 0) {
                sDragBaseX = sGroups[hit].dx;
                sDragBaseY = sGroups[hit].dy;
            }
            sIdleTicks = 0;                                  // 编辑中触摸 = 活动
            EASYUICONTEXT->resetScreensaverTimeOut();
            return true;
        case MotionEvent::E_ACTION_MOVE:
            if (sEditOnBtn) {                                // 手指移出按钮 -> 本次"完成"作废
                if (!ssHitDoneBtn(ev.mX, ev.mY)) sEditOnBtn = false;
                sIdleTicks = 0;
                EASYUICONTEXT->resetScreensaverTimeOut();
                return true;
            }
            sIdleTicks = 0;                                  // 拖动同理，不被屏保打断
            EASYUICONTEXT->resetScreensaverTimeOut();
            if (sDragIdx >= 0) {
                // 目标 = **绝对坐标（可负）**；只做离谱值护栏，不夹取（钟工 2026-10-01：任意位置摆动）
                const int nabsX = ssGuardPos(sGroups[sDragIdx].dl + sDragBaseX + (ev.mX - sDownX));
                const int nabsY = ssGuardPos(sGroups[sDragIdx].dt + sDragBaseY + (ev.mY - sDownY));
                const int ndx = nabsX - sGroups[sDragIdx].dl;
                const int ndy = nabsY - sGroups[sDragIdx].dt;
                if (ndx != sGroups[sDragIdx].dx || ndy != sGroups[sDragIdx].dy) {
                    sGroups[sDragIdx].dx = ndx;
                    sGroups[sDragIdx].dy = ndy;
                    ssApplyGroup(sDragIdx);
                    sDragMoved = true;
                }
            }
            return true;
        case MotionEvent::E_ACTION_UP:
        case MotionEvent::E_ACTION_CANCEL:
            if (sEditOnBtn) {                                // 抬起仍在按钮内 = 点「完成」
                const bool upOn = (ev.mActionStatus == MotionEvent::E_ACTION_UP) &&
                                  ssHitDoneBtn(ev.mX, ev.mY);
                sEditOnBtn = false;
                sDragIdx = -1;
                sDragMoved = false;
                if (upOn) {
                    LOGD("ss done btn -> exit edit");
                    ssExitEditToSsset();
                }
                return true;
            }
            if (sDragIdx >= 0 && sDragMoved) {
                ssSaveGroupPos(sDragIdx);        // 松手即存 /data
                LOGD("ss drag save %s -> (%d,%d)", sGroups[sDragIdx].key,
                     sGroups[sDragIdx].dl + sGroups[sDragIdx].dx,
                     sGroups[sDragIdx].dt + sGroups[sDragIdx].dy);
            } else if (sDragIdx < 0) {
                // 空白处：短按 = 完成（退出编辑）；长按 = 全部复位到默认位置
                //   （新口径允许把浮层摆到屏外，长按复位就是"找回来"的兜底，钟工 2026-10-01）
                if (nowMs() - sEditDownMs >= SS_RESET_HOLD_MS) {
                    ssResetAllPos();
                } else {
                    ssExitEditToSsset();             // 点空白退出编辑 -> 回「屏保显示」（钟工 2026-10-01）
                }
            }
            sDragIdx = -1;
            sDragMoved = false;
            return true;
        default:
            break;
        }
        return true;
    }

    if (ev.mActionStatus == MotionEvent::E_ACTION_DOWN) {
        sLastTouchMs = nowMs();
        sIdleTicks = 0;
        EASYUICONTEXT->resetScreensaverTimeOut();
        if (sScreenOff) {
            // 熄屏状态点击 -> **立即解除熄屏并直接进主界面**（钟工 2026-10-01）：
            //   旧口径只解除熄屏、人还停在屏保页上，要再点一下才进主界面 —— 现场反馈"先回到屏保了"。
            //   熄屏本来就是"关屏时段内没人用"的态，唤醒即视为要用面板 -> 直奔主界面。
            leaveScreenOff(false);
            LOGD("screen-off wake -> homeActivity");
            EASYUICONTEXT->openActivity("homeActivity");
            return true;
        }
        // 屏保静音键（右上角喇叭）：命中就**消费本次触摸** —— 只切声音，
        // 既不进主页也不退出屏保（否则用户想静音却跳回主界面）。
        if (ssMuteHit((int)ev.mX, (int)ev.mY)) {
            sMutePressed = true;
            return true;
        }
        LOGD("screensaver touch -> homeActivity");
        EASYUICONTEXT->openActivity("homeActivity");
        return true;      // 首触只做「解除屏保 + 进主页」，不再下发（钟工报告 1：避免直接触达继电器）
    }
    if (sMutePressed &&
        (ev.mActionStatus == MotionEvent::E_ACTION_UP || ev.mActionStatus == MotionEvent::E_ACTION_CANCEL)) {
        const bool up = (ev.mActionStatus == MotionEvent::E_ACTION_UP);
        sMutePressed = false;
        sIdleTicks = 0;
        EASYUICONTEXT->resetScreensaverTimeOut();
        if (up && ssMuteHit((int)ev.mX, (int)ev.mY)) toggleSsMute();
        return true;
    }
    return false;
}

// 「完成」按钮：保存并退出编辑模式 -> 回「屏保显示」页（钟工 2026-10-01）
//   ⚠️ 编辑态下这个回调其实不会被触发（本页 onTouchEvent 会吃掉事件，见 onmainActivityTouchEvent
//      里的按钮命中分支）；保留它是为了框架派发顺序变化时仍然正确。
static bool onButtonClick_ButtonSsEditDone(ZKButton *pButton) {
    (void)pButton;
    ssExitEditToSsset();
    return true;
}
