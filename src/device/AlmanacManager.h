/*
 * AlmanacManager.h -- 屏保黄历数据唯一事实源
 *
 * 需求（钟工 2026-10-01）：屏保加一块黄历区域，显示当天的农历/干支/宜/忌。
 *
 * ── 数据源（按顺序尝试，第一个成功的胜出）───────────────────────
 *   ① https://www.36jxs.com/api/Commonweal/almanac?sun=YYYY-MM-DD      ← 客户指定源
 *   ② https://cn.apihz.cn/api/time/getday.php?id=..&key=..&day=Y-M-D  ← 备用源
 *
 *   ⚠️ ①**当前上游是坏的**（2026-10-01 实测）：它自己的文档示例日期 sun=1970-01-23 也返回
 *      {"code":0,"msg":"非法请求类型"}，GET/POST/JSON/完整浏览器头都试过；响应头
 *      x-site-cache-status: DYNAMIC + CORS 说明这是**应用层**在报错，不是 WAF。
 *      WAF（阿里云 acw_sc__v2）那一层我们**已经能过**（见 solveAcwScV2()），
 *      所以只要上游修好，本模块会自动切回 ① —— 不需要改代码。
 *   ⚠️ ①的证书链是 Let's Encrypt YR1 -> ISRG Root X1（已内嵌）；
 *      ②是 SSL.com RSA SSL subCA -> SSL.com Root CA RSA（已补进 cacert_pem.h）。
 *
 * ── 线程模型（照抄 SensorManager 的口径）────────────────────────
 *   后台线程：只做「HTTP + 解析」，结果放 pending（加锁）；不碰 prefs、不碰 UI。
 *   主线程 tick()：把 pending 提升为 current 并落盘 /data（StoragePreferences 只由主线程写）。
 *   UI 每秒读 getter；无数据时给「黄历获取中」，**绝不编造**。
 *
 * ── 刷新节奏 ─────────────────────────────────────────────────
 *   首拉延迟 15s（等 WiFi/NTP 起来）→ 之后每小时醒一次，只在「日期变了」时真发请求；
 *   失败 60s 起指数退避（上限 30min）。跨零点自动取当天黄历。
 *
 * ── 字形裁定（本工程被这个坑过，见 tools/check_glyphs.js）──────────
 *   字库是裁剪子集，字体外的字符在板子上**静默渲染成空白**。宜/忌条目是**运行期外部文案**，
 *   提交前查不了 —— 所以用 src/device/font_ranges.h（由 tools/gen_font_ranges.js 从
 *   font/HanSans-Medium.ttf 生成的字形位图）在运行期过滤：含字库没有的字的条目直接**跳过**，
 *   并把被跳过的条目打进日志（便于事后补字库），而不是画成空白。
 */
#pragma once

#include <pthread.h>
#include <string>
#include <functional>

class AlmanacManager {
public:
    // ── 一次拉取的完整结果 ─────────────────────────────────────
    struct Data {
        std::string dateKey;    // 公历 "2026-10-01"（判「是不是今天的数据」用）
        std::string lunar;      // 农历月日，如「八月廿一」
        std::string ganzhi;     // 日干支，如「戊申」
        std::string shengxiao;  // 生肖，如「猴」
        std::string jieri;      // 节日（公历+农历），如「国庆节」
        std::string jieqi;      // 节气，如「秋分」
        std::string yi;         // 宜：原始条目串（'|' 分隔）
        std::string ji;         // 忌：同上
        std::string chong;      // 冲煞，如「兔日冲(丁酉)鸡」
        std::string zhishen;    // 建除十二神，如「满」
        std::string tianshen;   // 天神 + 吉凶，如「白虎 凶」
        std::string nayin;      // 日纳音，如「金箔金」
        std::string xiu;        // 星宿，如「东方亢金龙-凶」
        std::string pengzu;     // 彭祖百忌
        std::string taishen;    // 胎神方位
        int         source = 0; // 1 = 36jxs，2 = apihz
        bool        ok = false; // 本次是否拿到有效数据
    };

    static AlmanacManager* getInstance();

    void init();                 // 读 /data 缓存（重启瞬间先显示上次的值）
    void startAuto();            // 起后台拉取线程（幂等）
    void refreshAsync();         // 立即异步拉一次（改配置/手动刷新）
    void tick();                 // 主线程每 1s：提交后台结果落盘

    // ── 展示串（任意线程可调）──────────────────────────────────
    //   已按字库过滤 + 按像素宽度截断；无数据时分别给「黄历获取中」/「」/「」
    std::string line1() const;   // 农历 + 干支 +（节气或节日）
    std::string line2() const;   // 宜 …
    std::string line3() const;   // 忌 …
    bool  hasData() const;
    int   source() const;                      // 1 = 36jxs，2 = apihz，0 = 无
    std::string raw(int field) const;          // 调试用（0=农历 1=宜 2=忌）
    long long updatedMs() const;
    std::string statusText() const;            // 日志/自证串

    // 后台线程是否已起（诊断）
    bool started() const;

private:
    AlmanacManager() = default;
    ~AlmanacManager() = default;
    AlmanacManager(const AlmanacManager&) = delete;
    AlmanacManager& operator=(const AlmanacManager&) = delete;

    bool fetchOnce(Data* out, std::string* err);   // 只在后台线程内调用
    static void* threadEntry(void* arg);

    static std::string encode(const Data& d);
    static bool decode(const std::string& blob, Data* d);
    static void sanitize(std::string* s);          // 去掉换行（prefs 里按行存）

    mutable pthread_mutex_t mMtx = PTHREAD_MUTEX_INITIALIZER;
    Data mCur;                       // 当前对外可见
    Data mPending;
    bool mHasPending = false;
    bool mHasData = false;           // 已成功取到过真实数据（落盘 sp_alm_ok）
    long long mUpdatedMs = 0;        // 上次成功的本机墙钟（仅日志，不当判据）

    // 展示串缓存（避免每秒重算 + 反复打过滤日志）
    std::string mLine1, mLine2, mLine3;
    bool mLinesDirty = true;
    void rebuildLines();             // 需持锁；内部按 mCur 生成 mLine1/2/3

    bool mThreadStarted = false;
    pthread_t mThread = 0;
    volatile bool mStop = false;
    volatile bool mWantFetch = true;
    volatile int  mFailStreak = 0;
    long long mLastTryMs = 0;        // 后台线程私有
    std::string mLastDayKey;         // 上次成功拉取的日期（换日才再拉）
};
