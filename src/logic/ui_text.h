/*
 * ui_text.h -- 只依赖 ZKTextView 的极小 UI 工具（任何 logic .cc 都能安全包含）
 *
 * 为什么单独一个头：ui_common.h 里带了 nowMs()/noteActivity() 等 inline 单例，
 * 而 homeLogic.cc 自带同名 static 实现，直接包含会重定义冲突。这里只放与页面状态无关的工具。
 */
#pragma once

#include "control/ZKTextView.h"

#include <string>

/**
 * 「值真的变了才 setText」——判据是**控件当前的实际文本**，不是本地 static 缓存。
 *
 * 为什么不用 static std::string 记"上次写进去的值"：
 *   从屏保返回主页时，Activity 的控件会被重新实例化，文本回到 ui/*.json 里的静态占位
 *   （"读取中"/"获取中"），而本地 static 缓存仍认为"已经写过了" -> 于是再也不刷新，
 *   界面就永久停在占位文案上，直到真实数据下一次变化（最长要等 15 分钟的轮询）。
 *   实测踩到：屏保返回主界面后顶栏天气变成「获取中」。
 *   用 getText() 与实际文本比对，对该类重建/跨页/多入口场景天然免疫。
 *
 * EV_CONCURRENCY：只在主线程（UI 定时器/生命周期回调）里调用。
 * 返回 true 表示本次真的写了。
 */
inline bool setTextIfChanged(ZKTextView* p, const std::string& text) {
    if (p == NULL) return false;
    if (p->getText() == text) return false;
    p->setText(text.c_str());
    return true;
}

/**
 * 「可见性真的变了才 setVisible」——判据同样是控件的**实际状态**（isVisible()），
 * 不用本地缓存（理由同上：Activity 重建后控件状态会回到 ui/*.json 的默认值）。
 * 每秒调用也安全（没变就不写，不触发重绘）。返回 true 表示本次真的改了。
 */
inline bool setVisibleIfChanged(ZKTextView* p, bool visible) {
    if (p == NULL) return false;
    if (p->isVisible() == visible) return false;
    p->setVisible(visible);
    return true;
}
