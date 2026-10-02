#include "network/PanelLink.h"

#include "device/RelayManager.h"
#include "device/SensorManager.h"
#include "device/AlmanacManager.h"  // 黄历（status 里带上，便于不抓屏验收）
#include "scene/SceneManager.h"
#include "storage/ConfigStore.h"
#include "system/MediaGuard.h"      // sbad_list / sbad_clear（屏保素材标记）
#include "utils/Log.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

PanelLink* PanelLink::getInstance() {
    static PanelLink s;
    return &s;
}

void PanelLink::init(RelayManager* relay, SceneManager* scene, SensorManager* sensor) {
    if (mRunning) return;              // 幂等：重复调用不重建 socket/线程
    (void)relay;                       // 继电器状态一律读 RelayManager 单例（见 .h 说明）
    mScene = scene;
    mSensor = sensor;

    mFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (mFd < 0) {
        LOGE("PanelLink: socket failed");
        return;
    }
    int on = 1;
    setsockopt(mFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(mFd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(mFd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        LOGE("PanelLink: bind port %d failed", kPort);
        close(mFd);
        mFd = -1;
        return;
    }

    mRunning = true;
    std::thread(&PanelLink::loop, this).detach();
    // 继电器变化 -> 局域网广播（UDP event），与 MQTT 上报（MqttBridge）互不干扰
    RelayManager::getInstance()->addListener("panel_link", [](int, bool) {
        PanelLink::getInstance()->onStateChanged();
    });
    LOGD("PanelLink: UDP API listening on %d", kPort);
}

void PanelLink::stop() {
    mRunning = false;
    if (mFd >= 0) {
        close(mFd);
        mFd = -1;
    }
}

void PanelLink::loop() {
    char buf[2048];
    struct sockaddr_in from;
    socklen_t fromLen = sizeof(from);
    while (mRunning && mFd >= 0) {
        ssize_t n = recvfrom(mFd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr*)&from, &fromLen);
        if (n <= 0) continue;
        buf[n] = '\0';
        handlePacket(std::string(buf, n), from);
    }
}

std::string PanelLink::statusJson() {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);

    std::string id = ConfigStore::getInstance()->deviceId();
    std::string ip = localIp();

    // 继电器状态的唯一事实源 = RelayManager 单例。
    // 不再用 init() 注入的可空指针 + "? : false" 兜底成 false（那是 2026-09-27 HA 状态
    // 自相矛盾的直接原因）：未初始化就显式 LOGE，而不是静默地报一屏 false。
    RelayManager* rm = RelayManager::getInstance();
    if (!rm->initialized()) {
        LOGE("PanelLink: statusJson() 在 RelayManager::init() 之前被调用，relays 可能不准");
    }

    w.StartObject();
    w.Key("dev");    w.String("SmartHomePanel");
    w.Key("id");     w.String(id.c_str());
    w.Key("ip");     w.String(ip.c_str());
    w.Key("model");  w.String("SW48480040D1/Z20");
    w.Key("relays");
    w.StartArray();
    for (int i = 1; i <= RelayManager::kChannels; ++i) {
        w.Bool(rm->get(i));
    }
    w.EndArray();
    w.Key("relay_names");
    w.StartArray();
    for (int i = 1; i <= RelayManager::kChannels; ++i) {
        std::string name = ConfigStore::getInstance()->relayName(i);
        w.String(name.c_str());
    }
    w.EndArray();
    w.Key("scene");
    w.String(mScene ? mScene->currentScene().c_str() : "");
    if (mSensor) {
        w.Key("temp"); w.Double(mSensor->temp());
        w.Key("hum");  w.Double(mSensor->hum());
        w.Key("weather"); w.String(mSensor->weather().c_str());
        // 真实数据标记（2026-10-01）：没有拉到过数据时 has_data=false，
        // 消费方（小程序/网关）据此区分"真实 0"和"还没取到"。
        w.Key("has_data"); w.Bool(mSensor->hasData());
        w.Key("wx_code"); w.Int(mSensor->wxCode());
        w.Key("pm25"); w.Double(mSensor->pm25());
        w.Key("pm10"); w.Double(mSensor->pm10());
        w.Key("aqi");  w.Int(mSensor->aqi());
        w.Key("aqi_level"); w.String(mSensor->aqiLevel().c_str());
        w.Key("city"); w.String(mSensor->city().c_str());
        w.Key("updated_ms"); w.Int64(mSensor->updatedMs());
    }
    // 黄历（2026-10-01）：屏保顶部那块。直接用单例（不必走 init() 注入），
    //   带三行成品文案 —— 消费方不用重算排版，也方便"不抓屏"验收。
    {
        AlmanacManager* am = AlmanacManager::getInstance();
        w.Key("almanac");
        w.StartObject();
        w.Key("has_data");   w.Bool(am->hasData());
        w.Key("source");     w.Int(am->source());          // 1=36jxs 2=apihz 0=无
        w.Key("line1");      w.String(am->line1().c_str());
        w.Key("line2");      w.String(am->line2().c_str());
        w.Key("line3");      w.String(am->line3().c_str());
        w.Key("updated_ms"); w.Int64(am->updatedMs());
        w.EndObject();
    }
    // 屏保素材：被标记为「不可播」的数量（>0 说明有素材被自动改名成 *.bad 并移出轮播）
    {
        std::vector<std::string> bad = MediaGuard::scanMarked();
        w.Key("ss_bad"); w.Int((int)bad.size());
    }
    w.Key("port"); w.Int(kPort);
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

void PanelLink::broadcast(const std::string& json) {
    if (mFd < 0) return;
    struct sockaddr_in bcast;
    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(kPort);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(mFd, json.c_str(), json.size(), 0,
           (struct sockaddr*)&bcast, sizeof(bcast));
}

void PanelLink::replyTo(const struct sockaddr_in& to, const std::string& json) {
    if (mFd < 0) return;
    sendto(mFd, json.c_str(), json.size(), 0,
           (struct sockaddr*)&to, sizeof(to));
}

void PanelLink::onStateChanged() {
    // 状态变化 -> 广播（150ms 内合并，避免连点风暴）
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    std::string id = ConfigStore::getInstance()->deviceId();
    w.StartObject();
    w.Key("event"); w.String("status");
    w.Key("id");    w.String(id.c_str());
    w.Key("relays");
    w.StartArray();
    RelayManager* rm = RelayManager::getInstance();
    for (int i = 1; i <= RelayManager::kChannels; ++i) {
        w.Bool(rm->get(i));
    }
    w.EndArray();
    w.Key("scene"); w.String(mScene ? mScene->currentScene().c_str() : "");
    w.EndObject();
    broadcast(std::string(sb.GetString(), sb.GetSize()));
}

void PanelLink::handlePacket(const std::string& payload, const struct sockaddr_in& from) {
    rapidjson::Document doc;
    if (doc.Parse(payload.c_str()).HasParseError()) return;
    if (!doc.IsObject()) return;

    // 联动事件（其他面板广播的）-> 执行同名情景，不再二次广播，避免回环
    if (doc.HasMember("event") && doc["event"].IsString()
        && std::string(doc["event"].GetString()) == "scene") {
        if (!doc.HasMember("name") || !doc["name"].IsString()) return;
        std::string name = doc["name"].GetString();
        bool states[RelayManager::kChannels] = {false, false, false};
        bool* pStates = nullptr;
        if (doc.HasMember("relays") && doc["relays"].IsArray()) {
            auto& arr = doc["relays"];
            for (rapidjson::SizeType i = 0; i < arr.Size() && i < (rapidjson::SizeType)RelayManager::kChannels; ++i) {
                if (arr[i].IsBool()) states[i] = arr[i].GetBool();
            }
            pStates = states;
        }
        // 忽略自己广播回环
        if (doc.HasMember("from") && doc["from"].IsString()
            && std::string(doc["from"].GetString()) == ConfigStore::getInstance()->deviceId()) {
            return;
        }
        if (mScene) mScene->activate(name, true, pStates);
        return;
    }

    if (!doc.HasMember("cmd") || !doc["cmd"].IsString()) return;

    std::string cmd = doc["cmd"].GetString();

    if (cmd == "discover") {
        replyTo(from, statusJson());
        return;
    }

    if (cmd == "status") {
        replyTo(from, statusJson());
        return;
    }

    if (cmd == "set") {
        if (!doc.HasMember("relay") || !doc["relay"].IsInt()) return;
        if (!doc.HasMember("on") || !doc["on"].IsBool()) return;
        int ch = doc["relay"].GetInt();
        if (RelayManager::getInstance()->set(ch, doc["on"].GetBool())) {
            replyTo(from, statusJson());   // 应答携带最新状态
        }
        return;
    }

    if (cmd == "scene") {
        // 本机执行 + 向全网广播联动事件（多设备同情景联动）
        if (!doc.HasMember("name") || !doc["name"].IsString()) return;
        std::string name = doc["name"].GetString();
        if (mScene && mScene->activate(name, false)) {
            replyTo(from, statusJson());
        }
        return;
    }

    if (cmd == "define_scene") {
        if (!doc.HasMember("name") || !doc["name"].IsString()) return;
        if (!doc.HasMember("relays") || !doc["relays"].IsArray()
            || doc["relays"].Size() != (rapidjson::SizeType)RelayManager::kChannels) return;
        std::string name = doc["name"].GetString();
        std::string desc = (doc.HasMember("desc") && doc["desc"].IsString())
                           ? doc["desc"].GetString() : "";
        bool relays[RelayManager::kChannels];
        for (int i = 0; i < RelayManager::kChannels; ++i) {
            relays[i] = doc["relays"][(rapidjson::SizeType)i].GetBool();
        }
        if (mScene) {
            mScene->defineScene(name, relays, desc);
            replyTo(from, statusJson());
        }
        return;
    }

    if (cmd == "weather") {
        if (!doc.HasMember("text") || !doc["text"].IsString()) return;
        if (mSensor) mSensor->setWeather(doc["text"].GetString());
        replyTo(from, statusJson());
        return;
    }

    // 配置天气位置（2026-10-01）：{"cmd":"config_weather","city":"杭州"}
    //                    或 {"cmd":"config_weather","lat":"22.5431","lon":"114.0579"}
    // 传 city 时走 geocoding 解析经纬度（会发一次 HTTPS 请求，可能阻塞 ~1s）。
    if (cmd == "config_weather") {
        if (!mSensor) return;
        if (doc.HasMember("lat") && doc.HasMember("lon")) {
            char ll[64];
            snprintf(ll, sizeof(ll), "%.4f,%.4f",
                     doc["lat"].GetDouble(), doc["lon"].GetDouble());
            const std::string name = (doc.HasMember("city") && doc["city"].IsString())
                                     ? doc["city"].GetString() : "";
            mSensor->setLocation(name, ll);
        } else if (doc.HasMember("city") && doc["city"].IsString()) {
            if (!mSensor->setLocationByCity(doc["city"].GetString())) {
                LOGW("PanelLink: config_weather city '%s' not resolved", doc["city"].GetString());
            }
        } else {
            return;      // 两个都没给 -> 忽略
        }
        replyTo(from, statusJson());
        return;
    }

    if (cmd == "sensor") {
        if (!doc.HasMember("temp") || !doc.HasMember("hum")) return;
        if (mSensor) {
            mSensor->setTempHum((float)doc["temp"].GetDouble(),
                                (float)doc["hum"].GetDouble());
        }
        replyTo(from, statusJson());
        return;
    }

    if (cmd == "rename") {
        if (!doc.HasMember("relay") || !doc["relay"].IsInt()) return;
        if (!doc.HasMember("name") || !doc["name"].IsString()) return;
        ConfigStore::getInstance()->setRelayName(doc["relay"].GetInt(),
                                                 doc["name"].GetString());
        replyTo(from, statusJson());
        return;
    }

    // ── 屏保素材：已标记「不可播」的清单 / 一键取消标记（钟工 2026-10-01）──────
    //   标记形态 = 文件改名 <name>.<ext>.bad；这里给现场一个不改文件系统的查询与回退入口。
    if (cmd == "ssbad_list") {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        std::vector<std::string> marked = MediaGuard::scanMarked();
        std::vector<std::string> recs   = MediaGuard::records();
        w.StartObject();
        w.Key("ok"); w.Bool(true);
        w.Key("count"); w.Int((int)marked.size());
        w.Key("marked");
        w.StartArray();
        for (size_t i = 0; i < marked.size(); ++i) w.String(marked[i].c_str());
        w.EndArray();
        // 记录格式："<路径>|<原因>|<墙钟秒>"（0 = 未校时）
        w.Key("records");
        w.StartArray();
        for (size_t i = 0; i < recs.size(); ++i) w.String(recs[i].c_str());
        w.EndArray();
        w.EndObject();
        replyTo(from, std::string(sb.GetString(), sb.GetSize()));
        return;
    }

    if (cmd == "ssbad_clear") {
        std::vector<std::string> marked = MediaGuard::scanMarked();
        int ok = 0;
        for (size_t i = 0; i < marked.size(); ++i) {
            if (MediaGuard::unmark(marked[i])) ok++;
        }
        // 也清掉改名失败时留下的兜底黑名单
        std::vector<std::string> recs = MediaGuard::records();
        for (size_t i = 0; i < recs.size(); ++i) {
            const size_t bar = recs[i].find('|');
            if (bar != std::string::npos) MediaGuard::unmark(recs[i].substr(0, bar));
        }
        MediaGuard::clearRecords();
        LOGI("PanelLink: sbad_clear 取消标记 %d/%d", ok, (int)marked.size());
        replyTo(from, statusJson());
        return;
    }
}

std::string PanelLink::localIp() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "";
    const char* ifs[] = {"wlan0", "eth0"};
    for (size_t i = 0; i < sizeof(ifs) / sizeof(ifs[0]); ++i) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strcpy(ifr.ifr_name, ifs[i]);
        if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
            std::string ip = inet_ntoa(((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr);
            close(fd);
            return ip;
        }
    }
    close(fd);
    return "";
}
