/*
 * WallPlayer.h -- 多屏拼接播放内核（SimplePlayer 路线，钟工 2026-09-26 10:04 拍板）
 *
 * 参考实现：内网 `AppGroup/lib-simple-player` 的 src/player/simple_player.cpp（Z20/SSD202 分支）
 *   —— 但**不能直接引包**：fun 仓库里 simple-player@4.0.1 的 zip 是 403（私有/需登录授权，
 *      见 temp/dev/log_* 与结论），所以把等价逻辑移植进本工程。
 *
 * 两个关键手法（照抄）：
 *   1) 追赶不 seek：顺序读包，丢弃到 I 帧/SPS（`data[3]&0x1f == 2|7`），且 100ms 只送一帧；
 *   2) 墙钟 + PTS 控速：起步算"应在段内位置"，每包 `delay = (起点 + pts) - now` 后再送流。
 *
 * 与参考实现**唯一实质差异（也是本项目的关键修正）**：
 *   参考实现用各自机器的 `duration + parse_time` 当周期（`now % (duration + parse_time)`），
 *   两台一旦 duration/解析耗时不同，周期就错开 → 对齐失效。
 *   本实现改用 **WallLink 广播的统一周期 `seg_ms` + 统一 epoch**：
 *       begin  = (now - epoch) % period      // 两台同式 → 同一绝对时刻
 *       anchor = now - begin                 // 锚点是 epoch + k*period，两台重合
 *   于是"墙钟 → PTS"的映射在工程上被强制同源（不只是时间源同源）。
 *
 * 生命周期：start() 起内部线程持续送流；内容读完**不 stop/不重开**，seek 回片头、
 *   锚点 += period，直接接下一轮（0 循环缝，与 v4「每轮 stop/play」路线本质不同）。
 *
 * v3（2026-09-26 11:30，钟工）：进屏保交互——“先拉齐时间，再直接跳到该显示的位置”
 *   (1) 组内时间拉齐：主机起屏保即 WallLink::publishNow()；从机等 epoch（`sp_wall_join_to_ms`，
 *       默认 5s，超时用本机时间兜底 + WARN，绝不卡画面）；组内统一时间 gnow() = 本机墙钟 - skew
 *       （skew = 收到 epoch 的时刻 - 主机发包时刻，含单程链路延迟，实测 1~5ms）。
 *   (2) 直跳：begin = (gnow - epoch) % period -> av_seek_frame(fc, -1, begin, AVSEEK_FLAG_BACKWARD)
 *       + **avformat_flush + av_bsf_flush**（上一版不 flush 会“只出一个包就 EOF”）
 *       -> 把 begin 之前的包（≤1 个 GOP ≈ 24 包）快速送完 -> 立刻交回墙钟锁相。
 *       实测行：`wall[sp]: join begin= frame_pts= delta= took= skew= lock= prev= burst= wait= mode=`
 *       （took = 进屏保→出第一帧；delta = 锁相首帧相对 begin 的偏差，帧网格 41.67ms）。
 *   (3) 开关：prefs `sp_wall_join=1|0`（默认 1）；置 0/失联/seek 失败 -> 完全回到“追赶不 seek”老行为。
 *
 * v5（2026-09-27，钟工）：**一组多视频轮播（playlist.json v2）**
 *   "周期"不再只有单一口径：时间轴 = Σ clip.dur_ms（= WallLink::totalMs()），
 *      g = 组内时间 - epoch；pos = g mod total；clip k = 满足 cum[k] <= pos < cum[k]+dur[k]，off = pos - cum[k]。
 *   正在播第 k 个 clip -> 继续锁相；clip 变了 / 首次起播 / 失步超阈值 ->
 *      打开 c<k>/seg_<idx>.mp4 并 seek 到 off（av_seek_frame + avformat_flush + av_bsf_flush + burst 预热），
 *      再交给墙钟锁相（want = clipStartWall() + pts）。
 *   片段末尾（pts >= dur[k]）-> 直接 reopen 下一 clip 的 seg_<idx>.mp4（不 seek，几 ms，不留空档），
 *      最后一个 clip 回到第 1 个（轮末等待到轮边界，沿用 v1 做法）。
 *   **取消时长约束**：不再要求“内容时长 = 广播 seg_ms”；只在同一 clip 内各段声明时长不一致时由
 *      WallLink 侧 WARN。旧布局（无 playlist.json）= 单 clip，口径与 v1 完全一致。
 */
#pragma once

#include <string>
#include <stdint.h>

class WallPlayer {
public:
    static WallPlayer* getInstance();

    /** 异步起播（内部线程一直送流到 stop()）。已在播同一文件 -> 直接返回 true */
    bool start(const std::string& file, int x, int y, int w, int h);
    void stop();
    bool running();
    std::string currentFile();

    // —— 诊断快照（供 1s 心跳打印 / 验算相位；线程写主线程读）——
    long long periodMs();      // 本轮周期口径：playlist = Σclip.dur（total_ms），单 clip = WallLink::segMs()
    long long anchorMs();      // 当前轮起点（绝对墙钟 ms）= epoch + k*period
    long long beginMs();       // 起播时"已在段内的位置"（追赶目标）
    long long lastPtsMs();     // 最近一次按墙钟送出的帧 PTS
    long long lastWantMs();    // 该帧"应该"送的墙钟时刻 = anchor + pts
    long long lastNowMs();     // 实际送出时刻
    long long lastErrMs();     // lastNow - lastWant（>0 = 迟）
    int  cycles();             // 已完成的轮数
    int  secs();               // 标记过的"整秒 PTS"条数
    int  catchupFrames();      // 追赶阶段送出的 I/SPS 帧数
    bool locked();             // 是否已脱离追赶、进入墙钟锁相
    std::string lastError();

    // —— v5 playlist 诊断（一组多视频轮播）——
    int clipIndex();           // 当前在播的 clip 序号（0-based；单 clip 模式恒 0）
    int clipCount();           // 本节目 clip 数（单 clip 模式 = 1）
    std::string clipFile();    // 当前实际在播的文件（playlist 模式下随 clip 切换）

    // —— 音画同步（2026-10-01）——
    //   simple-player 是**异步**起播：start() 只起线程，线程里要建 H264Parser(解析 moov)
    //   + mi::H264Player(MI_VDEC_InitDev / 2MB 码流缓冲 / DISP 图层 enable / bind)，这一段
    //   要几百 ms 才有第一帧画面；而屏保音频是另一个播放器（ss_audio -> ZKMediaPlayer），
    //   立刻 play() 就会"画面还没出来声音先响"。这里给出**画面真正开始喂流的时刻**做握手：
    //   `mi::H264Player::stream()` 第一次被调用（= 解码器已经在吃第一包）记为 firstFeedMs()。
    //   上层（mainLogic）据此延后起播 mp3，音画就能对上。
    static void noteFirstFeed();   // 首次 stream() 调用点（内部只记第一次，重复调用无副作用）
    static void resetFirstFeed();  // 每次 start() 时清零
    static long long firstFeedMs();// 单调钟 ms；0 = 本次尚未开始喂流
    static long long monoMs();     // 单调钟 ms（CLOCK_MONOTONIC）——给上层算等待时长用

private:
    WallPlayer();
    ~WallPlayer();
    WallPlayer(const WallPlayer&);
    WallPlayer& operator=(const WallPlayer&);

    void run(std::string file, int x, int y, int w, int h);

    struct Impl;
    Impl* impl_;
};
