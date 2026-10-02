/*
 * ConfigStore.cpp -- 面板配置持久化实现（StoragePreferences / /data）
 */
#include "storage/ConfigStore.h"

#include <rapidjson/document.h>            // 出厂默认配置文件解析

#include "security/SecurityManager.h"      // v7.4：芯片唯一 ID（getDevID）
#include "storage/StoragePreferences.h"
#include "utils/Log.h"
#include "utils/TimeHelper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ── 键名（历史键保持兼容，勿改）──────────────────────────────
static const char* kKeyDeviceId   = "sp_device_id";
static const char* kKeyRelayName  = "sp_relay_name";     // + "1".."3"
static const char* kKeyRelayState = "sp_relay_state";    // bitmask
static const char* kKeySwShow     = "sw_show_";          // + "1".."3"（按键配置子页；历史键，勿改）
static const char* kKeyBrightWork = "sp_bright_work";    // 0..100
static const char* kKeyBrightSs   = "sp_bright_ss";      // 0..100
static const char* kKeyOffEnable  = "sp_off_en";         // 关屏总开关 0/1
static const char* kKeyOffStart   = "sp_off_start";      // 0..23
static const char* kKeyOffEnd     = "sp_off_end";        // 0..23
static const char* kKeyOffStartMin= "sp_off_start_min";  // 0..59（内部刻度，UI 不暴露；缺省 0）
static const char* kKeyOffEndMin  = "sp_off_end_min";    // 0..59
static const char* kKeySsItemPfx  = "ss_item_";          // + item -> 1/0（sssetLogic 同键）
static const char* kKeySsPosPfx   = "sp_ss_pos_";        // + item + "_x"/"_y"
static const char* kKeySceneJson  = "sp_scene_json";
static const char* kKeyRunMode    = "sp_run_mode";       // 0=HA 1=主机 2=从机
static const char* kKeyDevName    = "sp_dev_name";
static const char* kKeyMasterIp   = "sp_master_ip";
static const char* kKeyPeerName   = "sp_peer_name_";     // + deviceId
static const char* kKeyMqttEn     = "sp_mqtt_en";
static const char* kKeyMqttServer = "sp_mqtt_server";
static const char* kKeyMqttUser   = "sp_mqtt_user";
static const char* kKeyMqttPass   = "sp_mqtt_pass";
static const char* kKeyVideoInt   = "sp_video_int";      // 轮播间隔秒（videoLogic 同键）
static const char* kKeyPhotoSec   = "sp_photo_sec";      // 图片显示时长秒（默认 15）
// 屏保声音（视频音轨；2026-10-01）：sp_ss_sound = 1/0（默认 1 开）、sp_ss_vol = 0..100（默认 60）
static const char* kKeySsSound    = "sp_ss_sound";
static const char* kKeySsVol      = "sp_ss_vol";
static const char* kKeyQrUrl      = "sp_qr_url";         // 相册页二维码内容（非空则用它）
static const char* kKeyMpAppId    = "sp_mp_appid";       // 微信小程序 AppID
static const char* kKeyQrImgUrl   = "sp_qr_img_url";     // 远端小程序码图 URL（下载显示）
static const char* kKeyWebCfgCode = "sp_web_cfg_code";  // 板内配置网页口令码（钟工 2026-09-30）

// 默认二维码内容：小程序传图链接（钟工 2026-09-30）
//   来源：把原内置位图素材 images/album_qr_mp128.png 用 zxing-cpp 解出来（2026-09-30）：
//     https://mp.weixin.qq.com/a/~03uyHJCCRb4XvYYqL6pgKw~~
//   为什么要解出来重生成：位图 128px / 37 模块，模块宽不是整数像素，压缩后边缘发糊；
//   改用 ZKQrcode 控件现场生成，模块对齐、锐利。
static const char* kQrUrlDefault =
    "https://mp.weixin.qq.com/a/~03uyHJCCRb4XvYYqL6pgKw~~";
static const char* kKeyTemp       = "sp_temp";
static const char* kKeyHum        = "sp_hum";
static const char* kKeyWeather    = "sp_weather";
// 天气 / 空气质量（真实数据，2026-10-01）：城市、经纬度、WMO 代码、PM2.5/PM10/AQI、上次成功时刻
static const char* kKeyWxCity     = "sp_wx_city";
static const char* kKeyWxLatLon   = "sp_wx_latlon";
static const char* kKeyWxCode     = "sp_wx_code";
static const char* kKeyWxPm25     = "sp_wx_pm25";
static const char* kKeyWxPm10     = "sp_wx_pm10";
static const char* kKeyWxAqi      = "sp_wx_aqi";
static const char* kKeyWxMs       = "sp_wx_ms";
static const char* kKeyWxOk       = "sp_wx_ok";      // 1 = 已成功取到过真实数据
// 黄历（屏保顶部黄历区，2026-10-01）：整条记录 + 拉取时刻 + 有效标志
static const char* kKeyAlmBlob    = "sp_alm_blob";    // "k=v\n" 多行记录
static const char* kKeyAlmMs      = "sp_alm_ms";
static const char* kKeyAlmOk      = "sp_alm_ok";
// 默认位置 = 深圳福田（22.5431, 114.0579）。改城市走 UDP api: {"cmd":"config_weather","city":"杭州"}
static const char* kWxCityDefault   = "深圳";
static const char* kWxLatLonDefault = "22.5431,114.0579";

ConfigStore* ConfigStore::getInstance() {
    static ConfigStore s;
    return &s;
}

std::string ConfigStore::key(int ch) const {
    return std::string(kKeyRelayName) + char('0' + ch);
}

void ConfigStore::init() {
    // v7.4（2026-09-28 钟工）：device_id 改用**芯片唯一 ID**（SECURITYMANAGER->getDevID，取前 8 字节），
    //   替代 rand() 随机值 —— 随机值只要 pref 一丢就换新身份（现场一台上下午换过 3 个 ID）。
    //   迁移口径：算出的芯片 ID 与存量不同 -> 以芯片 ID 为准并覆盖（一次性换名，之后永不变）。
    {
        const std::string hw = hwDeviceId();
        std::string cur = StoragePreferences::getString(kKeyDeviceId, "");
        if (!hw.empty() && cur != hw) {
            LOGW("ConfigStore: deviceId migrate %s -> %s (chip id)",
                 cur.empty() ? "(empty)" : cur.c_str(), hw.c_str());
            StoragePreferences::putString(kKeyDeviceId, hw);
            mCachedId = hw;
        } else if (!hw.empty()) {
            mCachedId = hw;
        } else if (cur.empty()) {
            std::string id = generateDeviceId();
            StoragePreferences::putString(kKeyDeviceId, id);
            mCachedId = id;
            LOGW("ConfigStore: chip id unavailable -> fallback id %s", id.c_str());
        }
        LOGI("ConfigStore: deviceId=%s (chip=%s)", deviceId().c_str(), hw.empty() ? "n/a" : hw.c_str());
    }
    const char* def[4] = { "", "客厅灯", "卧室灯", "灯带" };
    for (int i = 1; i <= 3; i++) {
        if (StoragePreferences::getString(key(i), "").empty()) {
            StoragePreferences::putString(key(i), def[i]);
            LOGD("ConfigStore: default name ch%d = %s", i, def[i]);
        }
    }
    LOGD("ConfigStore: init dev=%s br=%d/%d off=%d %d-%d",
         deviceId().c_str(), workBrightness(), screensaverBrightness(),
         offEnabled() ? 1 : 0, offStartHour(), offEndHour());
    applyFactoryDefaults();
}

// ── 出厂默认（钟工 2026-09-30）──────────────────────────────
// 背景：开源版把代码里写死的 broker 默认值去掉了（内网地址不进仓），
//   但现场设备需要“开箱就能上 HA/Domoticz”。口径：
//   **出厂参数不进代码，走一份配置文件**；首次启动（prefs 里还没配）时读入并落盘到 prefs，
//   之后一律以 prefs 为准（现场也能用板内网页再改）。
// 查找顺序：/mnt/sdnand/panel-defaults.json → /mnt/extsd/panel-defaults.json → /res/etc/panel-defaults.json
// 支持的键（都可缺省）：mqtt_server / mqtt_user / mqtt_pass / mqtt_en
static const char* kFactoryCfgCandidates[] = {
    "/mnt/sdnand/panel-defaults.json",
    "/mnt/extsd/panel-defaults.json",
    "/res/etc/panel-defaults.json",
    NULL
};

void ConfigStore::applyFactoryDefaults() {
    // 已经配过就不动（prefs 优先）
    if (!StoragePreferences::getString(kKeyMqttServer, "").empty()) return;

    std::string text;
    const char* used = NULL;
    for (int i = 0; kFactoryCfgCandidates[i] != NULL; i++) {
        FILE* f = ::fopen(kFactoryCfgCandidates[i], "rb");
        if (f == NULL) continue;
        char buf[1024];
        size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
        ::fclose(f);
        buf[n] = '\0';
        text = buf;
        used = kFactoryCfgCandidates[i];
        break;
    }
    if (used == NULL || text.empty()) {
        LOGD("ConfigStore: no factory defaults file (mqtt server stays unset)");
        return;
    }

    rapidjson::Document d;
    if (d.Parse(text.c_str()).HasParseError() || !d.IsObject()) {
        LOGW("ConfigStore: factory defaults parse error (%s)", used);
        return;
    }
    const char* server = (d.HasMember("mqtt_server") && d["mqtt_server"].IsString())
                         ? d["mqtt_server"].GetString() : "";
    if (server == NULL || server[0] == '\0') {
        LOGD("ConfigStore: factory defaults has no mqtt_server (%s)", used);
        return;
    }
    const char* user = (d.HasMember("mqtt_user") && d["mqtt_user"].IsString())
                       ? d["mqtt_user"].GetString() : "";
    const char* pass = (d.HasMember("mqtt_pass") && d["mqtt_pass"].IsString())
                       ? d["mqtt_pass"].GetString() : "";
    bool en = true;
    if (d.HasMember("mqtt_en") && d["mqtt_en"].IsBool()) en = d["mqtt_en"].GetBool();

    setMqttConfig(en, server, user, pass);
    LOGW("ConfigStore: factory defaults applied from %s -> server=%s (persisted to prefs)",
         used, server);
}

// ── 设备 ────────────────────────────────────────────────────
std::string ConfigStore::hwDeviceId() {
    uint8_t dev[16];
    memset(dev, 0, sizeof(dev));
    int n = 0;
    try {
        n = SECURITYMANAGER->getDevID(dev);
    } catch (...) {
        n = 0;
    }
    if (n < 8) return std::string();                 // Z20/Z21/T113 口径：返回长度；不足 8 字节视为不可用
    char buf[32];
    snprintf(buf, sizeof(buf), "PANEL-%02X%02X%02X%02X%02X%02X%02X%02X",
             dev[0], dev[1], dev[2], dev[3], dev[4], dev[5], dev[6], dev[7]);
    return std::string(buf);
}

std::string ConfigStore::macDeviceId() {
    FILE* f = fopen("/sys/class/net/wlan0/address", "r");
    if (f == NULL) return std::string();
    char s[64] = {0};
    if (fgets(s, sizeof(s), f) == NULL) { fclose(f); return std::string(); }
    fclose(f);
    unsigned m[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) return std::string();
    char buf[32];
    snprintf(buf, sizeof(buf), "PANEL-%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return std::string(buf);
}

std::string ConfigStore::generateDeviceId() {
    std::string hw = hwDeviceId();
    if (!hw.empty()) return hw;
    std::string mac = macDeviceId();
    if (!mac.empty()) return mac;
    long long t = TimeHelper::getCurrentTime();           // 最后兜底（老随机法）
    unsigned seed = (unsigned)(t ^ (t >> 32) ^ (unsigned)getpid());
    srand(seed);
    char buf[24];
    snprintf(buf, sizeof(buf), "PANEL-%04X%04X", rand() & 0xFFFF, rand() & 0xFFFF);
    return std::string(buf);
}

std::string ConfigStore::deviceId() {
    if (!mCachedId.empty()) return mCachedId;              // v7.4：绝不再返回 "unknown"
    std::string id = StoragePreferences::getString(kKeyDeviceId, "");
    if (!id.empty() && id != "unknown") { mCachedId = id; return mCachedId; }
    mCachedId = generateDeviceId();
    StoragePreferences::putString(kKeyDeviceId, mCachedId);
    LOGW("ConfigStore: deviceId was missing -> derived %s", mCachedId.c_str());
    return mCachedId;
}

std::string ConfigStore::deviceName() {
    std::string n = StoragePreferences::getString(kKeyDevName, "");
    return n.empty() ? deviceId() : n;
}

void ConfigStore::setDeviceName(const std::string& name) {
    StoragePreferences::putString(kKeyDevName, name);
    LOGD("ConfigStore: deviceName -> %s", name.c_str());
}

// ── 3 路开关 ────────────────────────────────────────────────
std::string ConfigStore::relayName(int ch) {
    if (ch < 1 || ch > 3) return "";
    std::string n = StoragePreferences::getString(key(ch), "");
    return n.empty() ? std::string("开关") : n;
}

void ConfigStore::setRelayName(int ch, const std::string& name) {
    if (ch < 1 || ch > 3) return;
    StoragePreferences::putString(key(ch), name);
    LOGD("ConfigStore: name ch%d -> %s", ch, name.c_str());
}

// ── 主界面开关显隐（钟工 2026-09-29：设置要持久化/记住）──────
bool ConfigStore::swVisible(int ch) {
    if (ch < 1 || ch > 3) return true;
    return StoragePreferences::getInt(std::string(kKeySwShow) + char('0' + ch), 1) != 0;
}

void ConfigStore::setSwVisible(int ch, bool visible) {
    if (ch < 1 || ch > 3) return;
    std::string k = std::string(kKeySwShow) + char('0' + ch);
    StoragePreferences::putInt(k, visible ? 1 : 0);
    LOGD("ConfigStore: %s -> %d", k.c_str(), visible ? 1 : 0);   // 现场可查（记忆是否落盘）
}

bool ConfigStore::relayState(int ch) {
    if (ch < 1 || ch > 3) return false;
    int mask = StoragePreferences::getInt(kKeyRelayState, 0);
    return ((mask >> (ch - 1)) & 1) != 0;
}

void ConfigStore::setRelayState(int ch, bool on) {
    if (ch < 1 || ch > 3) return;
    int mask = StoragePreferences::getInt(kKeyRelayState, 0);
    if (on) mask |= (1 << (ch - 1));
    else    mask &= ~(1 << (ch - 1));
    StoragePreferences::putInt(kKeyRelayState, mask);
}

// ── 亮度 ────────────────────────────────────────────────────
int ConfigStore::workBrightness() {
    int v = StoragePreferences::getInt(kKeyBrightWork, 80);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

void ConfigStore::setWorkBrightness(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    StoragePreferences::putInt(kKeyBrightWork, v);
    LOGD("ConfigStore: work brightness -> %d", v);
}

int ConfigStore::screensaverBrightness() {
    int v = StoragePreferences::getInt(kKeyBrightSs, 30);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

void ConfigStore::setScreensaverBrightness(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    StoragePreferences::putInt(kKeyBrightSs, v);
    LOGD("ConfigStore: screensaver brightness -> %d", v);
}

// ── 关屏设置 ────────────────────────────────────────────────
bool ConfigStore::offEnabled() {
    return StoragePreferences::getInt(kKeyOffEnable, 1) != 0;   // 默认启用（设计说明书：必选）
}

void ConfigStore::setOffEnabled(bool on) {
    StoragePreferences::putInt(kKeyOffEnable, on ? 1 : 0);
    LOGD("ConfigStore: offEnabled -> %d", on ? 1 : 0);
}

int ConfigStore::offStartHour() {
    int v = StoragePreferences::getInt(kKeyOffStart, 0);
    if (v < kOffHourMin) v = kOffHourMin;
    if (v > kOffHourMax) v = kOffHourMax;
    return v;
}

int ConfigStore::offEndHour() {
    int v = StoragePreferences::getInt(kKeyOffEnd, 7);
    if (v < kOffHourMin) v = kOffHourMin;
    if (v > kOffHourMax) v = kOffHourMax;
    return v;
}

int ConfigStore::offStartMin() {
    int v = StoragePreferences::getInt(kKeyOffStartMin, 0);
    if (v < kOffMinMin) v = kOffMinMin;
    if (v > kOffMinMax) v = kOffMinMax;
    return v;
}

int ConfigStore::offEndMin() {
    int v = StoragePreferences::getInt(kKeyOffEndMin, 0);
    if (v < kOffMinMin) v = kOffMinMin;
    if (v > kOffMinMax) v = kOffMinMax;
    return v;
}

void ConfigStore::setOffPeriod(int startHour, int endHour) {
    if (startHour < kOffHourMin) startHour = kOffHourMin;
    if (startHour > kOffHourMax) startHour = kOffHourMax;
    if (endHour < kOffHourMin) endHour = kOffHourMin;
    if (endHour > kOffHourMax) endHour = kOffHourMax;
    StoragePreferences::putInt(kKeyOffStart, startHour);
    StoragePreferences::putInt(kKeyOffEnd, endHour);
    // UI 只暴露小时 -> 写入时段即代表"整点"，清零分钟刻度（内部/调试用的分钟不残留）
    StoragePreferences::putInt(kKeyOffStartMin, 0);
    StoragePreferences::putInt(kKeyOffEndMin, 0);
    LOGD("ConfigStore: off period -> %d:00-%d:00", startHour, endHour);
}

bool ConfigStore::offPeriodValid() {
    int s = offStartHour() * 60 + offStartMin();
    int e = offEndHour() * 60 + offEndMin();
    return s != e;                                // 起止「分钟-of-day」相同=无效
}

bool ConfigStore::offPeriodLong() {
    // 时长 > 12h 的时段视为「长时段」（仅用于行值摘要提示）
    int s = offStartHour() * 60 + offStartMin();
    int e = offEndHour() * 60 + offEndMin();
    if (s == e) return false;
    int span = (e > s) ? (e - s) : (e + 1440 - s);
    return span > 720;
}

bool ConfigStore::inOffPeriod(int hour, int min) {
    if (!offEnabled()) return false;
    int s = offStartHour() * 60 + offStartMin();
    int e = offEndHour() * 60 + offEndMin();
    if (s == e) return false;                     // 时段无效：不熄屏
    int cur = hour * 60 + min;
    if (s < e) return cur >= s && cur < e;
    return cur >= s || cur < e;                   // 跨零点（如 23:00-6:00）
}

// ── 屏保显示项（显隐 + 位置）────────────────────────────────
bool ConfigStore::ssItemVisible(const std::string& item, bool def) {
    return StoragePreferences::getInt(kKeySsItemPfx + item, def ? 1 : 0) != 0;
}

void ConfigStore::setSsItemVisible(const std::string& item, bool visible) {
    StoragePreferences::putInt(kKeySsItemPfx + item, visible ? 1 : 0);
}

void ConfigStore::ssItemPos(const std::string& item, int& x, int& y) {
    // "未设置"用哨兵 kSsPosUnset。⚠️ 不能用 -1：钟工 2026-10-01 起屏保浮层允许
    // **负坐标**（摆到屏幕外），-1 是合法位置。
    x = StoragePreferences::getInt(kKeySsPosPfx + item + "_x", kSsPosUnset);
    y = StoragePreferences::getInt(kKeySsPosPfx + item + "_y", kSsPosUnset);
}

void ConfigStore::setSsItemPos(const std::string& item, int x, int y) {
    StoragePreferences::putInt(kKeySsPosPfx + item + "_x", x);
    StoragePreferences::putInt(kKeySsPosPfx + item + "_y", y);
}

void ConfigStore::resetSsItemPos(const std::string& item) {
    StoragePreferences::remove(kKeySsPosPfx + item + "_x");
    StoragePreferences::remove(kKeySsPosPfx + item + "_y");
}

// 编辑模式是进程内瞬态标志（设置页->屏保页跳转同一进程）：
// 持久化会导致重启后屏保卡在编辑模式，故只用内存静态变量。
static bool sSsEditMode = false;

bool ConfigStore::ssEditMode() {
    return sSsEditMode;
}

void ConfigStore::setSsEditMode(bool on) {
    sSsEditMode = on;
    LOGD("ConfigStore: ssEditMode -> %d", on ? 1 : 0);
}

// ── 情景 / 运行模式 ─────────────────────────────────────────
std::string ConfigStore::sceneJson() {
    return StoragePreferences::getString(kKeySceneJson, "");
}

void ConfigStore::saveSceneJson(const std::string& json) {
    StoragePreferences::putString(kKeySceneJson, json);
}

int ConfigStore::runMode() {
    int m = StoragePreferences::getInt(kKeyRunMode, MODE_HA);
    if (m < MODE_HA || m > MODE_SLAVE) m = MODE_HA;
    return m;
}

void ConfigStore::setRunMode(int mode) {
    if (mode < MODE_HA || mode > MODE_SLAVE) mode = MODE_HA;
    StoragePreferences::putInt(kKeyRunMode, mode);
    LOGD("ConfigStore: runMode -> %d", mode);
}

std::string ConfigStore::masterIp() {
    return StoragePreferences::getString(kKeyMasterIp, "");
}

void ConfigStore::setMasterIp(const std::string& ip) {
    StoragePreferences::putString(kKeyMasterIp, ip);
}

std::string ConfigStore::peerName(const std::string& deviceId) {
    return StoragePreferences::getString(kKeyPeerName + deviceId, "");
}

void ConfigStore::setPeerName(const std::string& deviceId, const std::string& name) {
    StoragePreferences::putString(kKeyPeerName + deviceId, name);
}

bool ConfigStore::mqttEnabled() {
    return StoragePreferences::getBool(kKeyMqttEn, true);
}

// 钟工 2026-09-30：开源版**不写死**任何服务器/账号 —— 默认全空，
//   用户在「运行模式 -> HA」页扫面板上的二维码，用手机网页填服务器地址与令牌。
std::string ConfigStore::mqttServer() {
    return StoragePreferences::getString(kKeyMqttServer, "");
}

std::string ConfigStore::mqttUser() {
    return StoragePreferences::getString(kKeyMqttUser, "");
}

std::string ConfigStore::mqttPassword() {
    return StoragePreferences::getString(kKeyMqttPass, "");
}

bool ConfigStore::mqttConfigured() {
    return !mqttServer().empty();
}

// ── 板内配置网页口令码 ──────────────────────────────────────
// 6 位大写字母+数字（去掉 I/O/0/1 这些易混字符），首次生成后持久化；
// 二维码/面板文案里带上它，网页只在 http://<ip>:8080/<code>/ 可访问。
static const char kWebCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

std::string ConfigStore::webCfgCode() {
    std::string c = StoragePreferences::getString(kKeyWebCfgCode, "");
    if (!c.empty()) return c;
    srand((unsigned)(TimeHelper::getCurrentTime() ^ (unsigned)getpid()));
    c.clear();
    for (int i = 0; i < 6; i++) {
        c += kWebCodeAlphabet[rand() % (int)(sizeof(kWebCodeAlphabet) - 1)];
    }
    StoragePreferences::putString(kKeyWebCfgCode, c);
    LOGD("ConfigStore: webCfgCode generated %s", c.c_str());
    return c;
}

std::string ConfigStore::mqttPrefix() {
    return std::string("smartpanel/") + deviceId();
}

void ConfigStore::setMqttConfig(bool enabled, const std::string& server,
                                const std::string& user, const std::string& pass) {
    StoragePreferences::putBool(kKeyMqttEn, enabled);
    StoragePreferences::putString(kKeyMqttServer, server);
    StoragePreferences::putString(kKeyMqttUser, user);
    StoragePreferences::putString(kKeyMqttPass, pass);
}

// ── 屏保视频间隔 ────────────────────────────────────────────
int ConfigStore::videoIntervalSec() {
    return StoragePreferences::getInt(kKeyVideoInt, 0);
}

void ConfigStore::setVideoIntervalSec(int sec) {
    StoragePreferences::putInt(kKeyVideoInt, sec);
}

// ── 屏保图片显示时长（秒）────────────────────────────────────
int ConfigStore::photoIntervalSec() {
    int v = StoragePreferences::getInt(kKeyPhotoSec, 15);   // 设计说明书 3.4.5：图片每张 15 秒
    if (v < kPhotoSecMin) v = kPhotoSecMin;
    if (v > kPhotoSecMax) v = kPhotoSecMax;
    return v;
}

void ConfigStore::setPhotoIntervalSec(int sec) {
    if (sec < kPhotoSecMin) sec = kPhotoSecMin;
    if (sec > kPhotoSecMax) sec = kPhotoSecMax;
    StoragePreferences::putInt(kKeyPhotoSec, sec);
    LOGD("ConfigStore: photo interval -> %d s", sec);
}

// ── 屏保声音（视频音轨）────────────────────────────────────────
bool ConfigStore::ssSoundEnabled() {
    return StoragePreferences::getInt(kKeySsSound, 1) != 0;   // 默认开
}

void ConfigStore::setSsSoundEnabled(bool on) {
    StoragePreferences::putInt(kKeySsSound, on ? 1 : 0);
    LOGI("ConfigStore: ss sound -> %s", on ? "ON" : "MUTE");
}

int ConfigStore::ssVolume() {
    int v = StoragePreferences::getInt(kKeySsVol, 60);        // 默认 60
    if (v < kSsVolMin) v = kSsVolMin;
    if (v > kSsVolMax) v = kSsVolMax;
    return v;
}

void ConfigStore::setSsVolume(int v) {
    if (v < kSsVolMin) v = kSsVolMin;
    if (v > kSsVolMax) v = kSsVolMax;
    StoragePreferences::putInt(kKeySsVol, v);
    LOGI("ConfigStore: ss volume -> %d", v);
}

float ConfigStore::ssVolumeScale() {
    if (!ssSoundEnabled()) return 0.0f;
    return ssVolume() / 100.0f;
}

// ── 相册扫码二维码内容 / 小程序 AppID ────────────────────────
std::string ConfigStore::qrUrl() {
    return StoragePreferences::getString(kKeyQrUrl, kQrUrlDefault);
}

void ConfigStore::setQrUrl(const std::string& url) {
    StoragePreferences::putString(kKeyQrUrl, url);
    LOGD("ConfigStore: qrUrl -> %s", url.c_str());
}

std::string ConfigStore::mpAppId() {
    // 开源版默认空串（公司小程序 AppID 不随开源代码分发）；
    // 需要「扫码传图」小程序的部署方，自行在 prefs(kKeyMpAppId) 里配自己的 AppID。
    return StoragePreferences::getString(kKeyMpAppId, "");
}

std::string ConfigStore::qrImageUrl() {
    return StoragePreferences::getString(kKeyQrImgUrl, "");
}

void ConfigStore::setQrImageUrl(const std::string& url) {
    StoragePreferences::putString(kKeyQrImgUrl, url);
    LOGD("ConfigStore: qrImageUrl -> %s", url.c_str());
}

// ── 环境缓存 ────────────────────────────────────────────────
float ConfigStore::cachedTemp() {
    return StoragePreferences::getFloat(kKeyTemp, 25.0f);
}

float ConfigStore::cachedHum() {
    return StoragePreferences::getFloat(kKeyHum, 50.0f);
}

std::string ConfigStore::cachedWeather() {
    return StoragePreferences::getString(kKeyWeather, "未获取");
}

void ConfigStore::setCachedTempHum(float temp, float hum) {
    StoragePreferences::putFloat(kKeyTemp, temp);
    StoragePreferences::putFloat(kKeyHum, hum);
}

void ConfigStore::setCachedWeather(const std::string& text) {
    StoragePreferences::putString(kKeyWeather, text);
}

// ── 天气 / 空气质量（真实数据）────────────────────────────────
std::string ConfigStore::weatherCity() {
    return StoragePreferences::getString(kKeyWxCity, kWxCityDefault);
}

void ConfigStore::setWeatherCity(const std::string& city) {
    StoragePreferences::putString(kKeyWxCity, city);
}

std::string ConfigStore::weatherLatLon() {
    return StoragePreferences::getString(kKeyWxLatLon, kWxLatLonDefault);
}

void ConfigStore::setWeatherLatLon(const std::string& latlon) {
    StoragePreferences::putString(kKeyWxLatLon, latlon);
}

int ConfigStore::cachedWxCode() {
    return StoragePreferences::getInt(kKeyWxCode, -1);
}

void ConfigStore::setCachedWxCode(int code) {
    StoragePreferences::putInt(kKeyWxCode, code);
}

float ConfigStore::cachedPm25() {
    return StoragePreferences::getFloat(kKeyWxPm25, -1.0f);
}

float ConfigStore::cachedPm10() {
    return StoragePreferences::getFloat(kKeyWxPm10, -1.0f);
}

int ConfigStore::cachedAqi() {
    return StoragePreferences::getInt(kKeyWxAqi, -1);
}

void ConfigStore::setCachedAir(float pm25, float pm10, int aqi) {
    StoragePreferences::putFloat(kKeyWxPm25, pm25);
    StoragePreferences::putFloat(kKeyWxPm10, pm10);
    StoragePreferences::putInt(kKeyWxAqi, aqi);
}

long long ConfigStore::weatherUpdatedMs() {
    // 没有 long long 存取接口 -> 用字符串存（数值极小，无精度问题）
    const std::string s = StoragePreferences::getString(kKeyWxMs, "0");
    return (long long)atoll(s.c_str());
}

void ConfigStore::setWeatherUpdatedMs(long long ms) {
    char b[32];
    snprintf(b, sizeof(b), "%lld", ms);
    StoragePreferences::putString(kKeyWxMs, b);
}

bool ConfigStore::weatherHasData() {
    return StoragePreferences::getInt(kKeyWxOk, 0) != 0;
}

void ConfigStore::setWeatherHasData(bool on) {
    StoragePreferences::putInt(kKeyWxOk, on ? 1 : 0);
}

// ── 黄历缓存（2026-10-01）─────────────────────────────────────
//   一条记录整存一个键：字段十几个（农历/干支/宜/忌/冲煞/值神/星宿/彭祖/胎神…），
//   逐个开 prefs 键太碎，且这些字段**永远是整体更新**，没有单字段读写的需求。
//   格式 "k=v\n"（值里的换行在写入前被替换成空格，见 encode()）。
std::string ConfigStore::almanacBlob() {
    return StoragePreferences::getString(kKeyAlmBlob, "");
}

void ConfigStore::setAlmanacBlob(const std::string& blob) {
    StoragePreferences::putString(kKeyAlmBlob, blob);
}

long long ConfigStore::almanacFetchedMs() {
    const std::string s = StoragePreferences::getString(kKeyAlmMs, "0");
    return (long long)atoll(s.c_str());
}

void ConfigStore::setAlmanacFetchedMs(long long ms) {
    char b[32];
    snprintf(b, sizeof(b), "%lld", ms);
    StoragePreferences::putString(kKeyAlmMs, b);
}

bool ConfigStore::almanacHasData() {
    return StoragePreferences::getInt(kKeyAlmOk, 0) != 0;
}

void ConfigStore::setAlmanacHasData(bool on) {
    StoragePreferences::putInt(kKeyAlmOk, on ? 1 : 0);
}
