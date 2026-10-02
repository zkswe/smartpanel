// ui_common.h - 多页面共享的内联工具（每个 logic .cc 单独编译，共享状态走 inline 单例）
#pragma once

#include "control/ZKTextView.h"
#include "utils/TimeHelper.h"

#include <string>

// ── 颜色（0xRRGGBB 十进制）───────────────────────────────────
#define CLR_WHITE   16777215
#define CLR_GREEN   5422720
#define CLR_ORANGE  16754470
#define CLR_ON      4114175     // 开启高亮青蓝 #3EC6FF（参考图蓝色点缀）
// M3 粉彩 tonal accent（Nest 风格）
#define CLR_BLUE    11056122  // #A8C7FA
#define CLR_GREEN2  10915593  // #A6D189
#define CLR_ORANGE2 16618651  // #FDB99B
#define CLR_GRAY    8608392   // #837E88 次要文字
#define CLR_PURPLE  10141951    // #9b5cff
#define CLR_RED     16737948    // #ff5c5c
#define CLR_DIM     9054379     // #8a90ab 次要文字

// ── 空闲进屏保 ────────────────────────────────────────────────
#define IDLE_ENTER_MS 15000

inline long long& lastActivityMs() {
    static long long v = 0;
    return v;
}
inline long long nowMs() {
    return TimeHelper::getCurrentTime();
}
inline void noteActivity() {
    lastActivityMs() = nowMs();
}

// ── 环境视频（屏保与主页共享一路连续播放，跨页接力位置）────────
#define AMBIENT_VIDEO_LIST_FILE "/mnt/sdnand/main_video_list.txt"
inline int& ambientVideoPos() {
    static int v = 0;
    return v;
}
// 环境视频路径：取列表第一行；列表缺失时回退到已知屏保文件
#include <stdio.h>
inline std::string ambientVideoPath() {
    static std::string cached;
    if (!cached.empty()) return cached;
    char line[512];
    FILE* fp = fopen(AMBIENT_VIDEO_LIST_FILE, "r");
    if (fp != NULL) {
        if (fgets(line, sizeof(line), fp) != NULL) {
            std::string p = line;
            while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
            if (!p.empty()) cached = p;
        }
        fclose(fp);
    }
    if (cached.empty()) cached = "/mnt/sdnand/video/screensaver_1.mp4";
    return cached;
}

// ── 跨页面共享的硬件看门狗（任意页面定时器都要喂）──────────────
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "utils/Log.h"

inline int& watchdogFd() {
    static int fd = -1;
    return fd;
}
inline void openWatchdog() {
    watchdogFd() = open("/dev/watchdog", O_WRONLY);
    if (watchdogFd() >= 0) {
        LOGD("watchdog: opened /dev/watchdog, feeding every 1s");
    } else {
        LOGW("watchdog: /dev/watchdog unavailable (%d), skip feeding", errno);
    }
}
inline void feedWatchdog() {
    if (watchdogFd() >= 0) {
        if (write(watchdogFd(), "1", 1) < 0) {
            LOGW("watchdog: feed failed (%d)", errno);
        }
    }
}
inline void closeWatchdog() {
    if (watchdogFd() >= 0) {
        close(watchdogFd());
        watchdogFd() = -1;
    }
}

// ── 看门狗独立喂养线程 ────────────────────────────────────────
// 与页面/屏保壳定时器完全解耦：任何界面状态（含系统屏保 overlay 切换的
// 空窗期）都持续喂狗，避免"双方定时器都暂停"导致整机重启。
#include "system/Thread.h"
class WatchdogFeederThread : public Thread {
public:
    static void startOnce() {
        static WatchdogFeederThread s;
        if (!s.isRunning()) s.run("wdg_feed");
    }
protected:
    virtual bool threadLoop() {
        feedWatchdog();
        Thread::sleep(1000);
        return true;
    }
};
inline void startWatchdogFeeder() {
    WatchdogFeederThread::startOnce();
}

// ── 天气图标（晴/多云/阴/雾/雨/雪），suffix 区分屏保 22px 与其它尺寸资源 ──
//   素材由 tools/gen_weather_icons.js 生成（M3 主绿 #7BE0A3，22×22，透明底）。
//   历史坑：改之前工程里**根本没有** w_sun/w_cloud/w_rain/w_snow 这些文件（只有 ic_sun_g22.png），
//   所以 setBackgroundPic 一直加载失败 —— 已补齐。
inline void applyWeatherIcon(ZKTextView* icon, const std::string& w, const char* suffix) {
    if (icon == NULL) return;
    std::string base = "images/w_sun";
    if (w.find("雨") != std::string::npos || w.find("rain") != std::string::npos
        || w.find("雷") != std::string::npos) {
        base = "images/w_rain";
    } else if (w.find("雪") != std::string::npos) {
        base = "images/w_snow";
    } else if (w.find("雾") != std::string::npos || w.find("霾") != std::string::npos) {
        base = "images/w_fog";
    } else if (w.find("云") != std::string::npos || w.find("阴") != std::string::npos
               || w.find("cloud") != std::string::npos) {
        base = "images/w_cloud";
    }
    std::string pic = base + suffix + ".png";
    icon->setBackgroundPic(pic.c_str());
}

static const char* kWeekCN[] = {"日", "一", "二", "三", "四", "五", "六"};
