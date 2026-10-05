# -*- coding: utf-8 -*-
"""把参考素材视频目录整体推到设备 /mnt/sdnand/video（屏保视频列表的扫描目录）。

为什么不能直接 `adb push 目录`：
  素材文件名是中文（4个小猫.mp4 / 嫦娥.mp4 …）。中文名一旦作为**命令行参数**传给 adb/设备 shell，
  会经过 Windows 控制台编码（GBK/UTF-8 转换）而损坏。
做法（全程不让中文出现在命令行参数里）：
  ① 本地把每个 mp4 复制成 ASCII 暂存名 zkv_01.mp4…（纯 ASCII 路径 push，稳）
  ② 本地生成一份 UTF-8 映射文件 <ascii>|<中文原名>，推到设备 /tmp
  ③ 设备侧用**全 ASCII 的 shell 循环**读映射文件做 `mv`（中文名只存在于文件字节里）
  ④ 逐个校验「名字 + 大小」

用法：
  python projects/SmartPanel_HA/tools/push_video.py                 # 默认源/目标/设备
  python projects/SmartPanel_HA/tools/push_video.py --dry-run       # 只列清单
  python projects/SmartPanel_HA/tools/push_video.py --src D:\\vids --dest /mnt/sdnand/video --device 192.0.2.108:5555
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

# ⚠️ Windows 控制台默认 GBK：打印非 GBK 字符会 UnicodeEncodeError 把脚本打崩
# （实测踩过：探针输出里的符号直接让推送流程在收尾打印时挂掉）。
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

WS = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
# 2026-10-01 修：原默认路径 D:\flythings\tools\FlyThings_mcp_open\... 不存在（工具是从另一个工作区搬过来的）
DEFAULT_ADB = os.path.join(WS, 'fly-things-os_-mcp', 'tools', 'adb', 'adb.exe')
DEFAULT_SRC = os.path.join(WS, 'projects', 'SmartPanel_HA', '参考资料', 'video')
DEFAULT_DEST = '/mnt/sdnand/video'
DEFAULT_DEVICE = '192.168.101.2:5555'   # 本板（SmartPanel 4 寸面板）
BUSYBOX = os.path.join(WS, 'tools', 'busybox', 'bin', 'z20', 'busybox')

# 推送前规格体检（防止不达标素材进机：设备只吃 H.264 / 480x480 / <=1800 kb/s，
# 且设备侧无解码器也无转码器 → 进机的死料无法补救）。要处理素材请先跑 tools/media_gate.py。
MP4_PROBE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mp4_probe.js')
DEFAULT_NODE = r'C:\Users\zhong\.workbuddy\binaries\node\versions\22.22.2\node.exe'
MAX_BITRATE = 1800


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    return r.returncode, (r.stdout or '') + (r.stderr or '')


def spec_check(node, src, files, max_bitrate):
    """对要推的 mp4 跑 tools/mp4_probe.js。返回 (ok, 消息)。没有 mp4 时直接通过。"""
    mp4s = [os.path.join(src, f) for f in files if f.lower().endswith('.mp4')]
    if not mp4s:
        return True, '（本批无 mp4，跳过）'
    n = node if (node and os.path.isfile(node)) else (shutil.which('node') or DEFAULT_NODE)
    if not (n and os.path.isfile(n)) or not os.path.isfile(MP4_PROBE):
        return True, '（找不到 node 或 mp4_probe.js，跳过体检）'
    rc, out = run([n, MP4_PROBE, '--max-bitrate', str(max_bitrate)] + mp4s)
    if rc == 0:
        return True, '通过（%d 个 mp4，H.264 / <=480x480 / <=%d kb/s）' % (len(mp4s), max_bitrate)
    bad = [ln.strip() for ln in out.splitlines() if ln.strip().startswith('文件      :')]
    return False, '不达标 %d 个：%s\n%s' % (len(bad) or 1, '; '.join(x.split(':', 1)[1].strip().split()[0] for x in bad),
                                          '\n'.join(out.splitlines()[-40:]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=DEFAULT_SRC)
    ap.add_argument('--dest', default=DEFAULT_DEST)
    ap.add_argument('--device', default=DEFAULT_DEVICE)
    ap.add_argument('--adb', default=DEFAULT_ADB)
    ap.add_argument('--ext', default='.mp4',
                    help='要推送的扩展名，可逗号分隔（例：--ext .mp3 推同名音轨；--ext .mp4,.mp3 一起推）')
    ap.add_argument('--no-check', action='store_true',
                    help='跳过推送前的规格体检（默认开：不达标的 mp4 直接拦下）')
    ap.add_argument('--max-bitrate', type=int, default=MAX_BITRATE, help='体检码率上限 kb/s')
    ap.add_argument('--node', default=None, help='node 可执行文件路径（体检用）')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    if not os.path.isdir(a.src):
        print('源目录不存在: %s' % a.src)
        return 2
    exts = tuple(e.strip().lower() for e in a.ext.split(',') if e.strip())
    files = sorted(f for f in os.listdir(a.src)
                   if os.path.isfile(os.path.join(a.src, f)) and f.lower().endswith(exts))
    if not files:
        print('源目录没有 mp4: %s' % a.src)
        return 2

    total = sum(os.path.getsize(os.path.join(a.src, f)) for f in files)
    print('源: %s' % a.src)
    print('目标: %s:%s' % (a.device, a.dest))
    for f in files:
        print('  %-16s %9d B' % (f, os.path.getsize(os.path.join(a.src, f))))
    print('共 %d 个文件 / %.2f MB' % (len(files), total / 1048576.0))

    # ★ 规格体检：不达标的 mp4 一律拦下（设备无解码器/转码器，进了机就是死料）
    if not a.no_check:
        ok, msg = spec_check(a.node, a.src, files, a.max_bitrate)
        if ok:
            print('规格体检: [OK] %s' % msg)
        else:
            print('规格体检: [X] 已拦下 -- %s' % msg)
            print('\n  请先处理素材再推：')
            print('    python tools/media_gate.py --src "%s" --out <暂存目录> --mp3' % a.src)
            print('    （或加 --no-check 强行推送）')
            return 8

    if a.dry_run:
        print('(--dry-run：未推送)')
        return 0

    rc, out = run([a.adb, '-s', a.device, 'get-state'])
    if 'device' not in out:
        print('设备未就绪: %s -> %s' % (a.device, out.strip()))
        return 3
    run([a.adb, '-s', a.device, 'shell', 'mkdir -p %s' % a.dest])

    # ① 暂存 ASCII 名
    stage = tempfile.mkdtemp(prefix='zkswe_vid_')
    pairs = []
    for i, f in enumerate(files, 1):
        ext = os.path.splitext(f)[1].lower()
        alias = 'zkv_%02d%s' % (i, ext)
        shutil.copy2(os.path.join(a.src, f), os.path.join(stage, alias))
        pairs.append((alias, f))
    print('暂存目录: %s（ASCII 名 %d 个）' % (stage, len(pairs)))

    # ② 映射文件（UTF-8，无 BOM）；文件名只存在字节里，不进命令行
    map_local = os.path.join(stage, 'map.txt')
    with open(map_local, 'w', encoding='utf-8', newline='\n') as fh:
        for alias, real in pairs:
            fh.write('%s|%s\n' % (alias, real))
    map_dev = '/tmp/zkswe_video_map.txt'
    rc, out = run([a.adb, '-s', a.device, 'push', map_local, map_dev])
    if rc != 0:
        print('推映射文件失败: %s' % out.strip()[-300:])
        return 4

    # ③ 逐个 push（纯 ASCII 源/目标）
    n = 0
    for alias, real in pairs:
        rc, out = run([a.adb, '-s', a.device, 'push',
                       os.path.join(stage, alias), '%s/%s' % (a.dest, alias)])
        if rc != 0:
            print('push 失败 %s: %s' % (alias, out.strip()[-200:]))
            return 5
        n += 1
        print('  pushed %-12s -> %s/%s  (%.2f MB)' % (alias, a.dest, alias,
                                                      os.path.getsize(os.path.join(stage, alias)) / 1048576.0))

    # ③b 设备侧改名：全 ASCII 脚本读映射文件
    sh = ('cd %s && while IFS="|" read -r A B; do '
          '[ -n "$A" ] && [ -f "$A" ] && mv -f "$A" "$B"; done < %s; echo DONE' % (a.dest, map_dev))
    rc, out = run([a.adb, '-s', a.device, 'shell', sh])
    if 'DONE' not in out:
        print('改名失败: %s' % out.strip()[-300:])
        return 6
    run([a.adb, '-s', a.device, 'shell', 'rm -f %s' % map_dev])
    print('改名完成（%d 个 -> 中文原名）' % n)

    # ④ 校验：设备 busybox 精简（无 tar/stat/awk/wc）→ 只靠 `ls -l` 解析「大小 + 名字」
    rc, out = run([a.adb, '-s', a.device, 'shell', 'ls -l %s' % a.dest])
    dev = {}
    for line in out.splitlines():
        if not line.startswith('-'):
            continue
        p = line.split(None, 8)          # 权限 n link owner group SIZE date time NAME
        if len(p) >= 9 and p[4].isdigit():
            dev[p[8].strip()] = p[4]
    print('\n校验（本地 -> 设备）:')
    bad = 0
    for f in files:
        lsz = str(os.path.getsize(os.path.join(a.src, f)))
        dsz = dev.get(f)
        ok = dsz == lsz
        bad += 0 if ok else 1
        print('  %s %-16s local=%9s device=%9s' % ('OK ' if ok else 'BAD', f, lsz, dsz or 'MISSING'))
    extra = sorted(k for k in dev if k not in files)
    if extra:
        print('  设备上另有: %s' % ', '.join(extra))
    print('\n结果: %d/%d 一致%s' % (len(files) - bad, len(files), '' if bad == 0 else '  <== 有差异'))
    shutil.rmtree(stage, ignore_errors=True)
    return 0 if bad == 0 else 7


if __name__ == '__main__':
    sys.exit(main())
