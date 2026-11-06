// main/test_ui.h —— 联调固件的测试画面。
//
// 与产品固件的挂件界面没有任何关系：没有小鲸鱼、没有气泡、没有中文文案，
// 也不复用产品的中文字模。整屏四块状态卡片，用 LVGL 自带的 Montserrat，
// 目的只有一个 —— 把手机 App 送来的数据原样摆出来。
//
// 这也意味着改这里不需要重跑任何字模生成脚本。
#pragma once

#include <stdbool.h>

#include "whale_link_diag.h"

#ifdef __cplusplus
extern "C" {
#endif

// 建屏。内部持 LVGL 锁；失败返回 false（调用方应停止启动）。
bool test_ui_create(void);

// 按一份统计快照刷新整屏。可从任意任务调用（内部持锁）。
void test_ui_update(const whale_diag_t *d);

// 电量百分比；soc < 0 表示电量计不可用（显示 --）。
void test_ui_battery(int soc);

// 显示一帧投屏画面：payload 是手机发来的像素数据，w/h 是像素尺寸，
// fmt 决定怎么读：
//   0 = RGB565 + RLE（token 见 RleCodec.java，纯色多的界面用它省带宽）
//   1 = 裸 RGB565 小端（复杂画面 RLE 会膨胀，手机端自动退回这种，绝不丢帧）
// 内部解码到独立缓冲后按整数倍率铺满整屏，并盖住状态卡片。
// 解码失败/尺寸越界返回 false，屏幕上什么都不变。
// 可从任意任务调用（内部持 LVGL 锁）。
bool test_ui_mirror_show(const uint8_t *payload, uint32_t len, uint16_t w, uint16_t h,
                         uint8_t fmt);

// 收起投屏画面，回到四块状态卡片。主循环在「一段时间没有新帧」时调用。
void test_ui_mirror_hide(void);

// 投屏画面当前是否在显示（主循环据此决定要不要去收）。
bool test_ui_mirror_visible(void);

// FILE 卡的一行状态：text 是拼好的整行（如 "test.bin 262KB OK"），
// ok 决定颜色（绿=成功进行中/完成，橙=被拒/出错）。
// 可从任意任务调用（内部持 LVGL 锁），文件传输回调直接用。
void test_ui_file_status(const char *text, bool ok);

// 不接手机时验证显示链路：内部造一张 120x160 的四色象限 + 中心棋盘图，
// 走与真帧完全相同的显示入口。屏幕能看到图案就说明解码/缩放/铺屏都是通的，
// 剩下的问题一定在传输侧。可从任意任务调用（内部持锁）。
bool test_ui_mirror_selftest(void);

// 显示已挂载到 Flash 槽位的资源。
//
// 与 test_ui_mirror_show 的根本差别：px 不是解码缓冲，而是 flash 的
// 【映射地址】（whale_file_map_slot 的返回值）—— LVGL 直接从 flash 读像素，
// 不做任何内存拷贝。240x320 的整屏照片要 150KB，拷进内存必然失败，
// 所以这条路径是「大图能显示」的唯一办法。
//
// 调用方责任：显示期间不能擦写该槽位（擦除会让映射内容立刻变垃圾），
// 换图前先 whale_file_unmap() 或再调本函数映射新槽位。
bool test_ui_resource_show(const uint8_t *px, uint16_t w, uint16_t h);

// 收起资源画面，回到状态卡片。不解除 flash 映射（那归 whale_file 管）。
void test_ui_resource_hide(void);

#ifdef __cplusplus
}
#endif
