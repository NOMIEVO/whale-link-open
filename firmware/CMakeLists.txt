cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 纯蓝牙联调固件:与 ai-passport 主工程解耦,唯一用途是验证
# whale-link-android App 的时间 / 传感器 / 投屏三条数据通路。
project(WhaleLinkTest)
