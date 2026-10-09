#pragma once

// S4 托盘菜单「模型预测设置」入口的生产挂钩面（issue #23 / map #18）。
// 纯函数契约见 include/WeaselPredictSettings.h；本头只声明绑定入口，
// 实现在 PredictSettingsEntry.cpp（注册表 / ShellExecuteW / MessageBox
// 生产绑定，合成测试用注入依赖覆盖纯函数层）。

#include <WeaselPredictSettings.h>

namespace weasel {

// 生产依赖绑定：HKCU 注册表状态 + GetModuleFileNameW 安装目录 +
// ShellExecuteW 启动 + GetFileAttributesW 存在性 + LoadStringW/MessageBoxW
// 失败回退。
PredictSettingsDeps MakePredictSettingsDeps();

// WeaselTrayIcon::CustomizeMenu 的挂钩：禁用态或侧车 exe 不存在时保持
// 普通菜单（不插项）；否则在 ID_WEASELTRAY_SYNC 之后插入
// 「分隔符 + 模型预测设置」。动态插入，不改 .rc 菜单结构（AC5）。
void PredictSettingsEntryCustomizeMenu(HMENU hMenu);

// 菜单命令处理器（SetupMenuHandlers 绑定）：解析路径并带唯一参数
// L"settings" 启动侧车 GUI exe；启动失败弹一次 LoadStringW 文案。
bool RunPredictSettingsEntry();

}  // namespace weasel
