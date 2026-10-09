// S4 托盘菜单「模型预测设置」入口的合成测试（issue #23 / map #18）。
// 覆盖：入口插入（含幂等与锚点缺失）、禁用/恢复状态循环、
// 重复点击（launch 两次、report 零次）、启动失败回退（report 恰一次）、
// 启动参数契约（唯一参数 L"settings"，无正文/凭据）。
// 物理桌面行为（托盘右键视觉、真机弹窗）归真机发布验收。
#include "stdafx.h"
#include <WeaselPredictSettings.h>
#include "../../WeaselServer/resource.h"

using namespace weasel;

int g_failures = 0;
#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      ++g_failures; \
      std::cerr << "FAIL @line " << __LINE__ << ": " #cond << std::endl; \
    } \
  } while (0)

// ---- 可注入依赖的 fake ----
static std::optional<DWORD> g_entry_enabled;
static std::optional<std::wstring> g_override;
static std::optional<std::wstring> g_install_dir;
static std::vector<std::wstring> g_existing_files;
static int g_launch_calls = 0;
static std::wstring g_launch_exe;
static std::wstring g_launch_arg;
static bool g_launch_result = false;
static int g_report_calls = 0;
static std::vector<std::wstring> g_reports;

static std::optional<DWORD> FakeReadEntryEnabled() { return g_entry_enabled; }
static std::optional<std::wstring> FakeReadSidebarOverride() {
  return g_override;
}
static std::optional<std::wstring> FakeInstallDirectory() {
  return g_install_dir;
}
static bool FakeLaunch(const std::wstring& exe, const std::wstring& arg) {
  ++g_launch_calls;
  g_launch_exe = exe;
  g_launch_arg = arg;
  return g_launch_result;
}
static void FakeReportLaunchFailure(const std::wstring& message) {
  ++g_report_calls;
  g_reports.push_back(message);
}
static bool FakeFileExists(const std::wstring& path) {
  for (const auto& existing : g_existing_files) {
    if (existing == path) return true;
  }
  return false;
}

static PredictSettingsDeps MakeFakeDeps() {
  PredictSettingsDeps deps;
  deps.read_entry_enabled = &FakeReadEntryEnabled;
  deps.read_sidebar_override = &FakeReadSidebarOverride;
  deps.install_directory = &FakeInstallDirectory;
  deps.launch = &FakeLaunch;
  deps.report_launch_failure = &FakeReportLaunchFailure;
  deps.file_exists = &FakeFileExists;
  return deps;
}

static void ResetFakes() {
  g_entry_enabled.reset();
  g_override.reset();
  g_install_dir.reset();
  g_existing_files.clear();
  g_launch_calls = 0;
  g_launch_exe.clear();
  g_launch_arg.clear();
  g_launch_result = false;
  g_report_calls = 0;
  g_reports.clear();
}

// ---- 纯函数：状态判定与路径解析 ----
static void RunPurePredicateCases() {
  CHECK(PredictEntryEnabled(std::optional<DWORD>()) == true);   // 缺省=启用
  CHECK(PredictEntryEnabled(std::optional<DWORD>(0)) == false);  // 0=禁用
  CHECK(PredictEntryEnabled(std::optional<DWORD>(1)) == true);
  CHECK(PredictEntryEnabled(std::optional<DWORD>(7)) == true);

  const std::wstring dir = L"C:\\Program Files\\Weasel\\";
  const std::wstring expected =
      dir + kPredictSidebarDirName + L"\\" + kPredictSidebarExeName;
  const auto path = PredictDefaultSidebarPath(dir);
  CHECK(path.has_value() && *path == expected);
  const auto path_no_slash = PredictDefaultSidebarPath(L"C:\\Program Files\\Weasel");
  CHECK(path_no_slash.has_value() && *path_no_slash == expected);
  const auto path_fwd = PredictDefaultSidebarPath(L"C:/Program Files/Weasel");
  CHECK(path_fwd.has_value() && *path_fwd == expected);
  CHECK(!PredictDefaultSidebarPath(L"").has_value());

  CHECK(PredictSidebarExe(std::optional<std::wstring>(),
                          std::optional<std::wstring>()) == std::nullopt);
  CHECK(PredictSidebarExe(std::optional<std::wstring>(),
                          std::optional<std::wstring>(L"D:\\x.exe")) ==
        std::optional<std::wstring>(L"D:\\x.exe"));
  CHECK(PredictSidebarExe(std::optional<std::wstring>(L""),
                          std::optional<std::wstring>(L"D:\\x.exe")) ==
        std::optional<std::wstring>(L"D:\\x.exe"));  // 空覆盖视同缺省
  CHECK(PredictSidebarExe(std::optional<std::wstring>(L"E:\\override\\sidecar.exe"),
                          std::optional<std::wstring>()) ==
        std::optional<std::wstring>(L"E:\\override\\sidecar.exe"));

  CHECK(std::wstring(kPredictSettingsLaunchArg) == L"settings");
  CHECK(kPredictSettingsMenuCommandId == ID_WEASELTRAY_PREDICT_SETTINGS);
}

// ---- 真实 HMENU：插入、幂等、锚点缺失、失败回滚 ----
static int FindItemById(HMENU hMenu, UINT id) {
  const int count = GetMenuItemCount(hMenu);
  for (int index = 0; index < count; ++index) {
    if (GetMenuItemID(hMenu, index) == id) return index;
  }
  return -1;
}

static void BuildUpstreamLikeMenu(HMENU hMenu) {
  AppendMenuW(hMenu, MF_STRING, ID_WEASELTRAY_SETTINGS, L"Settings");
  AppendMenuW(hMenu, MF_STRING, ID_WEASELTRAY_DICT_MANAGEMENT, L"Dict");
  AppendMenuW(hMenu, MF_STRING, ID_WEASELTRAY_SYNC, L"Sync");
  AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(hMenu, MF_STRING, ID_WEASELTRAY_USERCONFIG, L"User folder");
  AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(hMenu, MF_STRING, ID_WEASELTRAY_QUIT, L"Quit");
}

static void RunMenuInsertionCases() {
  HMENU menu = CreatePopupMenu();
  CHECK(menu != nullptr);
  BuildUpstreamLikeMenu(menu);
  const int before = GetMenuItemCount(menu);
  const std::wstring label = L"Model predict settings";

  CHECK(InsertPredictSettingsEntry(menu, ID_WEASELTRAY_SYNC,
                                   ID_WEASELTRAY_PREDICT_SETTINGS,
                                   label.c_str()));
  CHECK(GetMenuItemCount(menu) == before + 2);
  const int item_pos = FindItemById(menu, ID_WEASELTRAY_PREDICT_SETTINGS);
  CHECK(item_pos > 0);
  if (item_pos > 0) {
    CHECK((GetMenuState(menu, item_pos - 1, MF_BYPOSITION) & MF_SEPARATOR) != 0);
    CHECK(FindItemById(menu, ID_WEASELTRAY_SYNC) == item_pos - 2);
  }

  // 幂等：再次插入不产生重复项。
  CHECK(!InsertPredictSettingsEntry(menu, ID_WEASELTRAY_SYNC,
                                    ID_WEASELTRAY_PREDICT_SETTINGS,
                                    label.c_str()));
  CHECK(GetMenuItemCount(menu) == before + 2);

  // 现有菜单项原位未动（AC5）。
  CHECK(FindItemById(menu, ID_WEASELTRAY_SETTINGS) == 0);
  CHECK(FindItemById(menu, ID_WEASELTRAY_QUIT) == before + 1);

  DestroyMenu(menu);

  // 锚点缺失：不插入。
  HMENU plain = CreatePopupMenu();
  AppendMenuW(plain, MF_STRING, ID_WEASELTRAY_SETTINGS, L"Settings");
  CHECK(!InsertPredictSettingsEntry(plain, 99999,
                                    ID_WEASELTRAY_PREDICT_SETTINGS,
                                    label.c_str()));
  CHECK(GetMenuItemCount(plain) == 1);
  DestroyMenu(plain);

  // 空菜单：不插入。
  CHECK(!InsertPredictSettingsEntry(nullptr, ID_WEASELTRAY_SYNC,
                                    ID_WEASELTRAY_PREDICT_SETTINGS,
                                    label.c_str()));
}

// ---- 启动链：参数契约、禁用/恢复、重复点击、失败回退 ----
static void RunLaunchCases() {
  const std::wstring failure_message = L"launch failed msg";

  // 入口：启用 + 覆盖路径 + 启动成功；参数契约=恰好 L"settings"。
  ResetFakes();
  g_entry_enabled = 1;
  g_override = L"E:\\ModelPredict\\rime-predict-sidecar-gui.exe";
  g_install_dir = L"C:\\Program Files\\Weasel\\";
  g_existing_files.push_back(*g_override);
  g_launch_result = true;
  PredictSettingsDeps deps = MakeFakeDeps();
  CHECK(LaunchPredictSettings(deps, failure_message));
  CHECK(g_launch_calls == 1);
  CHECK(g_launch_arg == L"settings");
  CHECK(g_launch_exe == *g_override);
  CHECK(g_report_calls == 0);

  // 重复点击：launch 两次、report 零次（可重复操作，AC2/AC4）。
  CHECK(LaunchPredictSettings(deps, failure_message));
  CHECK(g_launch_calls == 2);
  CHECK(g_report_calls == 0);
  CHECK(g_launch_arg == L"settings");

  // 禁用态：静默丢弃，不启动也不报错（普通菜单恢复的点击面）。
  ResetFakes();
  g_entry_enabled = 0;
  g_override = L"E:\\ModelPredict\\rime-predict-sidecar-gui.exe";
  g_existing_files.push_back(*g_override);
  deps = MakeFakeDeps();
  CHECK(!LaunchPredictSettings(deps, failure_message));
  CHECK(g_launch_calls == 0);
  CHECK(g_report_calls == 0);

  // 禁用 -> 恢复 -> 再禁用 -> 再恢复：菜单供给状态可逆可重复（AC2）。
  ResetFakes();
  g_install_dir = L"C:\\Program Files\\Weasel\\";
  const auto default_exe = PredictDefaultSidebarPath(*g_install_dir);
  CHECK(default_exe.has_value());
  g_existing_files.push_back(*default_exe);
  deps = MakeFakeDeps();
  CHECK(PredictSettingsEntryAvailable(deps));
  g_entry_enabled = 0;
  CHECK(!PredictSettingsEntryAvailable(deps));
  g_entry_enabled = std::optional<DWORD>();  // 安装器删值=恢复缺省启用
  CHECK(PredictSettingsEntryAvailable(deps));
  g_entry_enabled = 0;
  CHECK(!PredictSettingsEntryAvailable(deps));
  g_entry_enabled = 1;
  CHECK(PredictSettingsEntryAvailable(deps));

  // 未装侧车：默认 exe 不存在 -> 菜单不插项（纯普通菜单，AC5）。
  ResetFakes();
  g_install_dir = L"C:\\Program Files\\Weasel\\";
  deps = MakeFakeDeps();
  CHECK(!PredictSettingsEntryAvailable(deps));

  // 启动失败（<=32）：report 恰好一次，返回 false（AC4 失败回退）。
  ResetFakes();
  g_entry_enabled = 1;
  g_override = L"E:\\x.exe";
  g_existing_files.push_back(L"E:\\x.exe");
  g_launch_result = false;
  deps = MakeFakeDeps();
  CHECK(!LaunchPredictSettings(deps, failure_message));
  CHECK(g_launch_calls == 1);
  CHECK(g_report_calls == 1);
  CHECK(g_reports.size() == 1 && g_reports[0] == failure_message);

  // 解析不出 exe（防御分支）：静默 false，不启动不弹窗。
  ResetFakes();
  g_entry_enabled = 1;
  deps = MakeFakeDeps();
  CHECK(!LaunchPredictSettings(deps, failure_message));
  CHECK(g_launch_calls == 0);
  CHECK(g_report_calls == 0);
}

int main() {
  RunPurePredicateCases();
  RunMenuInsertionCases();
  RunLaunchCases();
  if (g_failures) {
    std::cerr << g_failures << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "TestPredictSettingsMenu: all checks passed" << std::endl;
  return 0;
}
