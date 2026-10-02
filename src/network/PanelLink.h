/*
 * PanelLink.h
 *
 * 局域网外部控制 API（UDP 广播，JSON 协议，端口 5050）。
 *
 * 为什么用 UDP 广播而不是 MQTT Server：
 *   设备端没有 broker 包，跑 MQTT Server 不现实；UDP 广播零依赖、
 *   小程序/其他设备无需配对即可发现与控制，同一局域网多台面板天然互通。
 *   MQTT 客户端（连部署方内网 broker）作为预留扩展在 MqttBridge 中实现。
 *
 * 协议（全部 UTF-8 JSON，端口 5050）：
 *   发现:   {"cmd":"discover"}            -> 单播应答设备信息
 *   状态:   {"cmd":"status"}              -> 单播应答全量状态
 *   开关:   {"cmd":"set","relay":1,"on":true}
 *   情景:   {"cmd":"scene","name":"away"} -> 本机执行并广播联动
 *   定义情景: {"cmd":"define_scene","name":"movie","relays":[0,0,1],"desc":"..."}
 *   天气推送: {"cmd":"weather","text":"晴 26C"}        （小程序/网关推送；会关闭自动拉取的文案覆盖）
 *   配置位置: {"cmd":"config_weather","city":"杭州"}    （走 geocoding 解析经纬度，改完立即重拉）
 *             或 {"cmd":"config_weather","lat":22.5431,"lon":114.0579,"city":"深圳"}
 *   传感器: {"cmd":"sensor","temp":26.5,"hum":55}
 *   改名:   {"cmd":"rename","relay":2,"name":"走廊灯"}
 *   屏保坏素材: {"cmd":"ssbad_list"}      -> {"ok":true,"count":N,"marked":[...],"records":[...]}
 *               {"cmd":"ssbad_clear"}     -> 取消全部素材标记（改名还原）并重扫
 *               records 每条形如 "<路径>|<原因>|<墙钟秒>"（秒为 0 = 设备尚未校时）
 *
 * status 里的环境字段（2026-10-01 起为真实数据，来源 Open-Meteo）：
 *   temp / hum / weather / wx_code / pm25 / pm10 / aqi / aqi_level / city /
 *   has_data（是否已成功拉到过）/ updated_ms（上次成功时刻）
 *   ss_bad（屏保素材里被标记为"不可播"的数量，>0 说明有素材被自动改名成 *.bad）
 *
 * 主动广播（255.255.255.255:5050）：
 *   状态变化: {"event":"status","id":"PANEL-XXXX","relays":[1,0,1],"scene":"home"}
 *   情景联动: {"event":"scene","from":"PANEL-XXXX","name":"away","relays":[0,0,0]}
 *   其他面板收到 scene 事件后按同名情景执行 -> 多设备联动。
 */
#pragma once

#include <functional>
#include <string>
#include <netinet/in.h>

class RelayManager;
class SceneManager;
class SensorManager;

class PanelLink {
public:
    static const int kPort = 5050;

    static PanelLink* getInstance();

    // 启动 UDP 监听线程（幂等：重复调用直接返回）。
    // scene/sensor 用于应答里附上情景与传感值；
    // **继电器状态一律读 RelayManager 单例**（唯一事实源），relay 参数仅作兼容保留。
    // 钟工 2026-09-27 事故：本函数当时全工程无调用点，下面又用可空 mRelay 兜底成 false
    // -> statusJson 的 relays 恒为 [false,false,false]。
    void init(RelayManager* relay, SceneManager* scene, SensorManager* sensor);

    void stop();

    // 全量状态 JSON（status 应答与广播共用）
    std::string statusJson();

    // 广播到局域网（状态/情景联动）
    void broadcast(const std::string& json);

    // 状态变化时由上层调用 -> 广播 + 通知 MQTT 桥
    void onStateChanged();

    // 本机 IP（wlan0 优先，其次 eth0；未联网返回空串）
    static std::string localIp();

private:
    PanelLink() = default;
    void loop();
    void handlePacket(const std::string& payload, const struct sockaddr_in& from);
    void replyTo(const struct sockaddr_in& to, const std::string& json);

    int mFd = -1;
    bool mRunning = false;
    SceneManager* mScene = nullptr;
    SensorManager* mSensor = nullptr;
    long long mLastBroadcastMs = 0;
};
