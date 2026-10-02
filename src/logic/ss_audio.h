/*
 * ss_audio.h -- 屏保音频（独立于视频内核）
 *
 * 方案（钟工 2026-10-01 定）：**两条路各管一段，互不耦合**
 *   · 视频帧：SimplePlayer / MI 硬解（src/wall/WallPlayer，WallLink 拼接与屏保共用）
 *   · 音频：  EasyUI 的 ZKMediaPlayer(E_MEDIA_TYPE_AUDIO) —— 见 ss_audio.cc
 *
 * 为什么要拆开：
 *   1) SimplePlayer 包**只有视频、完全没有音频代码**（env 只有 LOG/SYNC/ROTATION），
 *      屏保走它以后一点声音都没有；
 *   2) EasyUI 的媒体栈 dlopen 本固件 `/lib/libzkmedia.so`（34KB）——它**只实现了音频**
 *      （`media::ZKAudioPlayer` + `Mp3AudioParser`/`WavAudioParser`，无任何视频符号），
 *      所以 ZKMediaPlayer **只能出声音，放不了视频**；反过来 SimplePlayer 只出画面。
 *      → 两个内核各补对方的短板，这正是拆开的理由。
 *   3) ⚠️ 因此音轨**只认 mp3 / wav**，mp4 里的 AAC 它解不了 →
 *      约定：与视频**同名同目录**的 `.mp3`（`X.mp4` ↔ `X.mp3`），
 *      没有 mp3 就静音播放（不报错、不影响画面）。
 */
#ifndef _LOGIC_SS_AUDIO_H_
#define _LOGIC_SS_AUDIO_H_

#include <string>

namespace ss_audio {

// 懒创建播放器（幂等；没 mp3 素材时也不会白占资源）
void init();

// 为某个视频起播同名 mp3（同一个轨正在播则不动；无 mp3 -> 静音）
void playForVideo(const std::string& videoPath);

// 强制重开同名 mp3（先 stop 再 play，**忽略"同轨在播就不动"的短路**）。
//   只给「本条素材自我循环」的重对齐用（单素材循环 / 被轮播间隔按住）：
//   视频循环 ~10.4s、同名 mp3 10.057s，两者会慢慢错相；当某一轮边界**早于** mp3 结束时，
//   playForVideo 的短路会让这一轮不重开 -> 听到的是"隔一轮没声音"（2026-10-01 实测）。
//   循环重对齐的语义本来就是"丢掉上一轮的尾巴、跟新一轮一起从头"，所以这里必须硬重开。
void restartForVideo(const std::string& videoPath);

// 停（换素材 / 图片 / 熄屏 / 编辑态 / 离开屏保 都走它）
void stop(const char* why);

// 按 ConfigStore 的开关+音量下发（静音键、音量页保存后即时生效）
void applyVolume();

bool isPlaying();

// 同名 mp3 是否存在（决定屏保喇叭键显不显示）
bool hasTrack(const std::string& videoPath);

// 由视频路径推同名音轨路径（供日志/排查用）
std::string trackOf(const std::string& videoPath);

}   // namespace ss_audio

#endif  /* _LOGIC_SS_AUDIO_H_ */
