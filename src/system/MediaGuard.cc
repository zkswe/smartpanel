/*
 * MediaGuard.cc -- 素材可用性护栏的实现（设计与背景见 MediaGuard.h）
 *
 * 为什么要在播放前读容器：本机 VDEC 通道被写死成 H.264（mi_h264_player_port.cpp），
 *   喂 HEVC 进去**不抛异常、也不报错**，只是没有画面 —— 轮播照走满 10 秒，屏幕全黑。
 *   这种静默失败无法用"播放失败再兜底"抓住，只能先看 stsd 里声明的编码。
 */
#include "system/MediaGuard.h"

#include "storage/StoragePreferences.h"
#include "utils/Log.h"

#include <algorithm>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace MediaGuard {

static const char* kKeyBlocked = "sp_media_blocked";   // 改名失败时的兜底黑名单（'\n' 分隔）
static const char* kKeyRecords = "sp_media_badlog";    // 标记台账（"路径|原因|墙钟秒"，'\n' 分隔，最新在前）
static const int   kMaxRecords = 64;
static const char* kBadSuffix  = ".bad";

// ── 小工具 ────────────────────────────────────────────────────
static long long mgNowSec() {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec > 1000000000L) return (long long)ts.tv_sec;
    return 0;                                          // 未校时 -> 0（时间未知，不要瞎编）
}

static std::string lowerExt(const std::string& p) {
    const size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    std::string e = p.substr(dot);
    for (size_t i = 0; i < e.size(); i++) e[i] = (char)tolower((unsigned char)e[i]);
    return e;
}

static bool endsWith(const std::string& s, const char* suf) {
    const size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

static std::string baseName(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return (s == std::string::npos) ? p : p.substr(s + 1);
}

static std::vector<std::string> splitLines(const std::string& all) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        std::string one = all.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
        if (!one.empty()) out.push_back(one);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

static std::string joinLines(const std::vector<std::string>& v) {
    std::string all;
    for (size_t i = 0; i < v.size(); i++) { all += v[i]; all += '\n'; }
    return all;
}

// ── 扩展名口径（与 mainLogic/videoLogic 保持一致）──────────────
bool isImagePath(const std::string& p) {
    const std::string e = lowerExt(p);
    return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".bmp" || e == ".gif";
}

bool isVideoPath(const std::string& p) {
    const std::string e = lowerExt(p);
    return e == ".mp4" || e == ".avi" || e == ".mkv" || e == ".mov";
}

bool isMarkedName(const std::string& p) { return endsWith(p, kBadSuffix); }

std::vector<std::string> mediaDirs() {
    std::vector<std::string> v;
    v.push_back("/mnt/sdnand/video");
    v.push_back("/mnt/sdnand/album");
    v.push_back("/mnt/extsd/video");
    v.push_back("/mnt/usb1/video");
    return v;
}

// ── ISO-BMFF(MP4/MOV) box 走读 ────────────────────────────────
//  只读 box 头，mdat 用 size 直接跳过（fseek），所以大文件也是毫秒级。
static bool readBoxHeader(FILE* fp, long long pos, long long end, long long* next,
                          std::string* type, long long* cbeg, long long* cend) {
    if (pos + 8 > end) return false;
    if (fseek(fp, (long)pos, SEEK_SET) != 0) return false;
    unsigned char h[8];
    if (fread(h, 1, 8, fp) != 8) return false;
    unsigned long long sz = ((unsigned long long)h[0] << 24) | ((unsigned long long)h[1] << 16) |
                            ((unsigned long long)h[2] << 8) | (unsigned long long)h[3];
    long long hdr = 8;
    if (sz == 1) {                                     // largesize：真正的 64 位长度
        unsigned char e[8];
        if (fread(e, 1, 8, fp) != 8) return false;
        unsigned long long s64 = 0;
        for (int i = 0; i < 8; i++) s64 = (s64 << 8) | (unsigned long long)e[i];
        sz = s64;
        hdr = 16;
    }
    long long size = (sz == 0) ? (end - pos) : (long long)sz;   // size==0 = 直到父区间末尾
    if (size < hdr) return false;                      // 尺寸非法 -> 容器损坏
    if (pos + size > end) size = end - pos;            // 越界 -> 截到父区间（容错，不硬失败）
    char t[5];
    t[0] = (char)h[4]; t[1] = (char)h[5]; t[2] = (char)h[6]; t[3] = (char)h[7]; t[4] = 0;
    *type = std::string(t);
    *cbeg = pos + hdr;
    *cend = pos + size;
    *next = pos + size;
    return true;
}

// 在 [begin,end) 的**直属**子 box 里找第一个 type == want 的，返回其内容区间
static bool findChild(FILE* fp, long long begin, long long end, const char* want,
                      long long* cbeg, long long* cend) {
    long long p = begin;
    int guard = 0;
    while (p + 8 <= end && ++guard < 4096) {
        std::string t;
        long long nb = 0, cb = 0, ce = 0;
        if (!readBoxHeader(fp, p, end, &nb, &t, &cb, &ce)) return false;
        if (t == want) { *cbeg = cb; *cend = ce; return true; }
        if (nb <= p) return false;                     // 防死循环
        p = nb;
    }
    return false;
}

struct SampleEntry {
    std::string fourcc;
    int width;
    int height;
    SampleEntry() : width(0), height(0) {}
};

static bool isVideoFourcc(const std::string& f) {
    static const char* kV[] = { "avc1", "avc2", "avc3", "avc4", "hvc1", "hev1", "hvc2",
                                "mp4v", "m4v ", "av01", "vp09", "vp08", "s263", "h263",
                                "mjpa", "mjpb", "jpeg", "dvh1", "dvhe" };
    for (size_t i = 0; i < sizeof(kV) / sizeof(kV[0]); i++) if (f == kV[i]) return true;
    return false;
}

// MP4 sample entry 的 fourcc -> 统一编码名（不认识的保留原 fourcc 以便现场定位）
static std::string fourccToCodec(const std::string& f) {
    if (f == "avc1" || f == "avc2" || f == "avc3" || f == "avc4") return "h264";
    if (f == "hvc1" || f == "hev1" || f == "hvc2") return "hevc";
    if (f == "dvh1" || f == "dvhe") return "hevc";        // Dolby Vision 的 HEVC 基础层
    if (f == "mp4v" || f == "m4v ") return "mpeg4";
    if (f == "av01") return "av1";
    if (f == "vp09" || f == "vp08") return "vp9";
    if (f == "s263" || f == "h263") return "h263";
    if (f == "mjpa" || f == "mjpb" || f == "jpeg") return "mjpeg";
    if (f.empty()) return "other:空";
    return "other:" + f;
}

// stsd: version/flags(4) + entry_count(4) + sample entry*
//   VisualSampleEntry 的 width/height 在**自 box 起点** 32/34 字节处。
static bool parseStsd(FILE* fp, long long cbeg, long long cend, SampleEntry* out) {
    long long p = cbeg + 8;
    if (p + 8 > cend) return false;
    std::string t;
    long long nb = 0, cb = 0, ce = 0;
    if (!readBoxHeader(fp, p, cend, &nb, &t, &cb, &ce)) return false;
    out->fourcc = t;
    out->width = 0;
    out->height = 0;
    unsigned char wh[4];
    if (fseek(fp, (long)(p + 32), SEEK_SET) == 0 && fread(wh, 1, 4, fp) == 4) {
        out->width  = ((int)wh[0] << 8) | (int)wh[1];
        out->height = ((int)wh[2] << 8) | (int)wh[3];
    }
    return true;
}

// trak -> mdia -> minf -> stbl -> stsd
static bool trakStsd(FILE* fp, long long tbeg, long long tend, SampleEntry* out) {
    long long a = 0, b = 0;
    if (!findChild(fp, tbeg, tend, "mdia", &a, &b)) return false;
    if (!findChild(fp, a, b, "minf", &a, &b)) return false;
    if (!findChild(fp, a, b, "stbl", &a, &b)) return false;
    if (!findChild(fp, a, b, "stsd", &a, &b)) return false;
    return parseStsd(fp, a, b, out);
}

Probe probe(const std::string& path) {
    Probe r;
    FILE* fp = fopen(path.c_str(), "rb");
    if (fp == NULL) { r.error = "文件打不开"; return r; }
    struct stat st;
    if (fstat(fileno(fp), &st) != 0 || st.st_size < 32) {
        r.error = "文件为空或过小";
        fclose(fp);
        return r;
    }
    const long long total = (long long)st.st_size;

    // 顶层顺序走 box，找 moov（mdat 只跳不读）
    long long mbeg = -1, mend = -1;
    long long p = 0;
    int guard = 0;
    while (p + 8 <= total && ++guard < 4096) {
        std::string t;
        long long nb = 0, cb = 0, ce = 0;
        if (!readBoxHeader(fp, p, total, &nb, &t, &cb, &ce)) break;
        if (t == "moov") { mbeg = cb; mend = ce; break; }
        if (nb <= p) break;
        p = nb;
    }
    if (mbeg < 0) {
        r.error = "不是 MP4/MOV 容器（找不到 moov）";
        fclose(fp);
        return r;
    }

    // moov 里的每个 trak 取一条 stsd 记录，再挑出视频轨
    std::vector<SampleEntry> entries;
    long long q = mbeg;
    guard = 0;
    while (q + 8 <= mend && ++guard < 64) {
        std::string t;
        long long nb = 0, cb = 0, ce = 0;
        if (!readBoxHeader(fp, q, mend, &nb, &t, &cb, &ce)) break;
        if (t == "trak") {
            SampleEntry e;
            if (trakStsd(fp, cb, ce, &e)) entries.push_back(e);
        }
        if (nb <= q) break;
        q = nb;
    }
    fclose(fp);

    if (entries.empty()) {
        r.error = "容器里没有视频轨（stsd 缺失）";
        return r;
    }
    const SampleEntry* pick = NULL;
    for (size_t i = 0; i < entries.size(); i++) {
        if (isVideoFourcc(entries[i].fourcc)) { pick = &entries[i]; break; }
    }
    if (pick == NULL) pick = &entries[0];              // 全不认识 -> 用第一条，交给下面判"不支持"
    r.parsed = true;
    r.codec  = fourccToCodec(pick->fourcc);
    r.width  = pick->width;
    r.height = pick->height;
    return r;
}

std::string codecLabel(const std::string& codec) {
    if (codec == "h264")  return "H.264(AVC)";
    if (codec == "hevc")  return "H.265(HEVC)";
    if (codec == "mpeg4") return "MPEG-4";
    if (codec == "av1")   return "AV1";
    if (codec == "vp9")   return "VP9";
    if (codec == "mjpeg") return "MJPEG";
    if (codec.compare(0, 6, "other:") == 0) return "未知编码(" + codec.substr(6) + ")";
    return codec.empty() ? "未知" : codec;
}

bool codecSupported(const std::string& codec) {
    // 口径来源：src/wall/mi_h264_player_port.cpp 里
    //   attr.eCodecType = E_MI_VDEC_CODEC_TYPE_H264（写死）
    // -> 本机 MI 硬解只接受 H.264。要放开必须在那边按编码建对应通道。
    return codec == "h264";
}

std::string checkPlayable(const std::string& path) {
    if (isImagePath(path)) return std::string();       // 图片走 UI 层绘制，与解码器无关
    if (!isVideoPath(path)) return std::string();      // 非媒体扩展名（本不该进清单）
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return "文件不存在";
    if (st.st_size < 1024) return "文件过小，疑似损坏";

    const std::string e = lowerExt(path);
    if (e == ".mp4" || e == ".mov") {                  // ISO-BMFF：可精确读编码
        Probe p = probe(path);
        if (!p.parsed) return p.error.empty() ? std::string("容器无法解析") : p.error;
        if (!codecSupported(p.codec)) {
            return "编码 " + codecLabel(p.codec) + " 本机解码器不支持（仅 H.264）";
        }
        return std::string();
    }
    // .avi/.mkv 不做容器预检（解析成本高、格式杂），靠播放失败兜底
    return std::string();
}

// ── 黑名单（改名失败时的兜底）────────────────────────────────
static std::vector<std::string> blockedList() {
    return splitLines(StoragePreferences::getString(kKeyBlocked, ""));
}

static void addBlocked(const std::string& path) {
    std::vector<std::string> v = blockedList();
    for (size_t i = 0; i < v.size(); i++) if (v[i] == path) return;
    v.push_back(path);
    StoragePreferences::putString(kKeyBlocked, joinLines(v));
}

static void removeBlocked(const std::string& path) {
    std::vector<std::string> v = blockedList();
    std::vector<std::string> keep;
    for (size_t i = 0; i < v.size(); i++) if (v[i] != path) keep.push_back(v[i]);
    if (keep.size() != v.size()) StoragePreferences::putString(kKeyBlocked, joinLines(keep));
}

// ── 台账 ──────────────────────────────────────────────────────
std::vector<std::string> records() {
    return splitLines(StoragePreferences::getString(kKeyRecords, ""));
}

void clearRecords() { StoragePreferences::putString(kKeyRecords, ""); }

static void addRecord(const std::string& path, const std::string& why) {
    char ts[24];
    const long long s = mgNowSec();
    if (s > 0) snprintf(ts, sizeof(ts), "%lld", s);
    else snprintf(ts, sizeof(ts), "0");
    std::string line = path + "|" + why + "|" + ts;

    std::vector<std::string> v = records();
    std::vector<std::string> out;
    out.push_back(line);                               // 最新在前
    for (size_t i = 0; i < v.size() && (int)out.size() < kMaxRecords; i++) {
        if (v[i] == line) continue;
        // 同一路径的旧记录丢掉（只留最新一条），避免反复重试把台账塞满
        const size_t bar = v[i].find('|');
        if (bar != std::string::npos && v[i].substr(0, bar) == path) continue;
        out.push_back(v[i]);
    }
    StoragePreferences::putString(kKeyRecords, joinLines(out));
}

static void removeRecord(const std::string& path) {
    std::vector<std::string> v = records();
    std::vector<std::string> keep;
    for (size_t i = 0; i < v.size(); i++) {
        const size_t bar = v[i].find('|');
        const std::string p = (bar == std::string::npos) ? v[i] : v[i].substr(0, bar);
        if (p != path) keep.push_back(v[i]);
    }
    if (keep.size() != v.size()) StoragePreferences::putString(kKeyRecords, joinLines(keep));
}

// ── 标记 / 取消 ───────────────────────────────────────────────
bool isMarked(const std::string& path) {
    if (isMarkedName(path)) return true;
    std::vector<std::string> b = blockedList();
    for (size_t i = 0; i < b.size(); i++) if (b[i] == path) return true;
    return false;
}

bool autoMarkEnabled() {
    return StoragePreferences::getInt("sp_ss_autobad", 1) != 0;
}

bool mark(const std::string& path, const std::string& why) {
    if (path.empty() || isMarkedName(path)) return false;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        LOGW("mediaGuard: 标记跳过（源文件不存在）%s", path.c_str());
        return false;                                  // 不存在的路径不进黑名单（清单本就会丢它）
    }
    const std::string dst = path + kBadSuffix;
    bool renamed = false;
    if (rename(path.c_str(), dst.c_str()) == 0) {
        renamed = true;
    } else {
        LOGW("mediaGuard: 改名失败 %s -> %s (errno=%d %s) -> 退化为持久黑名单",
             path.c_str(), dst.c_str(), errno, strerror(errno));
        addBlocked(path);                              // 只读盘等场景：仍然不再播
    }
    addRecord(renamed ? dst : path, why);
    LOGW("mediaGuard: 标记不可播 %s（原因：%s）", (renamed ? dst : path).c_str(), why.c_str());
    return true;
}

bool unmark(const std::string& path) {
    if (path.empty()) return false;
    const bool hadSuffix = isMarkedName(path);
    const std::string src = hadSuffix ? path : (path + kBadSuffix);
    const std::string dst = hadSuffix ? path.substr(0, path.size() - strlen(kBadSuffix)) : path;

    bool ok = false;
    if (rename(src.c_str(), dst.c_str()) == 0) {
        ok = true;
        LOGI("mediaGuard: 取消标记 %s -> %s", src.c_str(), dst.c_str());
    } else {
        LOGW("mediaGuard: 取消标记改名失败 %s (errno=%d)", src.c_str(), errno);
    }
    removeBlocked(dst);                                // 无论改名成功与否都解封
    removeBlocked(src);
    removeRecord(src);
    removeRecord(dst);
    return ok;
}

std::vector<std::string> scanMarked() {
    std::vector<std::string> out;
    const std::vector<std::string> dirs = mediaDirs();
    for (size_t d = 0; d < dirs.size(); d++) {
        DIR* dp = opendir(dirs[d].c_str());
        if (dp == NULL) continue;
        std::vector<std::string> found;
        struct dirent* ent = NULL;
        while ((ent = readdir(dp)) != NULL) {
            const std::string nm = ent->d_name;
            if (nm == "." || nm == ".." || nm[0] == '.') continue;
            if (isMarkedName(nm)) found.push_back(dirs[d] + "/" + nm);
        }
        closedir(dp);
        std::sort(found.begin(), found.end());
        for (size_t i = 0; i < found.size(); i++) out.push_back(found[i]);
    }
    return out;
}

// ── 批量预检 ──────────────────────────────────────────────────
AuditResult audit(bool applyMark) {
    AuditResult r;
    const std::vector<std::string> dirs = mediaDirs();
    for (size_t d = 0; d < dirs.size(); d++) {
        DIR* dp = opendir(dirs[d].c_str());
        if (dp == NULL) continue;
        std::vector<std::string> found;
        struct dirent* ent = NULL;
        while ((ent = readdir(dp)) != NULL) {
            const std::string nm = ent->d_name;
            if (nm == "." || nm == ".." || nm[0] == '.') continue;
            const std::string full = dirs[d] + "/" + nm;
            if (isImagePath(full) || isVideoPath(full)) found.push_back(full);
        }
        closedir(dp);
        std::sort(found.begin(), found.end());
        for (size_t i = 0; i < found.size(); i++) {
            r.scanned++;
            const std::string why = checkPlayable(found[i]);
            if (why.empty()) continue;
            r.unplayable++;
            LOGW("mediaGuard: 预检不可播 %s（%s）", baseName(found[i]).c_str(), why.c_str());
            if (applyMark && mark(found[i], why)) r.marked++;
        }
    }
    return r;
}

}  // namespace MediaGuard
