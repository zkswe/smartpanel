/*
 * mi_h264_player_port.cpp -- 把依赖包 mi-module@5.0.2 里"预编译固化"的 mi::H264Player
 * 以**源码移植**方式编进本工程，从而能改 VDEC 通道属性。
 *
 * 背景（2026-09-27 钟工拍板）：
 *   依赖包 registry/public/z20/mi-module/5.0.2/lib/libmi-module.a 里的 h264_player.o 把解码
 *   通道属性写死为  u32BufSize = 512*1024 / u32RefFrameNum = 1 / eDpbBufMode = INPLACE_ONE_BUF
 *   （反汇编实证：`mov r3, #524288` @0x75c、`mov r3, #1` @0x750/0x758/0x77c；
 *     与真机 /proc/mi_modules/mi_vdec/mi_vdec0 chn1 实测 524288/1/INPLACE1 完全吻合）。
 *   而**同一个包里的源码版**（SSTAR 通用版，F133 侧 `ZKMEDIA_H264_VBVSIZE=2097152`）用的是
 *     BufSize 2MB / RefFrmNum 16 / DpbBufMode NORMAL
 *   → RefFrmNum=1 + INPLACE_ONE_BUF 意味着解码器只有 1 帧参考：P 帧参考错乱 → 块状花屏
 *     （实测设备帧块度 1.53~1.81，源帧基准 1.05~1.25；改成 16+NORMAL+2MB 后 1.08~1.17）。
 *
 *   ⚠️ 2026-09-27 反汇编更正（钟工 09:55 指示，证据 `temp/dev/zkmedia_disasm/`）：
 *     **Z20 上 zkmedia 不建 MI VDEC 通道**（`libzkmedia.so`=客户端 → socket → `/bin/ssdvideoplayer`
 *     = FFmpeg 软解 + MI_SYS/DIVP/DISP/GFX 上屏，全文件 MI_VDEC 零导入）。所以"对齐 zkmedia 口径"
 *     这种说法不成立 —— 那个曾被当作参照的 chn0（2MB/16/NORMAL）最可能是**我们自己更早实例泄漏的通道**。
 *     本补丁的正当性表述 = **厂商 SSTAR 通用版源码口径（16+NORMAL）+ 2MB 下限（F133 先例）+ 现场块度实证**。
 *     `UseCusPolicy/ErrMBPercentThreshold` **不随 `DpbBufMode` 变**（10:20 1.108 实测：NORMAL 下仍是 1/0），
 *     本 SDK 也没 API 设它 → 保持 1/0；chn0 的 0/30 是创建方显式写的，与花屏无关。
 *
 * 做法：本工程自带同名同签名的 mi::H264Player 定义 —— 链接期本工程的 .o 先满足符号，
 *       静态库里的 h264_player.o 就不会被拉进 libzkgui.so（`-Wl,--start-group` 也只看符号有无）。
 *       其余 mi::VDEC / mi::DISP / mi::BindChain 仍用包内实现（本文件只改通道属性口径）。
 *
 * 与包内实现的一致性说明（逐条比对过包内 .a 的反汇编）：
 *   - DISP 输入口：stDispWin = conf.out_*；u16SrcWidth/Height = ALIGN_BACK(in_w/in_h, 32)（480 → 480）
 *   - VDEC：静态共享实例 + 引用计数（count_/vdec_/vdec_mutex_），够 1 个播放器用
 *   - stream()：MI_VDEC_VideoStream_t{pu8Addr,len,0,0,0} → sendStream(stream, 0)
 *   - setOutputPortAttr(out_w, out_h)、setDepth(1, 2)、bind(vdec→disp, in_fps, out_fps, FRAME_BASE)
 * 唯一有意改动 = 通道属性三项（BufSize / RefFrmNum / DpbBufMode），其余保持包内口径。
 * 另：包内还有个旋转渲染线程（rotate != 0 时才起）用 getFrame() 取帧，本工程拼接不需要旋转，
 *     getFrame() 只留一个会报错的桩（没有任何调用点，不影响播放）。
 */
#ifdef __PLATFORM_Z20__

#include <mi/case/h264_player.h>
#include <mi/case/attr.h>

#include <cstring>

#include "wall/WallPlayer.h"        // 音画同步：第一包喂流时刻的通知点

namespace mi {

int H264Player::count_ = 0;
std::mutex H264Player::vdec_mutex_;
std::shared_ptr<VDEC> H264Player::vdec_;

#define ALIGN_BACK(x, a)        (((x) / (a)) * (a))

/* 参考口径：厂商 SSTAR 通用版源码（16 + NORMAL）+ 2MB 下限（F133 ZKMEDIA_H264_VBVSIZE 先例） */
#define ZKMEDIA_BUF_SIZE      (2 * 1024 * 1024)   /* 2,097,152 */
#define ZKMEDIA_REF_FRM_NUM   16

H264Player::H264Player(const Config& conf) :
    disp_(0),
    disp_layer_(disp_, 0),
    disp_layer_port_(disp_layer_, conf.disp_channel) {

  {
    auto attr = disp_layer_port_.getInputAttr();

    attr.stDispWin.u16X      = conf.out_x;
    attr.stDispWin.u16Y      = conf.out_y;
    attr.stDispWin.u16Width  = conf.out_w;
    attr.stDispWin.u16Height = conf.out_h;
    attr.u16SrcWidth         = ALIGN_BACK(conf.in_w, 32);
    attr.u16SrcHeight        = ALIGN_BACK(conf.in_h, 32);

    disp_layer_port_.setInputAttr(attr);
    disp_layer_port_.enable();
    disp_layer_port_.setSyncMode(E_MI_DISP_SYNC_MODE_FREE_RUN);
  }

  {
    std::lock_guard<std::mutex> lock(vdec_mutex_);
    if (count_ <= 0) {
      MI_VDEC_InitParam_t stVdecInitParam;
      memset(&stVdecInitParam, 0, sizeof(stVdecInitParam));
      stVdecInitParam.bDisableLowLatency = false;
      vdec_ = std::make_shared<VDEC>(stVdecInitParam);
    }
    ++count_;
  }

  {
    MI_VDEC_ChnAttr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.eCodecType     = E_MI_VDEC_CODEC_TYPE_H264;
    /* ★ 16 帧参考（厂商 SSTAR 通用版源码口径；原来是 1 帧 -> P 帧参考错乱 -> 块状花屏）*/
    attr.stVdecVideoAttr.u32RefFrameNum = ZKMEDIA_REF_FRM_NUM;
    attr.eVideoMode     = E_MI_VDEC_VIDEO_MODE_FRAME;
    /* ★ 2MB 码流缓冲（F133 ZKMEDIA_H264_VBVSIZE=2097152 先例；原来是 512KB）；更大分辨率按 5/4 面积放大 */
    {
      const MI_U32 need = (MI_U32)(conf.in_w * conf.in_h * 5 / 4);
      attr.u32BufSize = (need > ZKMEDIA_BUF_SIZE) ? ((need + 0xFFFF) & ~0xFFFFu) : ZKMEDIA_BUF_SIZE;
    }
    attr.u32PicWidth    = conf.in_w;
    attr.u32PicHeight   = conf.in_h;
    /* ★ DPB 正常模式（原来是 INPLACE_ONE_BUF，只有 1 帧 DPB）
     *   —— 注意：UseCusPolicy/ErrMBPercentThreshold **不随它变**（10:20 实测：NORMAL 下仍是 1/0），
     *      本 SDK 也没 API 设这两项；chn0 的 0/30 是创建方显式写的，与花屏无关。 */
    attr.eDpbBufMode    = E_MI_VDEC_DPB_MODE_NORMAL;
    attr.u32Priority    = 0;

    LOGI("h264_player[port]: chn=%d %ux%u BufSize=%u RefFrm=%u Dpb=%d Mode=%d (SSTAR 通用版口径)",
         conf.vdec_channel, conf.in_w, conf.in_h, attr.u32BufSize,
         attr.stVdecVideoAttr.u32RefFrameNum, (int)attr.eDpbBufMode, (int)attr.eVideoMode);

    auto channel = std::make_shared<VDEC::Channel>(*vdec_, conf.vdec_channel, attr);
    channel->start();

    auto out_attr = init<MI_VDEC_OutputPortAttr_t>();
    out_attr.u16Width  = conf.out_w;
    out_attr.u16Height = conf.out_h;

    LOGD("--- W x H --- %d x %d", out_attr.u16Width, out_attr.u16Height);
    channel->setOutputPortAttr(out_attr);
    channel->setDepth(1, 2);
    vdec_channel_ = channel;
  }

  chain_.bind(*vdec_channel_, disp_layer_port_,
      conf.in_frame_rate, conf.out_frame_rate, E_MI_SYS_BIND_TYPE_FRAME_BASE);
}

H264Player::~H264Player() {
  NO_EXCEPTION(chain_.unbindAll());
  vdec_channel_.reset();

  {
    std::lock_guard<std::mutex> lock(vdec_mutex_);
    --count_;
    if (count_ <= 0) {
      vdec_.reset();
    }
  }
}

void H264Player::stream(unsigned char* data, int len) {
  // ── 音画同步握手（2026-10-01）──────────────────────────────────────────
  //   这是"解码器开始吃第一包"的唯一时刻，也就是画面即将出现的时刻。
  //   屏保音频（独立播放器）等到这一刻再起播，就不会"画面还没出来声音先响"。
  //   只记第一次（noteFirstFeed 内部 CAS），每包调用无副作用。
  WallPlayer::noteFirstFeed();

  MI_VDEC_VideoStream_t stVdecStream = { 0 };
  stVdecStream.pu8Addr = data;
  stVdecStream.u32Len = len;
  stVdecStream.bEndOfFrame = 0;
  stVdecStream.bEndOfStream = 0;
  stVdecStream.u64PTS = 0;
  vdec_channel_->sendStream(stVdecStream, 0);
}

/* 包内实现里这个接口只服务"旋转渲染线程"（rotate != 0）。拼接墙不旋转、也无人调用它，
 * 这里留一个显式失败的桩，避免"未定义符号"这种更难查的问题。 */
int H264Player::getFrame(Frame& frame) {
  (void)frame;
  LOGE("h264_player[port]: getFrame() 未实现（源码移植版不提供旋转渲染线程）");
  return -1;
}

} /* namespace mi */

#endif /* PLATFORM_Z20__ */
