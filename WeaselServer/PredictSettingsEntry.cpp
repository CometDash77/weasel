// S4 托盘菜单设置入口的生产绑定（issue #23 / map #18）。
// 契约与纯函数见 include/WeaselPredictSettings.h：
//   * 状态：HKCU\Software\Rime\Weasel\ModelPredict\SettingsEntryEnabled
//     （REG_DWORD，缺省=启用，0=禁用；S8 安装器为操作者，AC2）。
//   * 路径：HKCU\...\SidebarExe 覆盖，缺省
//     <install_dir>\ModelPredict\rime-predict-sidecar-gui.exe；
//     exe 不存在则菜单不插项（未装侧车 = 纯普通菜单）。
//   * 启动：ShellExecuteW(exe, L"settings")——唯一参数，无正文/凭据（AC3）。
//   * 失败回退：LoadStringW(IDS_STR_PREDICT_LAUNCH_FAILED) + MessageBoxW
//     恰好一次；AC1 的回环/令牌/Host-Origin 契约全部由 sidecar 一侧持有。
#include "stdafx.h"
#include "PredictSettingsEntry.h"

#include "resource.h"

namespace weasel {
namespace {

std::optional<DWORD> ReadEntryEnabledFromRegistry() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  const LSTATUS status = ::RegGetValueW(
      HKEY_CURRENT_USER, kPredictSettingsSubkey, kPredictSettingsEntryValue,
      RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (status != ERROR_SUCCESS) {
    return std::nullopt;  // 缺值 = 启用（PredictEntryEnabled 语义）
  }
  return value;
}

std::optional<std::wstring> ReadSidebarOverrideFromRegistry() {
  DWORD bytes = 0;
  LSTATUS status = ::RegGetValueW(
      HKEY_CURRENT_USER, kPredictSettingsSubkey, kPredictSidebarPathValue,
      RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
  if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
    return std::nullopt;  // 缺值 = 用安装目录默认路径
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  status = ::RegGetValueW(HKEY_CURRENT_USER, kPredictSettingsSubkey,
                          kPredictSidebarPathValue, RRF_RT_REG_SZ, nullptr,
                          value.data(), &bytes);
  if (status != ERROR_SUCCESS) {
    return std::nullopt;
  }
  value.resize(bytes / sizeof(wchar_t) - 1);  // 去掉结尾 NUL
  return value;
}

std::optional<std::wstring> ModuleInstallDirectory() {
  wchar_t path[MAX_PATH] = {};
  const UINT length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::nullopt;
  }
  const std::wstring full(path);
  const size_t pos = full.find_last_of(L"\\/");
  if (pos == std::wstring::npos) {
    return std::nullopt;
  }
  return full.substr(0, pos + 1);  // 含尾分隔符
}

bool FileExistsOnDisk(const std::wstring& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool ShellExecuteLaunch(const std::wstring& exe, const std::wstring& arg) {
  // 判据与上游 WeaselServerApp::execute 一致：>32 即成功。
  const HINSTANCE result = ::ShellExecuteW(nullptr, nullptr, exe.c_str(),
                                           arg.c_str(), nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<uintptr_t>(result) > 32;
}

void ReportLaunchFailureViaMessageBox(const std::wstring& fallback_message) {
  wchar_t text[256] = {};
  const int length = ::LoadStringW(::GetModuleHandleW(nullptr),
                                   IDS_STR_PREDICT_LAUNCH_FAILED, text, 256);
  const wchar_t* message = length > 0 ? text : fallback_message.c_str();
  if (length <= 0 && fallback_message.empty()) {
    return;  // 无文案可报：静默（防御分支，资源表应当有 304 号串）
  }
  ::MessageBoxW(nullptr, message, L"Weasel", MB_OK | MB_ICONWARNING);
}

}  // namespace

PredictSettingsDeps MakePredictSettingsDeps() {
  PredictSettingsDeps deps;
  deps.read_entry_enabled = &ReadEntryEnabledFromRegistry;
  deps.read_sidebar_override = &ReadSidebarOverrideFromRegistry;
  deps.install_directory = &ModuleInstallDirectory;
  deps.launch = &ShellExecuteLaunch;
  deps.report_launch_failure = &ReportLaunchFailureViaMessageBox;
  deps.file_exists = &FileExistsOnDisk;
  return deps;
}

void PredictSettingsEntryCustomizeMenu(HMENU hMenu) {
  if (hMenu == nullptr) {
    return;
  }
  const PredictSettingsDeps deps = MakePredictSettingsDeps();
  if (!PredictSettingsEntryAvailable(deps)) {
    return;  // 禁用态 / 未装侧车：普通菜单原样
  }
  wchar_t label[128] = {};
  if (::LoadStringW(::GetModuleHandleW(nullptr), IDS_STR_PREDICT_SETTINGS,
                    label, 128) <= 0) {
    return;  // 无菜单文案：宁缺毋滥，保持普通菜单
  }
  InsertPredictSettingsEntry(hMenu, ID_WEASELTRAY_SYNC,
                             ID_WEASELTRAY_PREDICT_SETTINGS, label);
}

bool RunPredictSettingsEntry() {
  wchar_t text[256] = {};
  const int length = ::LoadStringW(::GetModuleHandleW(nullptr),
                                   IDS_STR_PREDICT_LAUNCH_FAILED, text, 256);
  const PredictSettingsDeps deps = MakePredictSettingsDeps();
  return LaunchPredictSettings(
      deps, length > 0 ? std::wstring(text) : std::wstring());
}

}  // namespace weasel
