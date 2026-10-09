#pragma once

// S4 托盘菜单设置入口契约（issue #23 / map #18）。
//
// 目标：托盘右键菜单出现「模型预测设置」一项，唤起本地网页设置会话
// （sidecar GUI exe -> settings-host 回环会话），并保留禁用与恢复普通
// 菜单的可逆路径。
//
// 设计（与 spec #18 一致）：
//   * 菜单项**动态插入**，不改上游 IDR_MENU_POPUP 的 .rc 菜单结构——
//     禁用态/未装侧车时右键菜单与上游完全一致（AC5）。
//   * 启用状态与侧车路径覆盖放在 HKCU 注册表（见下表），操作者是 S8
//     安装器（写入）与 S9 卸载器（移除）；托盘菜单每次右键现读，安装器
//     翻值即时生效。
//   * 启动参数契约：fork 对侧车 exe 只传唯一子命令参数 L"settings"。
//     不携带用户正文、拼音、候选、令牌或任何凭据；令牌与回环绑定、
//     Host/Origin 校验全部在 sidecar（settings-host）一侧完成（AC3）。
//
// 注册表契约（HKCU\Software\Rime\Weasel\ModelPredict，S8 冻结候选）：
//   | 值名                 | 类型    | 缺省语义                       |
//   | -------------------- | ------- | ------------------------------ |
//   | SettingsEntryEnabled | REG_DWORD | 缺省（值缺失）= 启用；0 = 禁用 |
//   | SidebarExe           | REG_SZ    | 缺省（值缺失）= 安装目录默认路径 |
//
// 纯函数层（本头文件）只做内存内的判定/组装，不触注册表与进程 API；
// 生产绑定（RegGetValueW/ShellExecuteW/MessageBoxW）在
// WeaselServer/PredictSettingsEntry.cpp，由合成测试
// test/TestPredictSettingsMenu 以注入依赖覆盖。

#include <windows.h>

#include <optional>
#include <string>

namespace weasel {

// 注册表子键与值名（S8 冻结候选；fork 代码只经本头引用）。
inline constexpr wchar_t kPredictSettingsSubkey[] =
    L"Software\\Rime\\Weasel\\ModelPredict";
inline constexpr wchar_t kPredictSettingsEntryValue[] = L"SettingsEntryEnabled";
inline constexpr wchar_t kPredictSidebarPathValue[] = L"SidebarExe";

// 安装目录下的侧车 GUI exe 相对路径（S8 冻结候选）。
inline constexpr wchar_t kPredictSidebarDirName[] = L"ModelPredict";
inline constexpr wchar_t kPredictSidebarExeName[] =
    L"rime-predict-sidecar-gui.exe";

// 传给侧车 exe 的唯一子命令参数（AC3：不带正文/凭据）。
inline constexpr wchar_t kPredictSettingsLaunchArg[] = L"settings";

// 菜单命令 ID：取 WeaselServer/resource.h 托盘段末尾的下一个空位
// （ID_WEASELTRAY_LOGDIR=40016 之后）。生产代码经 resource.h 引用，
// 此处仅为契约文档保留数值。
inline constexpr int kPredictSettingsMenuCommandId = 40017;

// 启用判定：SettingsEntryEnabled 缺省（无值）= 启用；显式 0 = 禁用；
// 其余任何值 = 启用（宽容解析，安装器只写 0/1）。
inline bool PredictEntryEnabled(
    const std::optional<DWORD>& settings_entry_enabled) {
  return !settings_entry_enabled.has_value() || *settings_entry_enabled != 0;
}

// 侧车 exe 路径解析（按优先级）：
//   1. 注册表 SidebarExe 覆盖（非空即用，S8 安装器写的绝对路径）；
//   2. <install_dir>\\ModelPredict\\rime-predict-sidecar-gui.exe；
//   3. 默认路径文件不存在 -> nullopt（未装侧车：菜单不插项，保持普通菜单）。
inline std::optional<std::wstring> PredictSidebarExe(
    const std::optional<std::wstring>& sidebar_exe_override,
    const std::optional<std::wstring>& default_sidebar_exe) {
  if (sidebar_exe_override.has_value() && !sidebar_exe_override->empty()) {
    return *sidebar_exe_override;
  }
  return default_sidebar_exe;
}

// 安装目录 -> 默认侧车 exe 绝对路径（install_dir 为空时 nullopt）。
inline std::optional<std::wstring> PredictDefaultSidebarPath(
    const std::wstring& install_dir) {
  if (install_dir.empty()) {
    return std::nullopt;
  }
  std::wstring base = install_dir;
  const wchar_t last = base.back();
  if (last != L'\\' && last != L'/') {
    base.push_back(L'\\');  // install_dir 通常已带尾分隔符，缺则补
  }
  return base + kPredictSidebarDirName + L"\\" + kPredictSidebarExeName;
}

// 在 hMenu 的 after_command_id 项之后插入「分隔符 + 设置项」。after 与
// item 均以命令 ID（而非位置）寻址，避免语言差异导致的位置漂移。
// 返回是否发生了插入；已存在 item 命令 ID 时跳过（幂等防御：托盘菜单
// 每次右键重新 LoadMenu，正常不会重复，插入两次即破坏菜单形状）。
inline bool InsertPredictSettingsEntry(HMENU hMenu, UINT after_command_id,
                                       UINT item_command_id,
                                       const wchar_t* item_label) {
  if (hMenu == nullptr || item_label == nullptr) {
    return false;
  }
  const int count = GetMenuItemCount(hMenu);
  if (count <= 0) {
    return false;
  }
  for (int index = 0; index < count; ++index) {
    if (GetMenuItemID(hMenu, index) == item_command_id) {
      return false;  // 已插入，幂等跳过
    }
  }
  int anchor = -1;  // after_command_id 缺省（-1）：不插入
  for (int index = 0; index < count; ++index) {
    if (GetMenuItemID(hMenu, index) == after_command_id) {
      anchor = index;
      break;
    }
  }
  if (anchor < 0) {
    return false;
  }
  const int insert_at = anchor + 1;
  MENUITEMINFOW separator = {};
  separator.cbSize = sizeof(separator);
  separator.fMask = MIIM_TYPE;
  separator.fType = MFT_SEPARATOR;
  MENUITEMINFOW item = {};
  item.cbSize = sizeof(item);
  item.fMask = MIIM_ID | MIIM_TYPE | MIIM_STATE;
  item.fType = MFT_STRING;
  item.fState = MFS_ENABLED;
  item.wID = item_command_id;
  item.dwTypeData = const_cast<LPWSTR>(item_label);
  item.cch = static_cast<UINT>(wcslen(item_label));
  if (!InsertMenuItemW(hMenu, insert_at, TRUE, &separator)) {
    return false;
  }
  if (!InsertMenuItemW(hMenu, insert_at + 1, TRUE, &item)) {
    // 分隔符插入成功而项目失败：撤掉分隔符，保持菜单形状完整。
    RemoveMenu(hMenu, insert_at, MF_BYPOSITION);
    return false;
  }
  return true;
}

// 依赖注入面：合成测试用 fake 全替换，生产绑定见 PredictSettingsEntry.cpp。
struct PredictSettingsDeps {
  // HKCU\...\ModelPredict\SettingsEntryEnabled（缺值 nullopt）。
  std::optional<DWORD> (*read_entry_enabled)() = nullptr;
  // HKCU\...\ModelPredict\SidebarExe 覆盖（缺值 nullopt）。
  std::optional<std::wstring> (*read_sidebar_override)() = nullptr;
  // 安装目录（GetModuleFileNameW 去文件名）。
  std::optional<std::wstring> (*install_directory)() = nullptr;
  // ShellExecuteW 绑定：返回 >32 视为成功（与上游 execute() 同判据）。
  bool (*launch)(const std::wstring& exe, const std::wstring& arg) = nullptr;
  // 启动失败的用户可见回退（MessageBox）；测试用 fake 记录调用。
  void (*report_launch_failure)(const std::wstring& message) = nullptr;
  // 文件存在性（生产=GetFileAttributesW）；决定菜单是否插项。
  bool (*file_exists)(const std::wstring& path) = nullptr;
};

// 菜单是否应当提供设置项：启用态且解析出的侧车 exe 真实存在。
// 禁用态（AC2）与未装侧车都返回 false -> 普通菜单原样（AC5）。
inline bool PredictSettingsEntryAvailable(const PredictSettingsDeps& deps) {
  if (!PredictEntryEnabled(deps.read_entry_enabled())) {
    return false;
  }
  const std::optional<std::wstring> exe = PredictSidebarExe(
      deps.read_sidebar_override(),
      PredictDefaultSidebarPath(
          deps.install_directory().value_or(std::wstring())));
  if (!exe.has_value() || exe->empty()) {
    return false;
  }
  return deps.file_exists(*exe);
}

// 菜单命令处理器：读状态 -> 解析路径 -> 恰好带一个 L"settings" 参数启动。
// 路径解析失败（未装侧车）：静默返回 false，不弹窗（此时菜单本就不该
// 出现该项，点击属防御分支）。启动失败：经 report_launch_failure 恰好
// 报告一次。返回是否启动成功。
inline bool LaunchPredictSettings(const PredictSettingsDeps& deps,
                                  const std::wstring& failure_message) {
  if (!PredictEntryEnabled(deps.read_entry_enabled())) {
    return false;  // 禁用态点击：静默丢弃
  }
  const std::optional<std::wstring> exe =
      PredictSidebarExe(deps.read_sidebar_override(),
                        PredictDefaultSidebarPath(
                            deps.install_directory().value_or(std::wstring())));
  if (!exe.has_value() || exe->empty()) {
    return false;  // 未装侧车：静默
  }
  if (deps.launch(*exe, kPredictSettingsLaunchArg)) {
    return true;
  }
  deps.report_launch_failure(failure_message);
  return false;
}

}  // namespace weasel
