# -*- coding: utf-8 -*-
"""编辑模式用的**每个控件各自的外边框**（钟工 2026-09-24 22:32）。

按屏保页 6 个可拖组的实际尺寸各生成一张透明底边框图（圆角实线，非虚线）：
  时间 480x140 / 日期 480x28 / 天气组 168x24 / 温湿度 480x28 / 状态条 452x40 / 黄历 400x70

画法（对齐 gen_borders.py 的过检口径，避免 AA 审计判「残差/硬阶梯」）：
  SS=4 超采样自绘 + Image.BOX 面积平均；边框 = 主强调绿 #7BE0A3（2px），
  内部 = 同色 8.5% 透明填充（让人一眼看出可拖区域），圆角 8。

用法：python ui/_gen/gen_edit_frame.py
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMGS = os.path.join(ROOT, 'resources', 'images')
ACC = (0x7B, 0xE0, 0xA3, 255)       # 主强调绿（边框）
ACC_FILL = (0x7B, 0xE0, 0xA3, 22)   # 内部淡填充（可拖区域提示）
RADIUS = 8
BORDER = 2
SS = 4

BOXES = [
    ('ss_ed_time.png',    480, 140, 0,   86),
    ('ss_ed_date.png',    480, 28,  0,   236),
    ('ss_ed_weather.png', 168, 24,  188, 282),
    ('ss_ed_th.png',      480, 28,  0,   326),
    ('ss_ed_bar.png',     452, 40,  16,  396),
    # 黄历（2026-10-01）：顶部整条 400x70。宽 400 而不是 480 —— 右上角 (424,20,40,40)
    # 是喇叭键，文字居中在 x24..424 内就不会压到它。
    ('ss_ed_almanac.png', 400, 70,  24,  4),
]


def frame_box(w, h):
    im = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # 外圈（不透明笔画）
    d.rounded_rectangle([0, 0, w * SS - 1, h * SS - 1], radius=RADIUS * SS, fill=ACC)
    # 内部挖空 + 淡填充（内缩 BORDER 像素，用覆盖法保留圆角过渡）
    inset = BORDER * SS
    inner = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    di = ImageDraw.Draw(inner)
    di.rounded_rectangle([inset, inset, w * SS - 1 - inset, h * SS - 1 - inset],
                         radius=max(0, (RADIUS - BORDER) * SS), fill=ACC_FILL)
    im.alpha_composite(inner)
    # 挖掉外圈内部（保留 BORDER 宽边框）：先做「内圈实心绿」再叠加淡填充会盖住，
    # 这里改用：整块淡填充 → 只把边框笔画画在最外圈（等价于 ring）
    return im.resize((w, h), Image.BOX)


def frame_ring(w, h):
    """覆盖率口径的圆角边框：外圆角实心 + 内圆角抠空（SS=4 超采样 + BOX 面积平均）。"""
    im = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, w * SS - 1, h * SS - 1], radius=RADIUS * SS, fill=ACC)
    # 抠空内部（透明），形成 BORDER 宽的环
    hole = Image.new('L', (w * SS, h * SS), 0)
    dh = ImageDraw.Draw(hole)
    dh.rounded_rectangle([BORDER * SS, BORDER * SS, w * SS - 1 - BORDER * SS,
                          h * SS - 1 - BORDER * SS],
                         radius=max(0, (RADIUS - BORDER) * SS), fill=255)
    im.putalpha(Image.composite(Image.new('L', im.size, 0), im.split()[3], hole))
    # 内部淡填充（提示可拖区域）
    fill = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    df = ImageDraw.Draw(fill)
    df.rounded_rectangle([BORDER * SS, BORDER * SS, w * SS - 1 - BORDER * SS,
                          h * SS - 1 - BORDER * SS],
                         radius=max(0, (RADIUS - BORDER) * SS), fill=ACC_FILL)
    im.alpha_composite(fill)
    return im.resize((w, h), Image.BOX)


def main():
    os.makedirs(IMGS, exist_ok=True)
    for name, w, h, left, top in BOXES:
        im = frame_ring(w, h)
        p = os.path.join(IMGS, name)
        im.save(p)
        a = im.split()[3]
        nz = sum(1 for v in a.tobytes() if v != 0)
        print('%-20s %sx%s  %5d B  非透明像素 %d' % (name, w, h, os.path.getsize(p), nz))


if __name__ == '__main__':
    main()
