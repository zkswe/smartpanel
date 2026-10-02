/*
 * AlmanacManager.cpp -- 屏保黄历数据（采集 / 解析 / 缓存 / 排版）
 *
 * 数据源、线程模型、字形裁定口径见 AlmanacManager.h 顶部注释。
 */
#include "device/AlmanacManager.h"

#include "device/font_ranges.h"      // 字形位图（自动生成，见 tools/gen_font_ranges.js）
#include "device/cacert_pem.h"       // 内嵌 CA（自动生成）
#include "storage/ConfigStore.h"
#include "utils/Log.h"

#include <curl/curl.h>
#include <rapidjson/document.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

static const int kFirstDelayS = 15;        // 首拉延迟（等 WiFi/NTP 起来）
static const int kFailBaseS   = 60;        // 失败退避基数
static const int kFailMaxS    = 1800;      // 失败退避上限（30min）
static const int kConnectMs   = 6000;
static const int kTotalMs     = 12000;

// ── 域名 / 参数（改这里即可换源）──────────────────────────────
static const char* kUrl36jxs = "https://www.36jxs.com/api/Commonweal/almanac?sun=";
static const char* kUrlApihz = "https://cn.apihz.cn/api/time/getday.php"
                               "?id=88888888&key=88888888&day=";
static const char* kUa       = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                               "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

// 展示用排版预算（控件 400px 宽，见 ui/main.json 的 TextSsAlm*；字号与 main.json 对齐）
static const int kFont1   = 20;            // line1 字号
static const int kFont23  = 18;            // line2/3 字号
static const int kMaxPx   = 400;           // 每行可用像素宽

// ── CA 文件（照 SensorManager 的口径：优先磁盘，找不到就把内嵌的落到 /tmp）──
static const char* kCaCandidates[] = {
    "/tmp/ui/cacert.pem",
    "/mnt/extsd/ui/cacert.pem",
    "/res/ui/cacert.pem",
    NULL
};
static const char* kCaTmpPath = "/tmp/sp_cacert.pem";

static bool alFileUsable(const char* p) {
    struct stat st;
    if (stat(p, &st) != 0) return false;
    return st.st_size > 512;
}

static std::string alResolveCaFile() {
    static std::string cached;
    static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mtx);
    if (!cached.empty()) { const std::string r = cached; pthread_mutex_unlock(&mtx); return r; }
    pthread_mutex_unlock(&mtx);

    std::string found;
    for (size_t i = 0; kCaCandidates[i] != NULL; i++) {
        if (alFileUsable(kCaCandidates[i])) { found = kCaCandidates[i]; break; }
    }
    if (found.empty()) {
        FILE* fp = fopen(kCaTmpPath, "wb");
        if (fp != NULL) {
            const size_t n = sizeof(kCACertPem) - 1;
            const size_t w = fwrite(kCACertPem, 1, n, fp);
            fclose(fp);
            if (w == n && alFileUsable(kCaTmpPath)) found = kCaTmpPath;
            else LOGW("almanac: write embedded ca failed (w=%u/%u errno=%d)", (unsigned)w, (unsigned)n, errno);
        } else {
            LOGW("almanac: open %s failed errno=%d", kCaTmpPath, errno);
        }
    }
    if (found.empty()) LOGW("almanac: no CA -> HTTPS verify disabled");

    pthread_mutex_lock(&mtx);
    cached = found;
    pthread_mutex_unlock(&mtx);
    return found;
}

// ── HTTP GET（libcurl；带"上一次响应给的 Cookie"，黄历的 WAF 挑战要用）──
struct HttpOut {
    long        code = 0;
    std::string body;
    std::string setCookie;      // 已拼成 "a=b; c=d" 形式
};

static size_t alWriteCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t alHeaderCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    const size_t n = size * nmemb;
    std::string line(ptr, n);
    // 大小写不敏感地找 Set-Cookie
    std::string low = line;
    for (size_t i = 0; i < low.size(); i++) low[i] = (char)tolower((unsigned char)low[i]);
    if (low.compare(0, 11, "set-cookie:") == 0) {
        std::string v = line.substr(11);
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
        const size_t end = v.find_first_of("\r\n;");
        if (end != std::string::npos) v = v.substr(0, end);
        if (!v.empty()) {
            std::string* acc = static_cast<std::string*>(ud);
            if (!acc->empty()) *acc += "; ";
            *acc += v;
        }
    }
    return n;
}

// cookieIn 为空 = 不带 Cookie。返回 false 只有网络层错误（HTTP 404 等算成功，由调用方看 code/body）
static bool alHttpGet(const std::string& url, const std::string& cookieIn,
                      HttpOut* out, std::string* err) {
    CURL* h = curl_easy_init();
    if (h == NULL) { *err = "curl_easy_init failed"; return false; }

    const std::string ca = alResolveCaFile();
    out->body.clear();
    out->setCookie.clear();
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, alWriteCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out->body);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, alHeaderCb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &out->setCookie);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, (long)kConnectMs);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)kTotalMs);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, kUa);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");      // 让 libcurl 自己解 gzip
    if (!ca.empty()) curl_easy_setopt(h, CURLOPT_CAINFO, ca.c_str());
    else { curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L); curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L); }
    if (!cookieIn.empty()) curl_easy_setopt(h, CURLOPT_COOKIE, cookieIn.c_str());

    const CURLcode rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out->code);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK) {
        char b[160];
        snprintf(b, sizeof(b), "curl %d: %s", (int)rc, curl_easy_strerror(rc));
        *err = b;
        return false;
    }
    return true;
}

// ── 阿里云 WAF acw_sc__v2 挑战 ────────────────────────────────
//   36jxs 前面挂着阿里云 ESA，裸 GET 会拿到一段 JS（var arg1='<40 hex>' + 混淆的
//   置换表），要求算出一个 cookie 再重放请求。算法是固定的：
//     1) arg1（40 个 hex 字符）按固定置换表重排成 u（40 字符）
//     2) u 与固定 key 逐 2 个 hex 异或，得到 20 字节 -> hex 串
//   置换表与 key 在 2026-10 实测的页面上是常量（见下）。上游改版会导致算不出 ->
//   第二次请求仍拿挑战页 -> 本函数失败 -> 由调用方退到备用源，不会死循环。
static const char* kAcwKey = "3000176000856006061501533003690027800375";
static const int kAcwPos[40] = {
    15,35,29,24,33,16,1,38,10,9,19,31,40,27,22,23,25,13,6,11,
    39,18,20,8,14,21,32,26,2,30,7,4,17,5,3,28,34,37,12,36
};

static bool solveAcwScV2(const std::string& arg1, std::string* cookieVal) {
    if (arg1.size() != 40) return false;
    char q[41];
    for (size_t i = 0; i < 40; i++) q[i] = 0;
    for (size_t x = 0; x < arg1.size(); x++) {
        for (int z = 0; z < 40; z++) {
            if (kAcwPos[z] == (int)x + 1) { q[z] = arg1[x]; break; }
        }
    }
    char out[41];
    for (int i = 0; i < 20; i++) {
        const int a = (int)strtol(std::string(q + i * 2, 2).c_str(), NULL, 16);
        const int b = (int)strtol(std::string(kAcwKey + i * 2, 2).c_str(), NULL, 16);
        snprintf(out + i * 2, 3, "%02x", (a ^ b) & 0xFF);
    }
    out[40] = 0;
    *cookieVal = out;
    return true;
}

// 从挑战页取出 arg1
static bool findArg1(const std::string& body, std::string* arg1) {
    const std::string tag = "var arg1='";
    const size_t p = body.find(tag);
    if (p == std::string::npos) return false;
    const size_t e = body.find('\'', p + tag.size());
    if (e == std::string::npos || e - (p + tag.size()) != 40) return false;
    *arg1 = body.substr(p + tag.size(), 40);
    for (size_t i = 0; i < arg1->size(); i++) {
        if (!isxdigit((unsigned char)(*arg1)[i])) return false;
    }
    return true;
}

// ── 小工具 ───────────────────────────────────────────────────
static long long alNowMs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

// 本机当天 "YYYY-MM-DD"；时钟明显未校时（年份 < 2020）返回空串
static std::string alTodayKey() {
    const time_t t = time(NULL);
    struct tm tmv;
    if (localtime_r(&t, &tmv) == NULL) return std::string();
    if (tmv.tm_year + 1900 < 2020) return std::string();
    char b[32];
    snprintf(b, sizeof(b), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return std::string(b);
}

// UTF-8 串的字符数
static int alUtf8Len(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        int l = 1;
        fontNextChar(s.c_str() + i, &l);
        i += (size_t)(l > 0 ? l : 1);
        n++;
    }
    return n;
}

// 估算渲染宽度（CJK/全角 ≈ fontSize，ASCII/半角 ≈ fontSize/2）——用于按像素预算截断
static int alTextPx(const std::string& s, int font) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        int l = 1;
        const unsigned int cp = fontNextChar(s.c_str() + i, &l);
        i += (size_t)(l > 0 ? l : 1);
        if (cp >= 0x2E80 || (cp >= 0x3000 && cp <= 0x303F)) w += font;         // CJK 及全角标点
        else w += font / 2 + 1;                                                // 半角
    }
    return w;
}

// 按 '|' / 空白 / 顿号切条目（36jxs 用空格分隔、apihz 用 '|'）
//   注：'、'(U+3001) 是多字节字符，写字符字面量会触发 -Wmultichar（本工程 -Werror）
//       -> 按 UTF-8 字节 E3 80 81 判断。
static void alSplitItems(const std::string& s, std::vector<std::string>* out) {
    out->clear();
    std::string cur;
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '|' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
            if (!cur.empty()) { out->push_back(cur); cur.clear(); }
            i++;
            continue;
        }
        if ((unsigned char)c == 0xE3 && i + 2 < s.size() &&
            (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x81) {   // '、'
            if (!cur.empty()) { out->push_back(cur); cur.clear(); }
            i += 3;
            continue;
        }
        int l = 1;
        fontNextChar(s.c_str() + i, &l);
        if (l <= 0) l = 1;
        cur.append(s, i, (size_t)l);
        i += (size_t)l;
    }
    if (!cur.empty()) out->push_back(cur);
}

// 节日串切分：中文节日名几乎都以「节」「日」结尾（"国庆节国际老年人日" -> ["国庆节","国际老年人日"]）
static void alSplitFestivals(const std::string& s, std::vector<std::string>* out) {
    out->clear();
    if (s.empty()) return;
    std::string cur;
    for (size_t i = 0; i < s.size();) {
        int l = 1;
        const unsigned int cp = fontNextChar(s.c_str() + i, &l);
        if (l <= 0) l = 1;
        cur.append(s, i, (size_t)l);
        i += (size_t)l;
        if (cp == 0x8282 || cp == 0x65E5) { out->push_back(cur); cur.clear(); }   // 节 / 日
    }
    if (!cur.empty()) {
        if (out->empty()) out->push_back(cur);
        else (*out)[out->size() - 1] += cur;     // 尾巴并入上一段
    }
}

// 去掉换行/回车（prefs 里按行存 k=v）
static void alStripCrLf(std::string* s) {
    for (size_t i = 0; i < s->size(); i++) {
        if ((*s)[i] == '\n' || (*s)[i] == '\r') (*s)[i] = ' ';
    }
}

static std::string alJstr(const rapidjson::Value& o, const char* k) {
    if (!o.IsObject() || !o.HasMember(k)) return std::string();
    const rapidjson::Value& v = o[k];
    if (!v.IsString()) return std::string();
    std::string s(v.GetString(), v.GetStringLength());
    alStripCrLf(&s);
    return s;
}

// ── 单例 ─────────────────────────────────────────────────────
AlmanacManager* AlmanacManager::getInstance() {
    static AlmanacManager s;
    return &s;
}

// ── 记录编解码（"k=v\n"）─────────────────────────────────────
void AlmanacManager::sanitize(std::string* s) { alStripCrLf(s); }

std::string AlmanacManager::encode(const Data& d) {
    std::string o;
    char b[512];
#define AL_ADD(k, v) do { std::string _v = (v); sanitize(&_v); \
        snprintf(b, sizeof(b), "%s=%s\n", k, _v.c_str()); o += b; } while (0)
    AL_ADD("date",   d.dateKey);
    AL_ADD("lunar",  d.lunar);
    AL_ADD("ganzhi", d.ganzhi);
    AL_ADD("sx",     d.shengxiao);
    AL_ADD("jieri",  d.jieri);
    AL_ADD("jieqi",  d.jieqi);
    AL_ADD("yi",     d.yi);
    AL_ADD("ji",     d.ji);
    AL_ADD("chong",  d.chong);
    AL_ADD("zhishen", d.zhishen);
    AL_ADD("tianshen", d.tianshen);
    AL_ADD("nayin",  d.nayin);
    AL_ADD("xiu",    d.xiu);
    AL_ADD("pengzu", d.pengzu);
    AL_ADD("taishen", d.taishen);
    snprintf(b, sizeof(b), "src=%d\n", d.source);
    o += b;
#undef AL_ADD
    return o;
}

bool AlmanacManager::decode(const std::string& blob, Data* d) {
    if (blob.empty()) return false;
    size_t p = 0;
    while (p < blob.size()) {
        const size_t e = blob.find('\n', p);
        const std::string line = blob.substr(p, (e == std::string::npos) ? std::string::npos : e - p);
        p = (e == std::string::npos) ? blob.size() : e + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if      (k == "date")     d->dateKey   = v;
        else if (k == "lunar")    d->lunar     = v;
        else if (k == "ganzhi")   d->ganzhi    = v;
        else if (k == "sx")       d->shengxiao = v;
        else if (k == "jieri")    d->jieri     = v;
        else if (k == "jieqi")    d->jieqi     = v;
        else if (k == "yi")       d->yi        = v;
        else if (k == "ji")       d->ji        = v;
        else if (k == "chong")    d->chong     = v;
        else if (k == "zhishen")  d->zhishen   = v;
        else if (k == "tianshen") d->tianshen  = v;
        else if (k == "nayin")    d->nayin     = v;
        else if (k == "xiu")      d->xiu       = v;
        else if (k == "pengzu")   d->pengzu    = v;
        else if (k == "taishen")  d->taishen   = v;
        else if (k == "src")      d->source    = atoi(v.c_str());
    }
    d->ok = (!d->dateKey.empty() && !d->lunar.empty());
    return d->ok;
}

// ── 排版：把 Data 变成三行展示串（字库过滤 + 像素预算截断）────────
//   过滤口径：**整条**含字库没有的字 -> 丢该条并打日志（宁可少一条，也不要在板上画空白）。
static std::string alJoinItems(const std::string& raw,
                               const std::string& prefix, int font, std::string* dropped) {
    std::vector<std::string> items;
    alSplitItems(raw, &items);
    std::string cur = prefix;
    bool truncated = false;
    for (size_t i = 0; i < items.size(); i++) {
        if (!fontHasUtf8(items[i].c_str())) {
            if (!dropped->empty()) *dropped += ' ';
            *dropped += items[i];
            continue;
        }
        const std::string cand = cur + " " + items[i];
        // 预留 " 等"（截断指示）的位置：后面还有条目才留
        const bool more = (i + 1 < items.size());
        const int reserve = more ? alTextPx(" 等", font) : 0;
        if (alTextPx(cand, font) + reserve > kMaxPx) {
            truncated = true;
            break;
        }
        cur = cand;
    }
    if (truncated && alTextPx(cur + " 等", font) <= kMaxPx) cur += " 等";
    if (cur == prefix) return std::string();     // 一条都放不下 -> 该行留空
    return cur;
}

void AlmanacManager::rebuildLines() {
    mLine1.clear(); mLine2.clear(); mLine3.clear();
    if (!mHasData) { mLine1 = "黄历获取中"; mLinesDirty = false; return; }

    std::string dropped;

    // ① 农历月日：主字库若没有「廿」（U+5EFF）就写成「二十」，避免静默空白
    std::string lunar = mCur.lunar;
    if (!fontHasChar(0x5EFF)) {
        std::string t;
        for (size_t i = 0; i < lunar.size();) {
            int l = 1;
            const unsigned int cp = fontNextChar(lunar.c_str() + i, &l);
            if (l <= 0) l = 1;
            if (cp == 0x5EFF) t += "二十";
            else t.append(lunar, i, (size_t)l);
            i += (size_t)l;
        }
        lunar = t;
    }

    std::string l1;
    if (fontHasUtf8(lunar.c_str())) l1 = "农历" + lunar;
    if (!mCur.ganzhi.empty() && fontHasUtf8(mCur.ganzhi.c_str()))
        l1 += " · " + mCur.ganzhi + "日";

    // ② 节气 / 节日（按能放下多少放多少；节日按「节/日」切成一个个名字）
    std::vector<std::string> extras;
    if (!mCur.jieqi.empty()) extras.push_back(mCur.jieqi);
    if (!mCur.jieri.empty()) {
        std::vector<std::string> fs;
        alSplitFestivals(mCur.jieri, &fs);
        for (size_t i = 0; i < fs.size(); i++) extras.push_back(fs[i]);
    }
    for (size_t i = 0; i < extras.size(); i++) {
        if (extras[i].empty() || !fontHasUtf8(extras[i].c_str())) { if (!extras[i].empty()) dropped += extras[i]; continue; }
        const std::string cand = l1 + " · " + extras[i];
        if (alTextPx(cand, kFont1) > kMaxPx) break;
        l1 = cand;
    }
    mLine1 = l1;

    // ③ 宜 / 忌
    mLine2 = alJoinItems(mCur.yi, "宜", kFont23, &dropped);
    mLine3 = alJoinItems(mCur.ji, "忌", kFont23, &dropped);

    if (!dropped.empty())
        LOGW("almanac: 字库无字形的条目已跳过 -> %s", dropped.c_str());

    mLinesDirty = false;
}

// ── 生命周期 ─────────────────────────────────────────────────
void AlmanacManager::init() {
    const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) LOGW("almanac: curl_global_init rc=%d", (int)rc);

    ConfigStore* cfg = ConfigStore::getInstance();
    Data d;
    const std::string blob = cfg->almanacBlob();
    const bool has = cfg->almanacHasData() && decode(blob, &d);
    const long long ms = cfg->almanacFetchedMs();

    pthread_mutex_lock(&mMtx);
    mHasData = has;
    if (has) { mCur = d; mLastDayKey = d.dateKey; }
    mUpdatedMs = ms;
    rebuildLines();
    pthread_mutex_unlock(&mMtx);

    LOGI("almanac: init has=%d date=%s lunar=%s src=%d",
         has ? 1 : 0, has ? d.dateKey.c_str() : "-", has ? d.lunar.c_str() : "-",
         has ? d.source : 0);
}

void AlmanacManager::startAuto() {
    pthread_mutex_lock(&mMtx);
    const bool already = mThreadStarted;
    pthread_mutex_unlock(&mMtx);
    if (already) return;
    mStop = false;
    mWantFetch = true;
    if (pthread_create(&mThread, NULL, &AlmanacManager::threadEntry, this) != 0) {
        LOGW("almanac: fetch thread create failed");
        return;
    }
    pthread_mutex_lock(&mMtx);
    mThreadStarted = true;
    pthread_mutex_unlock(&mMtx);
    LOGI("almanac: auto fetch started (first in %ds, then on date change)", kFirstDelayS);
}

void AlmanacManager::refreshAsync() {
    mWantFetch = true;
    pthread_mutex_lock(&mMtx);
    const bool started = mThreadStarted;
    pthread_mutex_unlock(&mMtx);
    if (!started) startAuto();
}

bool AlmanacManager::started() const { return mThreadStarted; }

// ── 后台线程 ─────────────────────────────────────────────────
void* AlmanacManager::threadEntry(void* arg) {
    AlmanacManager* self = static_cast<AlmanacManager*>(arg);

    for (int i = 0; i < kFirstDelayS && !self->mStop; i++) sleep(1);

    while (!self->mStop) {
        const std::string day = alTodayKey();
        const long long now = alNowMs();

        int ivS = 0;                                   // 成功过 -> 换日立即拉，不额外等
        if (self->mFailStreak > 0) {
            ivS = kFailBaseS;
            for (int i = 1; i < self->mFailStreak && ivS < kFailMaxS; i++) ivS *= 2;
            if (ivS > kFailMaxS) ivS = kFailMaxS;
        }
        const bool due = (self->mLastTryMs == 0) || (now > 0 && (now - self->mLastTryMs) >= (long long)ivS * 1000);
        const bool dayChanged = (!day.empty() && day != self->mLastDayKey);

        if (self->mWantFetch || (dayChanged && due)) {
            self->mWantFetch = false;
            self->mLastTryMs = (now > 0) ? now : 1;

            if (day.empty()) {
                // 还没校时（RTC 未就绪）-> 不发请求，避免拉到错的日期
                LOGW("almanac: 时钟未就绪（等待 NTP），暂不拉取");
                for (int i = 0; i < 30 && !self->mStop; i++) sleep(1);
                continue;
            }

            Data d;
            std::string err;
            if (self->fetchOnce(&d, &err)) {
                self->mFailStreak = 0;
                pthread_mutex_lock(&self->mMtx);
                self->mPending = d;
                self->mHasPending = true;
                pthread_mutex_unlock(&self->mMtx);
                LOGI("almanac: fetch ok src=%d date=%s lunar=%s ganzhi=%s yi=%d项 ji=%d项",
                     d.source, d.dateKey.c_str(), d.lunar.c_str(), d.ganzhi.c_str(),
                     alUtf8Len(d.yi), alUtf8Len(d.ji));
            } else {
                self->mFailStreak++;
                LOGW("almanac: fetch failed (streak=%d): %s", self->mFailStreak, err.c_str());
            }
        }
        sleep(1);
    }
    LOGI("almanac: fetch thread exit");
    return NULL;
}

// ── 解析：36jxs（客户指定源）──────────────────────────────────
static bool parse36jxs(const std::string& body, AlmanacManager::Data* out, std::string* err) {
    if (body.find("acw_sc__v2") != std::string::npos || body.find("var arg1=") != std::string::npos) {
        *err = "36jxs: 仍是 WAF 挑战页（cookie 未被接受）";
        return false;
    }
    rapidjson::Document d;
    d.Parse(body.c_str());
    if (d.HasParseError() || !d.IsObject()) { *err = "36jxs: json 解析失败"; return false; }
    const int code = (d.HasMember("code") && d["code"].IsInt()) ? d["code"].GetInt() : -1;
    if (code != 1) {
        char b[128];
        snprintf(b, sizeof(b), "36jxs: 上游 code=%d msg=%s", code,
                 alJstr(d, "msg").c_str());
        *err = b;
        return false;
    }
    if (!d.HasMember("data") || !d["data"].IsObject()) { *err = "36jxs: 无 data"; return false; }
    const rapidjson::Value& v = d["data"];

    out->lunar     = alJstr(v, "LMonth") + alJstr(v, "LDay");
    out->ganzhi    = alJstr(v, "TianGanDiZhiDay");
    out->shengxiao = alJstr(v, "LYear");
    out->jieqi     = alJstr(v, "SolarTermName");
    out->jieri     = alJstr(v, "GJie");
    if (out->jieri.empty()) out->jieri = alJstr(v, "LJie");
    out->yi        = alJstr(v, "Yi");
    out->ji        = alJstr(v, "Ji");
    out->chong     = alJstr(v, "Chong");
    out->zhishen   = alJstr(v, "JianShen");
    out->tianshen  = alJstr(v, "ShenWei");
    out->nayin     = alJstr(v, "WuxingNaDay");
    out->xiu       = alJstr(v, "XingEast");
    out->pengzu    = alJstr(v, "PengZu");
    out->taishen   = alJstr(v, "Taishen");
    out->dateKey   = alJstr(v, "GregorianDateTime");
    out->source    = 1;
    out->ok = !out->lunar.empty();
    if (!out->ok) { *err = "36jxs: 农历为空"; return false; }
    return true;
}

// ── 解析：apihz（备用源，实测可用）────────────────────────────
static bool parseApihz(const std::string& body, AlmanacManager::Data* out, std::string* err) {
    rapidjson::Document d;
    d.Parse(body.c_str());
    if (d.HasParseError() || !d.IsObject()) { *err = "apihz: json 解析失败"; return false; }
    if (!d.HasMember("nyue") || !d.HasMember("nri")) {
        *err = "apihz: 无 nyue/nri（" + alJstr(d, "msg") + alJstr(d, "info") + "）";
        return false;
    }
    out->lunar     = alJstr(d, "nyue") + alJstr(d, "nri");
    out->ganzhi    = alJstr(d, "ganzhiri");
    out->shengxiao = alJstr(d, "DAYSHENGXIAO");
    out->jieqi     = alJstr(d, "jieqi");
    out->jieri     = alJstr(d, "jieri");
    out->yi        = alJstr(d, "yi");
    out->ji        = alJstr(d, "ji");
    out->chong     = alJstr(d, "DAYCHONGDESC");
    out->zhishen   = alJstr(d, "ZHIXING");
    {   // 值神 + 吉凶 + 星宿（拼成人能读的短串）
        std::string ts = alJstr(d, "DAYTIANSHEN");
        const std::string lk = alJstr(d, "DAYTIANSHENLUCK");
        if (!ts.empty() && !lk.empty()) ts += " " + lk;
        out->tianshen = ts;
        std::string xiu = alJstr(d, "XIU");
        const std::string xa = alJstr(d, "XIUANIMAL");
        const std::string xl = alJstr(d, "XIULUCK");
        if (!xa.empty()) xiu += xa;
        if (!xl.empty()) xiu += "-" + xl;
        out->xiu = xiu;
    }
    out->nayin     = alJstr(d, "DAYNAYIN");
    {
        std::string pz = alJstr(d, "PENGZUGAN");
        const std::string pzz = alJstr(d, "PENGZUZHI");
        if (!pz.empty() && !pzz.empty()) pz += " " + pzz;
        out->pengzu = pz;
    }
    out->taishen   = alJstr(d, "taishen");
    {   // "20261001" -> "2026-10-01"
        const std::string id = alJstr(d, "id");
        if (id.size() == 8) out->dateKey = id.substr(0, 4) + "-" + id.substr(4, 2) + "-" + id.substr(6, 2);
    }
    out->source = 2;
    out->ok = !out->lunar.empty();
    if (!out->ok) { *err = "apihz: 农历为空"; return false; }
    return true;
}

// ── 一次完整拉取：按顺序试各数据源，第一个成功的胜出 ──────────────
bool AlmanacManager::fetchOnce(Data* out, std::string* err) {
    const std::string day = alTodayKey();
    if (day.empty()) { *err = "时钟未就绪"; return false; }

    Data d;
    std::string e1, e2;

    // ── ① 客户指定源：36jxs（前面有阿里云 WAF，需要两次请求）──
    {
        const std::string url = std::string(kUrl36jxs) + day;
        HttpOut r;
        std::string e;
        if (alHttpGet(url, std::string(), &r, &e)) {
            if (r.code == 200 && r.body.find("var arg1=") != std::string::npos) {
                std::string arg1;
                if (findArg1(r.body, &arg1)) {
                    std::string ck;
                    if (solveAcwScV2(arg1, &ck)) {
                        std::string cookie = "acw_sc__v2=" + ck;
                        if (!r.setCookie.empty()) cookie = r.setCookie + "; " + cookie;
                        HttpOut r2;
                        std::string e2;
                        if (alHttpGet(url, cookie, &r2, &e2) && r2.code == 200) {
                            if (parse36jxs(r2.body, &d, &e1)) {
                                d.dateKey = day;                 // 以本机当天为准
                                pthread_mutex_lock(&mMtx);
                                mLastDayKey = day;
                                pthread_mutex_unlock(&mMtx);
                                *out = d;
                                return true;
                            }
                        } else {
                            e1 = "36jxs 二次请求失败: " + e2;
                        }
                    } else {
                        e1 = "36jxs: acw_sc__v2 求解失败（上游改版？）";
                    }
                } else {
                    e1 = "36jxs: 挑战页里找不到 arg1";
                }
            } else if (r.code == 200) {
                if (parse36jxs(r.body, &d, &e1)) {
                    d.dateKey = day;
                    pthread_mutex_lock(&mMtx);
                    mLastDayKey = day;
                    pthread_mutex_unlock(&mMtx);
                    *out = d;
                    return true;
                }
            } else {
                char b[64];
                snprintf(b, sizeof(b), "36jxs http %ld", r.code);
                e1 = b;
            }
        } else {
            e1 = "36jxs: " + e;
        }
    }

    // ── ② 备用源：apihz ───────────────────────────────────────
    {
        const std::string url = std::string(kUrlApihz) + day;
        HttpOut r;
        std::string e;
        if (alHttpGet(url, std::string(), &r, &e)) {
            if (r.code == 200) {
                if (parseApihz(r.body, &d, &e2)) {
                    d.dateKey = day;
                    pthread_mutex_lock(&mMtx);
                    mLastDayKey = day;
                    pthread_mutex_unlock(&mMtx);
                    if (!e1.empty()) LOGW("almanac: 主源失败已切备用源 -> %s", e1.c_str());
                    *out = d;
                    return true;
                }
            } else {
                char b[64];
                snprintf(b, sizeof(b), "apihz http %ld", r.code);
                e2 = b;
            }
        } else {
            e2 = "apihz: " + e;
        }
    }

    *err = e1 + " | " + e2;
    return false;
}

// ── 主线程每秒：提交后台结果并落盘 ─────────────────────────────
void AlmanacManager::tick() {
    Data d;
    bool has = false;
    pthread_mutex_lock(&mMtx);
    if (mHasPending) {
        d = mPending;
        mHasPending = false;
        mCur = d;
        mHasData = d.ok;
        mUpdatedMs = alNowMs();
        rebuildLines();
        has = true;
    }
    pthread_mutex_unlock(&mMtx);
    if (!has) return;

    // StoragePreferences 只由主线程写 —— 顺序与 SensorManager::tick 一致
    ConfigStore* cfg = ConfigStore::getInstance();
    cfg->setAlmanacBlob(encode(d));
    cfg->setAlmanacFetchedMs(mUpdatedMs);
    cfg->setAlmanacHasData(d.ok);
    LOGD("almanac: tick 已落盘 date=%s src=%d", d.dateKey.c_str(), d.source);
}

// ── 读值 ─────────────────────────────────────────────────────
std::string AlmanacManager::line1() const {
    pthread_mutex_lock(&mMtx);
    if (mLinesDirty) const_cast<AlmanacManager*>(this)->rebuildLines();
    const std::string r = mLine1;
    pthread_mutex_unlock(&mMtx);
    return r;
}

std::string AlmanacManager::line2() const {
    pthread_mutex_lock(&mMtx);
    if (mLinesDirty) const_cast<AlmanacManager*>(this)->rebuildLines();
    const std::string r = mLine2;
    pthread_mutex_unlock(&mMtx);
    return r;
}

std::string AlmanacManager::line3() const {
    pthread_mutex_lock(&mMtx);
    if (mLinesDirty) const_cast<AlmanacManager*>(this)->rebuildLines();
    const std::string r = mLine3;
    pthread_mutex_unlock(&mMtx);
    return r;
}

bool AlmanacManager::hasData() const {
    pthread_mutex_lock(&mMtx);
    const bool r = mHasData;
    pthread_mutex_unlock(&mMtx);
    return r;
}

long long AlmanacManager::updatedMs() const {
    pthread_mutex_lock(&mMtx);
    const long long r = mUpdatedMs;
    pthread_mutex_unlock(&mMtx);
    return r;
}

int AlmanacManager::source() const {
    pthread_mutex_lock(&mMtx);
    const int r = mHasData ? mCur.source : 0;
    pthread_mutex_unlock(&mMtx);
    return r;
}

std::string AlmanacManager::raw(int field) const {
    pthread_mutex_lock(&mMtx);
    const std::string r = (field == 0) ? mCur.lunar : (field == 1 ? mCur.yi : mCur.ji);
    pthread_mutex_unlock(&mMtx);
    return r;
}

std::string AlmanacManager::statusText() const {
    pthread_mutex_lock(&mMtx);
    char b[256];
    snprintf(b, sizeof(b), "alm ok=%d src=%d date=%s lunar=%s yi=%u ji=%u",
             mHasData ? 1 : 0, mCur.source, mCur.dateKey.c_str(), mCur.lunar.c_str(),
             (unsigned)mCur.yi.size(), (unsigned)mCur.ji.size());
    pthread_mutex_unlock(&mMtx);
    return std::string(b);
}
