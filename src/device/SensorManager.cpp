/*
 * SensorManager.cpp -- 环境数据（真实数据源：Open-Meteo）
 *
 * 数据源（免费、无需 API Key、HTTPS）：
 *   https://api.open-meteo.com/v1/forecast?latitude=&longitude=
 *        &current=temperature_2m,relative_humidity_2m,weather_code&timezone=auto
 *   https://air-quality-api.open-meteo.com/v1/air-quality?latitude=&longitude=
 *        &current=pm2_5,pm10,us_aqi&timezone=auto
 *   https://geocoding-api.open-meteo.com/v1/search?name=<城市>&count=1&language=zh&format=json
 *
 * 证书：内嵌 7 张 CA 根（src/device/cacert_pem.h，由 tools/gen_cacert_roots.js 生成）。
 *   实测 api/air-quality/geocoding 三个主机的链末端都是 Let's Encrypt 的
 *   ISRG Root X1（RSA 链）或 X2（ECDSA 链），所以不必把 189KB 的 Mozilla 全量包塞进 .so。
 *   运行时优先用磁盘上的 cacert.pem（/tmp/ui、/mnt/extsd/ui、/res/ui 三种 resPath 形态），
 *   找不到才把内嵌的落到 /tmp/sp_cacert.pem 给 libcurl 读。
 *
 * 线程：拉取全在后台线程（本文件 threadEntry），结果只放 pending（加锁）；
 *   落盘 /data 由主线程 tick() 做 —— StoragePreferences 全工程只被主线程写过，
 *   这里不去破坏这个前提。
 */
#include "device/SensorManager.h"

#include "device/cacert_pem.h"      // 内嵌 CA（自动生成）

#include "storage/ConfigStore.h"
#include "utils/Log.h"

#include <curl/curl.h>
#include <rapidjson/document.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>          // clock_gettime（时间源，见 smNowMs）
#include <unistd.h>

// ── 常量 ──────────────────────────────────────────────────────
static const char* kForecastUrl = "https://api.open-meteo.com/v1/forecast";
static const char* kAirUrl      = "https://air-quality-api.open-meteo.com/v1/air-quality";
static const char* kGeoUrl      = "https://geocoding-api.open-meteo.com/v1/search";
static const char* kCaTmpPath   = "/tmp/sp_cacert.pem";
static const int   kFirstDelayS = 6;                       // 首拉延迟：等 WiFi + NTP 就绪
static const int   kOkPeriodMs  = 15 * 60 * 1000;          // 成功后 15 分钟一次
static const int   kFailBaseMs  = 60 * 1000;               // 失败 60s 起退避
static const int   kFailMaxMs   = kOkPeriodMs;             // 退避上限
static const long  kConnectMs   = 6000;
static const long  kTotalMs     = 15000;

// 磁盘上的 cacert.pem 候选（三种 resPath 形态；见 MEMORY「固化/SD/调试」三套部署）
static const char* kCaCandidates[] = {
    "/tmp/ui/cacert.pem",
    "/mnt/extsd/ui/cacert.pem",
    "/res/ui/cacert.pem",
};

// ── 本机墙钟（epoch 毫秒）─────────────────────────────────────
// ⚠️ 不能用 TimeHelper::getCurrentTime()：本平台它返回的是**被截断成 32 位的 epoch 毫秒**
//    （实测在真机上直接是负值，约 -1.55e8），拿它当时间戳会让"是否已取到数据"永远为假。
//    ClockManager 里早有同款踩坑（见其 cmNowMs 注释），这里照同一套写法取 CLOCK_REALTIME。
static long long smNowMs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        const long long ms = (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
        if (ms > 1000000000000LL) return ms;                 // 已校时到 2001 之后
    }
    return 0;                                                // 未校时 -> 0（视作"时间未知"）
}

SensorManager* SensorManager::getInstance() {
    static SensorManager s;
    return &s;
}

// ── 小工具 ────────────────────────────────────────────────────
static std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 0x0F];
        }
    }
    return o;
}

static bool parseLatLon(const std::string& s, double* lat, double* lon) {
    const size_t c = s.find(',');
    if (c == std::string::npos) return false;
    *lat = atof(s.substr(0, c).c_str());
    *lon = atof(s.substr(c + 1).c_str());
    if (*lat < -90.0 || *lat > 90.0 || *lon < -180.0 || *lon > 180.0) return false;
    if (fabs(*lat) < 1e-6 && fabs(*lon) < 1e-6) return false;      // 0,0 视为未配置
    return true;
}

static bool fileUsable(const char* p) {
    struct stat st;
    if (stat(p, &st) != 0) return false;
    return st.st_size > 512;                                       // 小于 0.5KB 的当坏文件
}

// 解析出可用的 CA 文件；找不到磁盘上的就把内嵌的写到 /tmp
// 注：加锁 —— 后台拉取线程和 PanelLink（UDP 线程，改城市时）都会走到这里。
static std::string resolveCaFile() {
    static std::string cached;
    static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mtx);
    if (!cached.empty()) { const std::string r = cached; pthread_mutex_unlock(&mtx); return r; }
    pthread_mutex_unlock(&mtx);

    std::string found;
    for (size_t i = 0; i < sizeof(kCaCandidates) / sizeof(kCaCandidates[0]); i++) {
        if (fileUsable(kCaCandidates[i])) { found = kCaCandidates[i]; break; }
    }
    if (found.empty()) {
        FILE* fp = fopen(kCaTmpPath, "wb");
        if (fp != NULL) {
            const size_t n = sizeof(kCACertPem) - 1;               // 去掉结尾 '\0'
            const size_t w = fwrite(kCACertPem, 1, n, fp);
            fclose(fp);
            if (w == n && fileUsable(kCaTmpPath)) {
                found = kCaTmpPath;
                LOGI("SensorManager: ca file = %s (embedded %u bytes)", found.c_str(), (unsigned)n);
            } else {
                LOGW("SensorManager: write embedded ca failed (w=%u/%u errno=%d)",
                     (unsigned)w, (unsigned)n, errno);
            }
        } else {
            LOGW("SensorManager: open %s failed errno=%d", kCaTmpPath, errno);
        }
    } else {
        LOGI("SensorManager: ca file = %s (on disk)", found.c_str());
    }
    if (found.empty()) {
        LOGW("SensorManager: no CA available -> HTTPS verify DISABLED (insecure fallback)");
    }

    pthread_mutex_lock(&mtx);
    cached = found;
    pthread_mutex_unlock(&mtx);
    return found;
}

// ── HTTP GET（libcurl）────────────────────────────────────────
static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static bool httpGet(const std::string& url, std::string& body, std::string& err) {
    CURL* h = curl_easy_init();
    if (h == NULL) { err = "curl_easy_init failed"; return false; }

    const std::string ca = resolveCaFile();
    body.clear();
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, kConnectMs);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, kTotalMs);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);          // 多线程下必须：禁用信号式超时
    curl_easy_setopt(h, CURLOPT_USERAGENT, "SmartPanel/1.0");
    if (!ca.empty()) {
        curl_easy_setopt(h, CURLOPT_CAINFO, ca.c_str());
    } else {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    const CURLcode rc = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK) {
        char b[160];
        snprintf(b, sizeof(b), "curl %d: %s", (int)rc, curl_easy_strerror(rc));
        err = b;
        return false;
    }
    if (code != 200) {
        char b[64];
        snprintf(b, sizeof(b), "http %ld (%u bytes)", code, (unsigned)body.size());
        err = b;
        return false;
    }
    if (body.empty()) { err = "empty body"; return false; }
    return true;
}

// ── WMO 天气代码 -> 中文文案 ──────────────────────────────────
// 注：文案用词的每个字都过了 tools/check_glyphs.js（裁剪字库，缺字会静默变空白）。
//     尤其不能用 ℃ ° · μ ³ —— 主字库 zkswe-hans-common.ttf 里**没有**这几个字形。
const char* SensorManager::wmoToText(int code) {
    switch (code) {
    case 0:  return "晴";
    case 1:  return "晴";
    case 2:  return "多云";
    case 3:  return "阴";
    case 45: return "雾";
    case 48: return "雾";
    case 51: return "毛毛雨";
    case 53: return "小雨";
    case 55: return "小雨";
    case 56: return "冻雨";
    case 57: return "冻雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: return "冻雨";
    case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "米雪";
    case 80: return "阵雨";
    case 81: return "阵雨";
    case 82: return "暴雨";
    case 85: return "阵雪";
    case 86: return "阵雪";
    case 95: return "雷阵雨";
    case 96: return "雷阵雨";
    case 99: return "雷阵雨";
    default: return "未知";
    }
}

// ── PM2.5 -> 空气质量等级（中国 HJ 633-2012，24h 均值浓度限值）──
const char* SensorManager::pm25ToLevel(float pm25) {
    if (pm25 < 0.0f) return "";
    if (pm25 <= 35.0f)  return "优";
    if (pm25 <= 75.0f)  return "良";
    if (pm25 <= 115.0f) return "轻度污染";
    if (pm25 <= 150.0f) return "中度污染";
    if (pm25 <= 250.0f) return "重度污染";
    return "严重污染";
}

// ── 生命周期 ──────────────────────────────────────────────────
void SensorManager::init() {
    // curl 全局初始化必须在任何 curl 调用之前、且由一个线程先做 —— 这里在主线程做。
    const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) LOGW("SensorManager: curl_global_init rc=%d", (int)rc);

    ConfigStore* cfg = ConfigStore::getInstance();
    pthread_mutex_lock(&mMtx);
    // 先显示上次缓存（重启瞬间不空白；这些是**上次真实拉到的值**）
    mCur.temp = cfg->cachedTemp();
    mCur.hum  = cfg->cachedHum();
    mCur.wxCoded = cfg->cachedWxCode();
    mCur.pm25 = cfg->cachedPm25();
    mCur.pm10 = cfg->cachedPm10();
    mCur.aqi  = cfg->cachedAqi();
    mCur.updatedMs = cfg->weatherUpdatedMs();
    mWxText = cfg->cachedWeather();
    mLevelText = pm25ToLevel(mCur.pm25);
    mCity = cfg->weatherCity();
    mLatLon = cfg->weatherLatLon();
    mHasData = cfg->weatherHasData();
    pthread_mutex_unlock(&mMtx);

    LOGI("SensorManager: init city=%s latlon=%s temp=%.1f hum=%.1f wx=%s pm25=%.1f aqi=%d has=%d",
         mCity.c_str(), mLatLon.c_str(), mCur.temp, mCur.hum, mWxText.c_str(),
         mCur.pm25, mCur.aqi, mHasData ? 1 : 0);
}

void SensorManager::startAuto() {
    pthread_mutex_lock(&mMtx);
    const bool already = mThreadStarted;
    pthread_mutex_unlock(&mMtx);
    if (already) return;
    mStop = false;
    mWantFetch = true;
    if (pthread_create(&mThread, NULL, &SensorManager::threadEntry, this) != 0) {
        LOGW("SensorManager: fetch thread create failed (will retry on next call)");
        return;
    }
    pthread_mutex_lock(&mMtx);
    mThreadStarted = true;
    pthread_mutex_unlock(&mMtx);
    LOGI("SensorManager: auto fetch started (first in %ds, then %dmin)", kFirstDelayS, kOkPeriodMs / 60000);
}

void SensorManager::refreshAsync() {
    mWantFetch = true;
    // 线程还没起过（例如 init 阶段就刷新）-> 直接拉起
    if (!mThreadStarted) startAuto();
}

void* SensorManager::threadEntry(void* arg) {
    SensorManager* self = static_cast<SensorManager*>(arg);

    for (int i = 0; i < kFirstDelayS && !self->mStop; i++) sleep(1);

    while (!self->mStop) {
        const long long now = smNowMs();
        // 退避：60s * 2^failStreak，上限 15min
        int iv = kFailBaseMs;
        const int fs = self->mFailStreak;
        for (int i = 0; i < fs && iv < kFailMaxMs; i++) iv *= 2;
        if (iv > kFailMaxMs) iv = kFailMaxMs;

        const bool want = self->mWantFetch;
        // now 在校时前是 0 -> 用 mLastTryMs != 0 判断，避免"永远该拉"每秒打一次 HTTP
        const bool due = (now == 0) ? (self->mLastTryMs != 0)
                                    : (self->mLastTryMs != 0 && (now - self->mLastTryMs) >= iv);
        if (want || due) {
            self->mWantFetch = false;
            self->mLastTryMs = (now == 0) ? 1 : now;

            EnvData d;
            std::string err;
            if (self->fetchOnce(&d, &err)) {
                self->mFailStreak = 0;
                pthread_mutex_lock(&self->mMtx);
                self->mPending = d;
                self->mHasPending = true;
                pthread_mutex_unlock(&self->mMtx);
                LOGI("SensorManager: fetch ok t=%.1fC h=%.0f%% wx=%d(%s) pm25=%.1f pm10=%.1f aqi=%d",
                     d.temp, d.hum, d.wxCoded, wmoToText(d.wxCoded), d.pm25, d.pm10, d.aqi);
            } else {
                self->mFailStreak++;
                LOGW("SensorManager: fetch failed (streak=%d, next in %dms): %s",
                     self->mFailStreak, iv, err.c_str());
            }
        }
        sleep(1);
    }
    LOGI("SensorManager: fetch thread exit");
    return NULL;
}

// ── 一次完整拉取 ──────────────────────────────────────────────
bool SensorManager::fetchOnce(EnvData* out, std::string* err) {
    double lat = 0, lon = 0;
    std::string latlon;
    pthread_mutex_lock(&mMtx);
    latlon = mLatLon;
    pthread_mutex_unlock(&mMtx);
    if (!parseLatLon(latlon, &lat, &lon)) {
        *err = "bad latlon: " + latlon;
        return false;
    }

    char u[512];
    std::string body;

    // (1) 天气 + 温湿度
    snprintf(u, sizeof(u),
             "%s?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code&timezone=auto",
             kForecastUrl, lat, lon);
    if (!httpGet(u, body, *err)) return false;

    {
        rapidjson::Document d;
        d.Parse(body.c_str());
        if (d.HasParseError() || !d.IsObject()) { *err = "forecast json parse error"; return false; }
        if (!d.HasMember("current") || !d["current"].IsObject()) { *err = "forecast: no current"; return false; }
        const rapidjson::Value& c = d["current"];
        bool got = false;
        if (c.HasMember("temperature_2m") && c["temperature_2m"].IsNumber()) {
            out->temp = (float)c["temperature_2m"].GetDouble();
            got = true;
        }
        if (c.HasMember("relative_humidity_2m") && c["relative_humidity_2m"].IsNumber()) {
            out->hum = (float)c["relative_humidity_2m"].GetDouble();
        }
        if (c.HasMember("weather_code") && c["weather_code"].IsNumber()) {
            out->wxCoded = (int)c["weather_code"].GetDouble();
        }
        if (!got) { *err = "forecast: no temperature"; return false; }
    }

    // (2) 空气质量（PM2.5 / PM10 / AQI）
    //     单列一个请求：air-quality 是独立主机，且更新频率（1h）与天气（15min）不同；
    //     它失败**不**让整次拉取失败（温湿度/天气仍然有效）。
    snprintf(u, sizeof(u),
             "%s?latitude=%.4f&longitude=%.4f&current=pm2_5,pm10,us_aqi&timezone=auto",
             kAirUrl, lat, lon);
    std::string aerr;
    if (httpGet(u, body, aerr)) {
        rapidjson::Document d;
        d.Parse(body.c_str());
        if (!d.HasParseError() && d.IsObject() && d.HasMember("current") && d["current"].IsObject()) {
            const rapidjson::Value& c = d["current"];
            if (c.HasMember("pm2_5") && c["pm2_5"].IsNumber())  out->pm25 = (float)c["pm2_5"].GetDouble();
            if (c.HasMember("pm10") && c["pm10"].IsNumber())    out->pm10 = (float)c["pm10"].GetDouble();
            if (c.HasMember("us_aqi") && c["us_aqi"].IsNumber()) out->aqi = (int)c["us_aqi"].GetDouble();
        } else {
            LOGW("SensorManager: air json parse error");
        }
    } else {
        LOGW("SensorManager: air-quality failed: %s", aerr.c_str());
    }

    out->updatedMs = smNowMs();
    return true;
}

// ── 主线程每秒调用：落盘 + 通知 ────────────────────────────────
void SensorManager::tick() {
    EnvData d;
    bool has = false;
    pthread_mutex_lock(&mMtx);
    if (mHasPending) {
        d = mPending;
        mHasPending = false;
        mCur = d;
        has = true;
    }
    const bool injTh = mInjTh;
    const bool injWx = mInjWx;
    pthread_mutex_unlock(&mMtx);
    if (!has) return;

    ConfigStore* cfg = ConfigStore::getInstance();
    // 落盘（StoragePreferences 只由主线程写）
    if (!injTh) cfg->setCachedTempHum(d.temp, d.hum);
    cfg->setCachedWxCode(d.wxCoded);
    if (d.pm25 >= 0.0f) cfg->setCachedAir(d.pm25, d.pm10, d.aqi);
    cfg->setWeatherUpdatedMs(d.updatedMs);

    pthread_mutex_lock(&mMtx);
    // 天气文案也落盘（否则重启后 init() 会把上次的真实文案读成默认「未获取」——
    // 会出现 has_data=true 但天气文案是「未获取」的自相矛盾状态）
    if (!injWx) mWxText = wmoToText(d.wxCoded);
    mLevelText = pm25ToLevel(d.pm25);
    mHasData = true;
    const std::string wxText = mWxText;
    pthread_mutex_unlock(&mMtx);
    if (!injWx) cfg->setCachedWeather(wxText);
    cfg->setWeatherHasData(true);

    LOGI("SensorManager: committed t=%.1f h=%.0f wx=%d pm25=%.1f aqi=%d -> /data",
         d.temp, d.hum, d.wxCoded, d.pm25, d.aqi);
    if (mListener) mListener();
}

// ── 外部注入 ──────────────────────────────────────────────────
void SensorManager::setTempHum(float temp, float hum) {
    if (temp < -40.0f || temp > 85.0f || hum < 0.0f || hum > 100.0f) {
        LOGW("SensorManager: invalid data t=%.1f h=%.1f", temp, hum);
        return;
    }
    pthread_mutex_lock(&mMtx);
    mInjTh = true;
    mCur.temp = temp;
    mCur.hum  = hum;
    mCur.updatedMs = smNowMs();
    mHasData = true;
    pthread_mutex_unlock(&mMtx);
    ConfigStore* cfg = ConfigStore::getInstance();
    cfg->setCachedTempHum(temp, hum);
    cfg->setWeatherUpdatedMs(smNowMs());
    cfg->setWeatherHasData(true);
    if (mListener) mListener();
}

void SensorManager::setWeather(const std::string& text) {
    if (text.empty()) return;
    pthread_mutex_lock(&mMtx);
    mInjWx = true;
    mWxText = text;
    mCur.updatedMs = smNowMs();
    mHasData = true;
    pthread_mutex_unlock(&mMtx);
    ConfigStore* cfg = ConfigStore::getInstance();
    cfg->setCachedWeather(text);
    cfg->setWeatherUpdatedMs(smNowMs());
    cfg->setWeatherHasData(true);
    if (mListener) mListener();
}

void SensorManager::setLocation(const std::string& city, const std::string& latlon) {
    pthread_mutex_lock(&mMtx);
    if (!city.empty()) mCity = city;
    if (!latlon.empty()) mLatLon = latlon;
    pthread_mutex_unlock(&mMtx);
    ConfigStore* cfg = ConfigStore::getInstance();
    if (!city.empty()) cfg->setWeatherCity(city);
    if (!latlon.empty()) cfg->setWeatherLatLon(latlon);
    LOGI("SensorManager: location -> %s (%s)", mLatLon.c_str(), mCity.c_str());
    refreshAsync();
}

bool SensorManager::setLocationByCity(const std::string& cityName) {
    if (cityName.empty()) return false;
    const std::string url = std::string(kGeoUrl) + "?name=" + urlEncode(cityName) +
                            "&count=1&language=zh&format=json";
    std::string body, err;
    if (!httpGet(url, body, err)) {
        LOGW("SensorManager: geocode '%s' failed: %s", cityName.c_str(), err.c_str());
        return false;
    }
    rapidjson::Document d;
    d.Parse(body.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("results") || !d["results"].IsArray()
        || d["results"].Size() == 0) {
        LOGW("SensorManager: geocode '%s' no result", cityName.c_str());
        return false;
    }
    const rapidjson::Value& r = d["results"][(rapidjson::SizeType)0];
    if (!r.HasMember("latitude") || !r.HasMember("longitude")) return false;
    char ll[64];
    snprintf(ll, sizeof(ll), "%.4f,%.4f", r["latitude"].GetDouble(), r["longitude"].GetDouble());
    std::string show = cityName;
    if (r.HasMember("name") && r["name"].IsString()) show = r["name"].GetString();
    setLocation(show, ll);
    LOGI("SensorManager: geocode '%s' -> %s", cityName.c_str(), ll);
    return true;
}

void SensorManager::setListener(const std::function<void()>& listener) {
    mListener = listener;
}

// ── 读值 ──────────────────────────────────────────────────────
float SensorManager::temp() const {
    pthread_mutex_lock(&mMtx);
    const float v = mCur.temp;
    pthread_mutex_unlock(&mMtx);
    return v;
}
float SensorManager::hum() const {
    pthread_mutex_lock(&mMtx);
    const float v = mCur.hum;
    pthread_mutex_unlock(&mMtx);
    return v;
}
std::string SensorManager::weather() const {
    pthread_mutex_lock(&mMtx);
    const std::string s = mWxText;
    pthread_mutex_unlock(&mMtx);
    return s;
}
int SensorManager::wxCode() const {
    pthread_mutex_lock(&mMtx);
    const int v = mCur.wxCoded;
    pthread_mutex_unlock(&mMtx);
    return v;
}
float SensorManager::pm25() const {
    pthread_mutex_lock(&mMtx);
    const float v = mCur.pm25;
    pthread_mutex_unlock(&mMtx);
    return v;
}
float SensorManager::pm10() const {
    pthread_mutex_lock(&mMtx);
    const float v = mCur.pm10;
    pthread_mutex_unlock(&mMtx);
    return v;
}
int SensorManager::aqi() const {
    pthread_mutex_lock(&mMtx);
    const int v = mCur.aqi;
    pthread_mutex_unlock(&mMtx);
    return v;
}
std::string SensorManager::aqiLevel() const {
    pthread_mutex_lock(&mMtx);
    const std::string s = mLevelText;
    pthread_mutex_unlock(&mMtx);
    return s;
}
long long SensorManager::updatedMs() const {
    pthread_mutex_lock(&mMtx);
    const long long v = mCur.updatedMs;
    pthread_mutex_unlock(&mMtx);
    return v;
}
bool SensorManager::hasData() const {
    pthread_mutex_lock(&mMtx);
    const bool v = mHasData;
    pthread_mutex_unlock(&mMtx);
    return v;
}
std::string SensorManager::city() const {
    pthread_mutex_lock(&mMtx);
    const std::string s = mCity;
    pthread_mutex_unlock(&mMtx);
    return s;
}

std::string SensorManager::statusText() const {
    pthread_mutex_lock(&mMtx);
    const EnvData d = mCur;
    const std::string wx = mWxText;
    const std::string city = mCity;
    const int fail = mFailStreak;
    const bool has = mHasData;
    pthread_mutex_unlock(&mMtx);

    char b[256];
    if (!has) {
        snprintf(b, sizeof(b), "环境：等待首次获取（失败 %d 次）", fail);
        return std::string(b);
    }
    const long long now = smNowMs();
    const long long ageS = (now > 0 && d.updatedMs > 0) ? (now - d.updatedMs) / 1000 : -1;
    if (d.pm25 < 0.0f) {
        snprintf(b, sizeof(b), "环境(%s)：%.1fC %.0f%% %s，PM2.5 无数据（%lld 秒前更新）",
                 city.c_str(), d.temp, d.hum, wx.c_str(), ageS);
    } else {
        snprintf(b, sizeof(b), "环境(%s)：%.1fC %.0f%% %s，PM2.5 %.0f(%s) AQI %d（%lld 秒前更新）",
                 city.c_str(), d.temp, d.hum, wx.c_str(), d.pm25,
                 pm25ToLevel(d.pm25), d.aqi, ageS);
    }
    return std::string(b);
}
