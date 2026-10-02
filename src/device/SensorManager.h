/*
 * SensorManager.h
 *
 * 环境数据（温度 / 湿度 / 天气 / 空气质量）唯一事实源。
 *
 * ── 2026-10-01 重做（原实现是"假的"）─────────────────────────────
 *   改前真相：
 *     1) 屏保上的「晴 26」「26.5°C / 58 %」和主页上的「晴 26°C」「26.5°C」
 *        都是 ui/*.json 里**写死的静态文案**，对应的 Text 控件在 C++ 里
 *        连一次 setText 都没有（grep 零命中）—— 那就不是"演示数据"，是贴图。
 *     2) demoTick() 的随机游走是唯一的"数据源"，但它**全工程没有调用点**。
 *     3) fetchWeather() 是空壳，只有一行注释 + 一行日志。
 *   改后：接 **Open-Meteo**（免费、无需申请 Key、HTTPS）拉真实数据：
 *     forecast    -> temperature_2m / relative_humidity_2m / weather_code
 *     air-quality -> pm2_5 / pm10 / us_aqi
 *   同时**删掉随机游走的演示值**：没有真实数据时显示「获取中」，
 *   绝不编造数字（编造的数字会让人以为功能是好的，比空着更糟）。
 *
 * ── 数据来源优先级 ────────────────────────────────────────────
 *   1. 外部注入：UDP API / MQTT / 串口传感器 -> setTempHum() / setWeather()
 *   2. Open-Meteo 后台定时拉取（默认开启，15 分钟一次）
 *   3. 无 -> 显示「获取中」（hasData() == false）
 *
 * ── 线程模型 ──────────────────────────────────────────────────
 *   后台线程：只做「HTTP + 解析」，把结果放进 pending（加锁），不碰 prefs、不碰 UI。
 *   主线程：tick() 把 pending 提升为 current 并落盘 /data（StoragePreferences 只由主线程写）；
 *          UI 侧每秒读 getter（加锁拷贝），值变了才 setText。
 */
#pragma once

#include <functional>
#include <pthread.h>
#include <string>

class SensorManager {
public:
    // ── 一次拉取的完整结果 ─────────────────────────────────────
    struct EnvData {
        float     temp = 0.0f;      // °C
        float     hum  = 0.0f;      // %
        int       wxCoded = -1;     // WMO 天气代码；-1 = 未知
        float     pm25 = -1.0f;     // μg/m³；<0 = 无数据
        float     pm10 = -1.0f;     // μg/m³；<0 = 无数据
        int       aqi  = -1;        // US AQI；<0 = 无数据
        long long updatedMs = 0;    // 拉取成功时的本机墙钟
    };

    static SensorManager* getInstance();

    void init();
    // 起后台拉取线程（幂等）。首拉延迟 6s（等 WiFi/NTP 起来），成功后每 15 分钟一次；
    // 失败 60s 后重试，连续失败逐步退避（最长 15 分钟）。
    void startAuto();

    // ── 读值（任意线程可调；内部加锁，返回拷贝）────────────────
    float temp() const;                          // 温度 °C
    float hum()  const;                          // 相对湿度 %
    std::string weather() const;                 // 天气文案，如「小雨」；无数据 =「获取中」
    int   wxCode() const;                        // WMO 天气代码；-1 = 未知
    float pm25() const;                          // PM2.5 μg/m³；<0 = 无数据
    float pm10() const;                          // PM10 μg/m³；<0 = 无数据
    int   aqi() const;                           // US AQI；<0 = 无数据
    std::string aqiLevel() const;                // 空气质量等级（中国 HJ 633 口径）
    long long updatedMs() const;                 // 上次成功拉取的本机墙钟（epoch ms）；0 = 未知
    bool  hasData() const;                       // 是否已有真实数据（UI 据此显示数值或「获取中」）

    // ── 外部注入（UDP API / MQTT / 串口）；注入后 hasData() 变 true ──
    void setTempHum(float temp, float hum);
    void setWeather(const std::string& text);

    // ── 位置配置 ───────────────────────────────────────────────
    std::string city() const;                    // 当前城市名（默认「深圳」）
    // 按城市名解析经纬度并保存（走 geocoding API）；返回是否解析成功
    bool setLocationByCity(const std::string& city);
    // 直接指定经纬度（"纬度,经度"），顺带记一个显示名
    void setLocation(const std::string& city, const std::string& latlon);
    // 立即异步拉一次（改完位置 / 手动刷新用）
    void refreshAsync();

    void setListener(const std::function<void()>& listener);

    // 主线程每 1s 调用：提交后台结果落盘 + 失败退避判定
    void tick();

    // WMO 天气代码 -> 中文文案（屏保图标复用同一套关键字配色）
    static const char* wmoToText(int code);
    // PM2.5(μg/m³) -> 空气质量等级文案（中国 HJ 633-2012 24h 均值口径）
    static const char* pm25ToLevel(float pm25);

    // 调试 / 自证串（日志、版本页用）
    std::string statusText() const;

private:
    SensorManager() = default;
    ~SensorManager() = default;
    SensorManager(const SensorManager&) = delete;
    SensorManager& operator=(const SensorManager&) = delete;

    // 一次完整拉取（HTTP + 解析）；成功返回 true 并填 out。只在后台线程内调用。
    bool fetchOnce(EnvData* out, std::string* err);
    static void* threadEntry(void* arg);

    mutable pthread_mutex_t mMtx = PTHREAD_MUTEX_INITIALIZER;
    EnvData   mCur;                   // 当前对外可见
    EnvData   mPending;               // 后台刚拉到，等主线程落盘
    bool      mHasPending = false;
    std::string mWxText = "获取中";    // 天气文案（外部注入的文本优先）
    std::string mLevelText;            // 空气质量等级
    std::string mCity = "深圳";
    std::string mLatLon = "22.5431,114.0579";
    bool      mInjTh = false;          // 温湿度来自外部注入（拉取结果不覆盖这两项）
    bool      mInjWx = false;          // 天气文案来自外部注入（拉取结果不覆盖文案，仍更新图标代码）
    bool      mHasData = false;        // 已成功取到过真实数据（落盘到 sp_wx_ok）

    bool      mThreadStarted = false;
    pthread_t mThread = 0;
    volatile bool mStop = false;
    volatile bool mWantFetch = true;
    volatile int  mFailStreak = 0;
    long long mLastTryMs = 0;                 // 后台线程私有，无需加锁

    std::function<void()> mListener;
};
