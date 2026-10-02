/*
 * ss_audio.cc -- 屏保音频播放器（EasyUI ZKMediaPlayer，AUDIO 类型）
 *
 * 走的是 EasyUI 自己的媒体栈：ZKMediaPlayer -> InternalMediaPlayer -> dlopen("libzkmedia.so")
 *   -> media::PlayerFactory::create(E_MEDIA_TYPE_AUDIO) -> media::ZKAudioPlayer
 * 本固件 libzkmedia.so 只注册了音频播放器（Mp3/Wav 解析器），所以这条链**只出声、不出画**；
 * 画面交给 SimplePlayer/MI（见 ss_audio.h 的方案说明）。
 *
 * 真机符号核对（2026-10-01，tools/so_syms.js）：
 *   /lib/libeasyui.so   导出 ZKMediaPlayer(C1ENS_10EMediaTypeE) / play / stop / setVolume / isPlaying
 *   /lib/libzkmedia.so  导出 media::ZKAudioPlayer::play/stop/setVolume/isPlaying + Mp3AudioParser
 *   → 两个符号都在设备上，可直接链接使用。
 */
#include "logic/ss_audio.h"

#include "media/ZKMediaPlayer.h"        // EasyUI 公开媒体播放类（EMediaType = AUDIO/VIDEO）
#include "storage/ConfigStore.h"
#include "utils/Log.h"

#include <sys/stat.h>
#include <string>

namespace {

ZKMediaPlayer* sPlayer = NULL;          // 音频播放器（进程内单例，懒创建）
std::string    sCurTrack;               // 当前音轨路径（同曲不重开）

std::string stripExt(const std::string& p) {
    const size_t dot = p.rfind('.');
    const size_t slash = p.rfind('/');
    if (dot == std::string::npos) return p;
    if (slash != std::string::npos && dot < slash) return p;   // "." 在目录名里
    return p.substr(0, dot);
}

bool fileExists(const std::string& p) {
    struct stat st;
    return (stat(p.c_str(), &st) == 0) && S_ISREG(st.st_mode);
}

}   // namespace

namespace ss_audio {

std::string trackOf(const std::string& videoPath) {
    return stripExt(videoPath) + ".mp3";
}

void init() {
    if (sPlayer != NULL) return;
    sPlayer = new ZKMediaPlayer(ZKMediaPlayer::E_MEDIA_TYPE_AUDIO);
    LOGI("ss[audio]: ZKMediaPlayer(AUDIO) created %p", (void*)sPlayer);
}

bool hasTrack(const std::string& videoPath) {
    return fileExists(trackOf(videoPath));
}

void applyVolume() {
    if (sPlayer == NULL) return;
    const float v = ConfigStore::getInstance()->ssVolumeScale();   // 关=0.0，开=vol/100
    sPlayer->setVolume(v);
    LOGD("ss[audio]: volume -> %.2f", v);
}

void playForVideo(const std::string& videoPath) {
    init();
    if (sPlayer == NULL) return;

    const std::string track = trackOf(videoPath);
    if (!fileExists(track)) {                    // 没有同名 mp3：静音播放，不影响画面
        if (!sCurTrack.empty()) stop("no-track");
        return;
    }
    if (track == sCurTrack && sPlayer->isPlaying()) return;        // 同一轨在播，别重开

    sPlayer->stop();
    sCurTrack = track;
    applyVolume();                               // 先给音量（部分实现 play 前不吃）
    sPlayer->play(track.c_str(), 0);
    applyVolume();                               // play 后再给一次（与 ZKVideoView 同款处理）
    LOGD("ss[audio]: play %s (vol=%.2f)", track.c_str(),
         ConfigStore::getInstance()->ssVolumeScale());
}

void restartForVideo(const std::string& videoPath) {
    // 「本条自我循环」的重对齐：**硬重开**。上一轮的 mp3 常在这时刚好放完、或播放器的
    // isPlaying() 还滞后地报 true —— 走 playForVideo 会被"同轨在播"短路掉，于是那一轮没声音
    // （2026-10-01 实测：重启间隔在 10.4s / 20.8s 之间跳）。先 stop 清掉 sCurTrack 再起播。
    stop("loop-resync");
    playForVideo(videoPath);
}

void stop(const char* why) {
    if (sPlayer == NULL) { sCurTrack.clear(); return; }
    const bool was = (sPlayer->isPlaying() || !sCurTrack.empty());
    if (was) LOGD("ss[audio]: stop (%s) track=%s playing=%d", why, sCurTrack.c_str(),
                  sPlayer->isPlaying() ? 1 : 0);
    sPlayer->stop();
    sCurTrack.clear();
}

bool isPlaying() {
    return (sPlayer != NULL) && sPlayer->isPlaying();
}

}   // namespace ss_audio
