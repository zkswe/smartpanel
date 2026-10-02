/*
 * MediaGuard.h -- 素材可用性护栏（钟工 2026-10-01）
 *
 * 背景：本机屏保视频走 MI 硬解内核（WallPlayer + simple-player 包），而
 *   `src/wall/mi_h264_player_port.cpp` 把 VDEC 通道写死为
 *   `attr.eCodecType = E_MI_VDEC_CODEC_TYPE_H264`
 *   —— 也就是说**本机只能解 H.264**。喂 H.265(HEVC)/MPEG-4/AV1 进去不会抛异常、
 *   也不会报错，只是解不出画面：轮播照走 10 秒，屏幕上什么都没有。这种"静默失败"
 *   靠"播放报错再兜底"是抓不到的，必须在**播放之前**读容器头判断编码。
 *
 * 两类不可播都要处理：
 *   ① 预检失败（编码不支持 / 容器损坏 / 空文件）—— 扫描清单时就判定，不必先黑屏 10 秒；
 *   ② 播放失败（SimplePlayer::play() 抛异常）—— 按文件累计次数，达阈值再标记。
 *
 * 标记 = 把文件改名 `<name>.<ext>.bad`：
 *   - 立刻从扫描清单里消失（扩展名不再匹配 .mp4/.avi/…）→「以后不要播放」；
 *   - 在文件浏览器里一眼可见，也可被视频设置页列出来（点击即还原）；
 *   - 落在数据分区，**掉电不丢**；
 *   - 若改名失败（只读挂载），退化成 StoragePreferences 里的持久黑名单。
 */
#pragma once

#include <string>
#include <vector>

namespace MediaGuard {

// ── 容器探测结果 ────────────────────────────────────────────────
struct Probe {
    bool        parsed;      // 容器头是否解析成功
    std::string codec;       // "h264" / "hevc" / "mpeg4" / "av1" / "vp9" / "other:xxxx" / ""
    int         width;       // 视频轨宽（0 = 未知）
    int         height;      // 视频轨高
    std::string error;       // parsed == false 时的原因（中文短句，进日志与标记记录）

    Probe() : parsed(false), width(0), height(0) {}
};

// 读 ISO-BMFF(MP4/MOV) 容器头：走到 moov/trak/mdia/minf/stbl/stsd，取 sample entry 的
// fourcc 与宽高。全程只读 box 头 + fseek 跳过 mdat，不读码流数据（大文件也很快）。
Probe probe(const std::string& path);

// 本机解码器是否支持该编码（当前口径：只支持 h264）
bool codecSupported(const std::string& codec);

// 编码名 -> 给人看的短名（"hevc" -> "H.265(HEVC)"）
std::string codecLabel(const std::string& codec);

// 综合判定：返回空串 = 可以播；否则返回不可播原因（中文，可直接进日志/记录）
std::string checkPlayable(const std::string& path);

// ── 标记 ───────────────────────────────────────────────────────
// 标记为不可播：改名 <path>.bad（成功时 record 里留一条"原因+时间"）；
// 改名失败则写入持久黑名单（仍然不再播）。返回 true 表示"已生效（不会再播）"。
bool mark(const std::string& path, const std::string& why);

// 是否已被标记：路径本身带 .bad，或命中持久黑名单
bool isMarked(const std::string& path);

// 取消标记：<path>.bad -> <path>（也清黑名单）。path 可以是带 .bad 的或原路径。
bool unmark(const std::string& path);

// 扫所有媒体目录，列出已标记的文件（*.bad）
std::vector<std::string> scanMarked();

// 标记记录（每条形如 "<路径>|<原因>|<墙钟秒>"，最新在前），供 UDP 查询/现场对账
std::vector<std::string> records();
void clearRecords();

// 批量预检：扫一遍所有媒体目录，逐个判定可播性。
//   applyMark = false 时只统计不动文件（`sp_ss_autobad=0` 的"只跳过不改名"模式）。
struct AuditResult {
    int scanned;      // 检查过的媒体文件数
    int unplayable;   // 判定不可播的数量
    int marked;       // 实际标记（改名/进黑名单）的数量
    AuditResult() : scanned(0), unplayable(0), marked(0) {}
};
AuditResult audit(bool applyMark);

// 是否允许自动标记（prefs `sp_ss_autobad`，默认 1）。关掉后仍会跳过不可播项，但不动文件。
bool autoMarkEnabled();

// 媒体目录（与 mainLogic / videoLogic 的扫描口径保持一致）
std::vector<std::string> mediaDirs();

// 扩展名判定（与 mainLogic 的 isImagePath/isVideoPath 同口径；这里额外把 .bad 也算"媒体"）
bool isImagePath(const std::string& p);
bool isVideoPath(const std::string& p);
bool isMarkedName(const std::string& p);     // 以 .bad 结尾

}  // namespace MediaGuard
