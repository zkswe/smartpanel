/*
 * WallPlayer.cpp -- 多屏拼接播放内核（**改用官方 simple-player 包**，v7.12 钟工 2026-09-29）
 *
 * 变更（钟工 2026-09-29 09:49：「添加这个依赖包，直接用包的代码实现视频播放」）：
 *   原来是「照抄内网 lib-simple-player 的实现、自己维护一份送流内核」（旧版 1039 行，备份见
 *   temp/WallPlayer.cpp.bak_v711_port）。现在官方授权拿到手（Manifest 里带 accessKey），
 *   改为**直接调用包**：
 *       SimplePlayer sp;
 *       sp.setSynchronizing(true);          // 包自带对齐：本地绝对钟 % (时长 + 解析耗时)
 *       sp.play(file, rect);                // 阻塞到 EOF 或 stop()
 *   解码/送显/按 PTS 控速/落后追帧/I 帧丢弃/落位，全交给包（它是现场验证过的那套）；
 *   本工程只保留：
 *     · **选片**：playlist（一组多视频轮播）模式下按组内时间（WallLink epoch + Σ时长）挑当前 clip
 *       的本机分格文件，每轮 play() 返回后重挑一次；
 *     · **组网/校时**：WallLink（epoch/单播）+ ClockManager（v7.11 自研 SNTP + 从机跟随）
 *       —— 包的格子对齐要求各机钟一致（现场老 NTP 包有 0.3s 系统偏置，v7.11 已修）。
 *
 * 与旧移植版的口径差异（可观测性）：
 *   · 旧版自测相位（pts/want/now/err/locked 每秒自证）；**包内部接管后不再有这些量**，
 *     故 lastPtsMs/lastWantMs/lastNowMs/lastErrMs 返回 -1（n/a），心跳日志里带 `pkg=1` 标记；
 *     phaseMs()/beginMs()/anchorMs() 仍按 WallLink 的组内时间轴给（用于状态页与对账）。
 *   · 循环：**不每轮重建播放器**（同尺寸时包内部复用 MI 通道/解码器），只在 play() 返回后
 *     重挑 clip 再 play()。
 * · 旋转：包按 `SIMPLE_PLAYER_CLOCKWISE_ROTATION` 环境变量取顺时针角度（不设则用
 *   ConfigManager 的 screenRotate 反推）。本工程沿用 prefs `sp_wall_rotate`（默认 0 = 不旋转）。
 *
 * ★★ 2026-10-01 关键修正：**同步模式（setSynchronizing）必须区分"拼墙"与"屏保"**
 *   包的 sync = "对基准钟播放"（反汇编 libsimple-player.a::doTask 得到）：
 *       phase  = standardTime() % (duration + parseTime) - parseTime
 *       target = standardTime() - phase
 *       每包等 standardTime() >= accum_pts + target 才 stream()；最后一个包之后再等到
 *       standardTime() >= duration + target 才从 play() 返回
 *     ⇒ play() 返回时长 = duration + parseTime - (standardTime_start mod T)，
 *       即"播到**绝对钟**的下一个整点"（T = duration + parseTime），
 *       standardTime() 取 gettimeofday 墙钟（SIMPLE_PLAYER_LOG=1 时改用 uptime）。
 *    拼墙（2~4 台同刻出画）需要它；**屏保单机循环不需要**，开了反而出三个病：
 *      ① 起播时把 phase 对应的**前若干秒画面一次性灌进解码器**（到期时刻已过去）→ 跳帧/卡帧；
 *      ② phase 大时灌流把 MI DISP 喂不上（实测起播后 5~11s `mi_disp0 RecvBufCnt` 仍为 0，
 *         `mi_vdec0` 一度整个 proc 消失 = VDEC 通道被销毁重建）→ 黑屏很久；
 *      ③ 声音走独立播放器（ss_audio 播同名 mp3）不受该相位影响、从 0 正常播 → 声音先出。
 *    现场证据（2026-10-01）：同一素材 play() 时长在 0.2s~11.0s 之间随机漂移，且"短播"会
 *    换素材（换前 森林溪瀑 0.91s → 换后 海滩浪花 1.03s）= 时间相位效应，与编码无关。
 *    ⇒ 口径：**拼墙（WallLink enabled）开；屏保关**。prefs `sp_ss_sync` 可覆盖（-1=自动 0=关 1=开）。
 */
#include "wall/WallPlayer.h"

#include "storage/StoragePreferences.h"
#include "utils/Log.h"
#include "wall/WallLink.h"

#include "simple_player.h"          // 官方包 simple-player@4.0.1 的头
#include <base/rectangle.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "WallPlayer"

struct WallPlayer::Impl {
    std::thread th;
    std::atomic<bool> alive{false};
    std::atomic<bool> stopReq{false};
    std::atomic<bool> playing{false};      // 当前有 play() 在跑（= 锁相由包接管中）
    std::atomic<int> cycles{0};            // 已完成的 play() 轮数
    std::atomic<int> clipIdx{0};
    std::atomic<int> clipCnt{1};
    std::atomic<int> x{0}, y{0}, w{0}, h{0};

    std::mutex mu;
    std::string file;                      // 初始节目键（换键 -> 重开）
    std::string cur;                       // 当前实际在播文件
    std::string err;

    std::mutex spMu;
    SimplePlayer* sp = NULL;                // 线程内栈对象，供 stop() 从外部打断
};

WallPlayer* WallPlayer::getInstance() {
    static WallPlayer s;
    return &s;
}

// ── 音画同步握手（见 WallPlayer.h）────────────────────────────────────
//   ⚠️ 必须用 **4 字节**原子：32 位 ARM 上 `std::atomic<long long>` 会引 `__atomic_load_8`
//      等 libatomic 符号，本工程没链 libatomic -> `undefined reference to __atomic_store_8`。
//      开机毫秒数 49 天才回绕一次，存 32 位足够（回绕成 0 的概率可忽略）。
static std::atomic<unsigned int> sFirstFeedMs{0};

long long WallPlayer::monoMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)(ts.tv_nsec / 1000000);
}

void WallPlayer::resetFirstFeed() {
    sFirstFeedMs.store(0);
}

void WallPlayer::noteFirstFeed() {
    unsigned int expect = 0;
    sFirstFeedMs.compare_exchange_strong(expect, (unsigned int)monoMs());   // 只记第一次
}

long long WallPlayer::firstFeedMs() {
    return (long long)sFirstFeedMs.load();
}

WallPlayer::WallPlayer() {
    impl_ = new Impl();
}

WallPlayer::~WallPlayer() {
    stop();
    delete impl_;
}

bool WallPlayer::running() {
    return impl_->alive.load();
}

std::string WallPlayer::currentFile() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->cur;
}

// ── 组内时间轴（诊断用；播放相位由包自己算）──────────────────────────────
static long long groupElapsed(long long* period) {
    WallLink* wl = WallLink::getInstance();
    const long long p = wl->playlistMode() ? wl->totalMs() : wl->segMs();
    if (period != NULL) *period = p;
    if (!wl->enabled() || !wl->hasEpoch() || p <= 0) return -1;
    const long long n = WallLink::wallNowMs();
    if (n <= 0) return -1;
    long long g = (n - wl->epochMs()) % p;
    if (g < 0) g += p;
    return g;
}

long long WallPlayer::periodMs() {
    long long p = 0;
    groupElapsed(&p);
    return p;
}

long long WallPlayer::beginMs() {
    return groupElapsed(NULL);
}

long long WallPlayer::anchorMs() {
    long long p = 0;
    const long long g = groupElapsed(&p);
    if (g < 0) return -1;
    return WallLink::wallNowMs() - g;
}

// 以下四个量在「包接管」后不再由本工程测量 -> -1（n/a），不要当 0 用
long long WallPlayer::lastPtsMs()  { return -1; }
long long WallPlayer::lastWantMs() { return -1; }
long long WallPlayer::lastNowMs()  { return -1; }
long long WallPlayer::lastErrMs()  { return -1; }
int WallPlayer::secs()             { return 0; }
int WallPlayer::catchupFrames()    { return 0; }

int WallPlayer::cycles() {
    return impl_->cycles.load();
}

bool WallPlayer::locked() {
    // 包在跑 play() 就代表"格子对齐已接管"；不再有旧版的追赶/锁相两态
    return impl_->playing.load() && impl_->err.empty();
}

int WallPlayer::clipIndex() { return impl_->clipIdx.load(); }
int WallPlayer::clipCount() { return impl_->clipCnt.load(); }

std::string WallPlayer::clipFile() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->cur;
}

std::string WallPlayer::lastError() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->err;
}

static bool wantSynchronizing();      // 定义在下方 run() 之前（同步模式口径，见文件头）

bool WallPlayer::start(const std::string& file, int x, int y, int w, int h) {
    Impl* im = impl_;
    if (file.empty()) return false;
    if (im->alive.load()) {
        bool same;
        {
            std::lock_guard<std::mutex> lk(im->mu);
            same = (im->file == file) && im->w.load() == w && im->h.load() == h;
        }
        if (same) return true;                 // 已在播同一个节目 -> 不动（避免反复拆建 MI 图层）
        stop();
    }
    // 旋转口径：prefs sp_wall_rotate（0 = 用包默认：按 ConfigManager screenRotate 反推）
    const int rot = StoragePreferences::getInt("sp_wall_rotate", 0);
    if (rot != 0) {
        char b[16];
        snprintf(b, sizeof(b), "%d", rot);
        setenv("SIMPLE_PLAYER_CLOCKWISE_ROTATION", b, 1);
        LOGI("wall[sp-pkg]: SIMPLE_PLAYER_CLOCKWISE_ROTATION=%d（prefs sp_wall_rotate）", rot);
    }
    {
        std::lock_guard<std::mutex> lk(im->mu);
        im->file = file;
        im->err.clear();
        im->cur.clear();
    }
    im->x = x; im->y = y; im->w = w; im->h = h;
    im->cycles = 0;
    im->stopReq = false;
    resetFirstFeed();                       // 音画同步：本次起播的"第一包"时刻重新计
    im->alive = true;
    im->th = std::thread(&WallPlayer::run, this, file, x, y, w, h);
    LOGI("wall[sp-pkg]: start %s rect=(%d,%d,%d,%d) engine=simple-player(pkg) sync=%d",
         file.c_str(), x, y, w, h, wantSynchronizing() ? 1 : 0);
    return true;
}

void WallPlayer::stop() {
    Impl* im = impl_;
    if (!im->alive.load()) return;
    im->stopReq = true;
    if (im->playing.load()) {                  // 打断正在跑的 play()
        SimplePlayer* p = NULL;
        {
            std::lock_guard<std::mutex> lk(im->spMu);
            p = im->sp;
        }
        if (p != NULL) p->stop();
    }
    if (im->th.joinable()) im->th.join();
    im->alive = false;
    im->playing = false;
    LOGI("wall[sp-pkg]: stopped (cycles=%d)", im->cycles.load());
}

// 同步模式口径（见文件头"★★ 关键修正"）：
//   拼墙 = 需要"绝对钟网格"对齐（多台同刻出画）-> 开；
//   屏保 = 单机循环，不需要 -> 关（开了会被相位劫持：跳帧 + 黑屏 + 声音先出）。
//   prefs `sp_ss_sync`：-1 = 自动（默认，按 WallLink）  0 = 强制关  1 = 强制开。
static bool wantSynchronizing() {
    const int ov = StoragePreferences::getInt("sp_ss_sync", -1);
    if (ov == 0) return false;
    if (ov == 1) return true;
    return WallLink::getInstance()->enabled();     // 自动：只有拼接场景才对齐基准钟
}

void WallPlayer::run(std::string file, int x, int y, int w, int h) {
    Impl* im = impl_;
    SimplePlayer sp;                           // 一次创建，多轮复用（包内部按尺寸复用 MI 通道）
    const bool syncOn = wantSynchronizing();
    sp.setSynchronizing(syncOn);
    LOGI("wall[sp-pkg]: sync=%d (%s)", syncOn ? 1 : 0,
         syncOn ? "拼墙：对齐绝对钟网格" : "屏保：按 PTS 正常起播（不被相位劫持）");
    {
        std::lock_guard<std::mutex> lk(im->spMu);
        im->sp = &sp;
    }
    const base::Rectangle rect(x, y, w, h);
    std::string lastLocalPick;                  // 只为日志去重（没 epoch 时的本地选段）

    while (!im->stopReq.load()) {
        std::string f = file;
        WallLink* wl = WallLink::getInstance();
        // 多视频轮播：按**本机 NTP 钟 + 固定绝对锚点**挑"当前 clip 的本机分格"。
        //   v7.13（钟工 2026-09-29「方案B」）：**不再用主机 epoch**——各屏都有自己的 NTP 时间，
        //   锚点是个常数（prefs sp_wall_anchor_ms，默认 0）-> 同一时刻算出同一 clip，
        //   播放相位与"此刻该放哪段"都**零网络**；断网/拥塞不影响对齐。
        if (wl->playlistMode()) {
            const long long total = wl->totalMs();
            const long long n = WallLink::wallNowMs();          // 本机墙钟（NTP 校过）
            const long long anchor = (long long)StoragePreferences::getInt("sp_wall_anchor_ms", 0);
            if (total > 0 && n > 0) {
                long long g = (n - anchor) % total;
                if (g < 0) g += total;
                int k = 0;
                long long off = 0;
                if (wl->clipAt(g, &k, &off)) {
                    const std::string nf = wl->segPathForClip(k);
                    if (!nf.empty()) {
                        if (nf != f) LOGI("wall[sp-pkg]: clip -> %d/%d off=%lldms %s (ntp grid)",
                                          k, wl->clipCount(), off, nf.c_str());
                        f = nf;
                        im->clipIdx = k;
                        im->clipCnt = wl->clipCount();
                    }
                }
            } else {
                const std::string sel = StoragePreferences::getString("sp_video_sel", "");
                f = sel.empty() ? wl->segPathForClip(0) : sel;
                if (f != lastLocalPick) {
                    lastLocalPick = f;
                    LOGI("wall[sp-pkg]: clock not ready -> local pick %s", f.c_str());
                }
                im->clipIdx = 0;
                im->clipCnt = wl->clipCount();
            }
        }
        // 双保险：任何情况下都不把清单/配置文件当视频交给播放器
        if (f.size() > 5 && f.compare(f.size() - 5, 5, ".json") == 0) {
            LOGW("wall[sp-pkg]: %s 不是视频，回退清单第 0 段", f.c_str());
            f = wl->segPathForClip(0);
        }
        {
            std::lock_guard<std::mutex> lk(im->mu);
            im->cur = f;
        }
        im->playing = true;
        LOGI("wall[sp-pkg]: play %s (cycle=%d, clip=%d/%d)", f.c_str(),
             im->cycles.load() + 1, im->clipIdx.load(), im->clipCnt.load());
        try {
            sp.play(f, rect);                  // 阻塞：读到 EOF 或 stop()
            // 本片正常播完 -> 清掉上一轮遗留的错误。不清的话 lastError() 会一直非空，
            // 上层"按错误标记坏素材"的逻辑就会把后面能播的片子也一起判掉（2026-10-01 修）。
            {
                std::lock_guard<std::mutex> lk(im->mu);
                im->err.clear();
            }
        } catch (std::exception& e) {
            {
                std::lock_guard<std::mutex> lk(im->mu);
                im->err = e.what();
            }
            LOGW("wall[sp-pkg]: play error -> %s（1s 后重试）", e.what());
            usleep(1000 * 1000);
        } catch (...) {
            {
                std::lock_guard<std::mutex> lk(im->mu);
                im->err = "unknown exception";
            }
            LOGW("wall[sp-pkg]: play unknown error（1s 后重试）");
            usleep(1000 * 1000);
        }
        im->playing = false;
        // 音画同步（2026-10-01）：**先清 firstFeed 再自增 cycles** —— 上层一旦观察到 cycles 变化，
        // 必定读到 firstFeedMs()==0（= 新一轮还没开始喂流），于是挂起音频等这一轮的第一包；
        // 反过来先自增会让上层读到上一轮的旧值 -> 音频提前起播（错位 ~200ms）。
        resetFirstFeed();
        im->cycles.fetch_add(1);
    }
    {
        std::lock_guard<std::mutex> lk(im->spMu);
        im->sp = NULL;
    }
    LOGI("wall[sp-pkg]: player thread exit (cycles=%d)", im->cycles.load());
}
