/*
 * ConfigStore.h -- 面板配置持久化（业务单例）
 *
 * 多 Activity 口径（钟工 2026-09-24）：页面之间不互相操作控件，跨页共享的状态放业务单例；
 * 本类只用 easyui StoragePreferences（/data 键值存储），任何页面都能通过它读写同一份数据。
 *
 * 内容：设备/名称、屏幕亮度、关屏时段、屏保项显隐+位置、情景定义、运行模式、MQTT 配置、环境缓存。
 */
#ifndef _STORAGE_CONFIG_STORE_H_
#define _STORAGE_CONFIG_STORE_H_

#include <string>

class ConfigStore {
public:
    static ConfigStore* getInstance();

    void init();

    // ── 设备 ────────────────────────────────────────────────
    std::string deviceId();                                  // 首次生成后持久化（PANEL-XXXXXXXX）
    std::string deviceName();                                // 设置页可见/可重命名，默认 = 设备 ID
    void setDeviceName(const std::string& name);

    // ── 3 路开关 ────────────────────────────────────────────
    std::string relayName(int ch);
    void setRelayName(int ch, const std::string& name);
    bool relayState(int ch);                                 // 断电记忆（bitmask）
    void setRelayState(int ch, bool on);
    static const int kMaxNameBytes = 24;                     // 8 个汉字（UTF-8）

    // ── 主界面开关显隐（按键配置子页；钟工 2026-09-29：必须记住）──
    // ch = 1..3；false = 主界面不显示该开关并自动重排（默认 true）
    bool swVisible(int ch);
    void setSwVisible(int ch, bool visible);

    // ── 屏幕亮度（0..100）──────────────────────────────────
    int workBrightness();
    void setWorkBrightness(int v);
    int screensaverBrightness();
    void setScreensaverBrightness(int v);

    // ── 关屏设置（钟工 2026-09-24 口径：整点、支持跨零点、起止相同=无效）──
    //   ⚠️ 2026-10-02：UI 只暴露「小时」，但内部保留「分钟」刻度（sp_off_*_min），
    //      使熄屏/唤醒可在分钟级判定（支持子小时窗口 + 快速复测）。UI 写入时段会清零分钟。
    static const int kOffHourMin = 0;
    static const int kOffHourMax = 23;
    static const int kOffMinMin = 0;
    static const int kOffMinMax = 59;
    bool offEnabled();
    void setOffEnabled(bool on);
    int offStartHour();                                      // 0..23
    int offEndHour();                                        // 0..23
    int offStartMin();                                       // 0..59（内部刻度，UI 不暴露；缺省 0）
    int offEndMin();                                         // 0..59
    void setOffPeriod(int startHour, int endHour);           // 同时清零分钟（UI=小时级）
    bool offPeriodValid();                                   // 起止「分钟-of-day」不同
    bool offPeriodLong();                                    // 用于行值摘要
    // 当前时刻是否落在关屏时段（含跨零点；时段无效时恒 false）。hour/min 为当前"分-of-day"。
    bool inOffPeriod(int hour, int min);

    // ── 屏保显示项（显隐 + 位置）────────────────────────────
    // item = time / date / temphum / weather / statusbar
    bool ssItemVisible(const std::string& item, bool def);
    void setSsItemVisible(const std::string& item, bool visible);
    // x=y=-100000（哨兵）表示默认位置。⚠️ 负坐标是**合法位置**（钟工 2026-10-01：
    // 屏保浮层可摆到屏外），所以调用方不能用「x<0」判未设置，要比哨兵。
    static const int kSsPosUnset = -100000;
    void ssItemPos(const std::string& item, int& x, int& y);
    void setSsItemPos(const std::string& item, int x, int y);
    void resetSsItemPos(const std::string& item);
    bool ssEditMode();                                       // 屏保位置编辑模式（进程内瞬态）
    void setSsEditMode(bool on);

    // ── 情景定义 / 运行模式 ─────────────────────────────────
    std::string sceneJson();
    void saveSceneJson(const std::string& json);

    static const int MODE_HA = 0;                            // 外接 HA / MQTT broker
    static const int MODE_MASTER = 1;                        // 本机内嵌 broker
    static const int MODE_SLAVE = 2;                         // 接入同网段主机
    int runMode();
    void setRunMode(int mode);

    std::string masterIp();                                  // 从机指向的主机 IP
    void setMasterIp(const std::string& ip);
    std::string peerName(const std::string& deviceId);
    void setPeerName(const std::string& deviceId, const std::string& name);

    // ── 外接 broker（HA 模式）配置 ───────────────────────────
    // 钟工 2026-09-30：**不再有任何硬编码默认值**（原来默认指向内网 EMQX + 内置账号）——
    //   服务器/用户名/密码都为空，由用户在「运行模式 -> HA」页扫码用手机网页填。
    bool mqttEnabled();
    std::string mqttServer();                                // 全空 = 未配置
    std::string mqttUser();
    std::string mqttPassword();
    std::string mqttPrefix();                                // smartpanel/<deviceId>
    bool mqttConfigured();                                   // server 非空
    void setMqttConfig(bool enabled, const std::string& server,
                       const std::string& user, const std::string& pass);

    // ── 板内配置网页口令码（钟工 2026-09-30）─────────────────
    // 网页只在 http://<ip>:8080/<code>/ 下可访问（二维码里带这个码），
    // 同网段的无关设备猜不到路径，避免随手被改配置。首次生成后持久化。
    std::string webCfgCode();

    // 相册扫码：二维码内容 + 小程序 AppID（钟工 2026-09-25）
    //   sp_qr_url  非空 -> 用它（扫普通链接二维码打开小程序 的链接）；
    //              空 -> 回退到本机上传地址 http://<ip>:9000/upload
    //   默认值 = **小程序传图链接**（钟工 2026-09-30：以前是直接铺一张压缩过的位图素材，
    //   128px / 37 模块量化难看；改成用二维码控件按这个链接现场生成）
    //   sp_mp_appid 小程序 AppID（仅记录/备用，不参与本机生成二维码）
    std::string qrUrl();
    void setQrUrl(const std::string& url);
    std::string mpAppId();                                   // 默认空（开源版不带公司小程序 AppID）
    // 远端小程序码图 URL（非空 -> 面板下载后显示该图，用于微信小程序码；空 -> 本地二维码）
    std::string qrImageUrl();
    void setQrImageUrl(const std::string& url);

    // ── 屏保视频轮播间隔（秒；0=连续，播完即切）─────────────
    int videoIntervalSec();
    void setVideoIntervalSec(int sec);

    // 屏保图片显示时长（秒；钟工 2026-09-25：做成可配，默认 15）
    static const int kPhotoSecMin = 3;
    static const int kPhotoSecMax = 300;
    int photoIntervalSec();
    void setPhotoIntervalSec(int sec);

    // ── 屏保声音（视频音轨；2026-10-01）──────────────────────
    //   默认「开 / 音量 60」：屏保素材是白噪音视频，开箱即有声是预期行为；
    //   屏保页右上角的喇叭键可一键静音（写的就是这个开关）。
    static const int kSsVolMin = 0;
    static const int kSsVolMax = 100;
    bool ssSoundEnabled();                                   // sp_ss_sound，默认 true
    void setSsSoundEnabled(bool on);
    int  ssVolume();                                         // sp_ss_vol，0..100，默认 60
    void setSsVolume(int v);
    // 实际播放用：关 -> 0.0，开 -> volume/100
    float ssVolumeScale();

    // ── 环境数据缓存（重启后先显示上次真实值）────────────────
    //   注意：这些键在**首次成功联网拉取之前**是空的（sp_wx_ms <= 0）。
    //   判"有没有真实数据"一律看 weatherUpdatedMs()，不要拿 temp/hum 的兜底值当数据用。
    float cachedTemp();
    float cachedHum();
    std::string cachedWeather();
    void setCachedTempHum(float temp, float hum);
    void setCachedWeather(const std::string& text);

    // ── 天气 / 空气质量（Open-Meteo 真实数据；SensorManager 写入）──
    //   位置：城市名 + 经纬度。城市名只用于显示/重新解析，实际请求走经纬度。
    std::string weatherCity();                               // 默认「深圳」
    void setWeatherCity(const std::string& city);
    std::string weatherLatLon();                             // "纬度,经度"，默认深圳福田
    void setWeatherLatLon(const std::string& latlon);
    int  cachedWxCode();                                     // WMO 天气代码；-1 = 未知
    void setCachedWxCode(int code);
    float cachedPm25();                                      // μg/m³；<0 = 无数据
    float cachedPm10();
    int   cachedAqi();                                       // US AQI；<0 = 无数据
    void setCachedAir(float pm25, float pm10, int aqi);
    long long weatherUpdatedMs();                            // 上次成功拉取的本机墙钟；<=0 = 未知
    void setWeatherUpdatedMs(long long ms);
    // 「是否已成功取到过真实数据」——**不要**用 updatedMs>0 判断：
    //   TimeHelper::getCurrentTime() 在本平台返回的是**被截断成 32 位的 epoch 毫秒**
    //   （实测为负值），所以时间戳不能当有效性判据。见 SensorManager::hasData()。
    bool weatherHasData();
    void setWeatherHasData(bool on);

    // ── 黄历缓存（屏保顶部黄历区；AlmanacManager 写入）─────────────
    //   一条记录用「k=v\n」多行格式整体存进一个键（字段十几个，逐个开键太碎）。
    //   判"有没有数据"同样**不要**看时间戳，看 almanacHasData()。
    std::string almanacBlob();                               // 空 = 从未成功过
    void setAlmanacBlob(const std::string& blob);
    long long almanacFetchedMs();                            // 上次成功拉取的本机墙钟；<=0 = 未知
    void setAlmanacFetchedMs(long long ms);
    bool almanacHasData();
    void setAlmanacHasData(bool on);

private:
    ConfigStore() = default;
    std::string key(int ch) const;
    std::string generateDeviceId();
    // 出厂默认（钟工 2026-09-30）：从 panel-defaults.json 把现场参数（MQTT broker 等）落到 prefs。
    //   代码里**不带任何内网地址**；量产/固化只需带一份 json 即可开箱就连上现场 broker。
    void applyFactoryDefaults();
    // v7.4（钟工 2026-09-28）：唯一 ID 来源 —— 芯片 ID（getDevID，取前 **8 字节**）优先，
    //   其次 wlan0 MAC，最后随机兜底；mCachedId 进程内缓存，deviceId() 绝不再返回 "unknown"
    std::string hwDeviceId();
    std::string macDeviceId();
    std::string mCachedId;
};

#endif // _STORAGE_CONFIG_STORE_H_
