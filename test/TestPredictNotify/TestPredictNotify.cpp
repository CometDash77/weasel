// TestPredictNotify.cpp : S3 completion-notify receiver contract tests.
//
// Synthetic receiver window: the test instantiates the real ServerImpl
// window (class WeaselIPCWindow_1.0) with a recording request handler and
// drives WM_COPYDATA against it, covering byte-exact payload delivery,
// malformed rejection, identity gating and failure silence. Real Weasel
// interop is release-acceptance territory on a real machine; when a real
// WeaselServer already owns the single-instance mutex the window cases are
// skipped and only the pure parsing cases run.

#include "stdafx.h"
#include <WeaselIPC.h>
#include <WeaselPredictNotify.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

CAppModule _Module;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      ++g_failures;                                                      \
      std::cerr << "FAIL @line " << __LINE__ << ": " #cond << std::endl; \
    }                                                                    \
  } while (0)

const char kEngine[] = "0123456789abcdef0123456789abcdef01234567";  // 40 hex
const char kRequest[] = "89abcdef0123456789abcdef01234567";         // 32 hex

std::string ToUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return s;
}

// Mirrors the production gate in RimeWithWeaselHandler::PredictCompletion:
// published identity in, the very same pure matcher, flip counter out.
class RecordingPredictHandler : public weasel::RequestHandler {
 public:
  std::vector<weasel::PredictNotify> delivered;
  std::string published_engine_id;
  std::string published_request_id;
  std::string published_seq;
  int flips = 0;

  void PredictCompletion(const weasel::PredictNotify& notify) override {
    if (!weasel::PredictIdentityMatches(published_engine_id,
                                        published_request_id, published_seq,
                                        notify)) {
      return;  // reject silently, exactly like production
    }
    ++flips;
    delivered.push_back(notify);
  }
};

std::string PayloadWithSeq(const std::string& seq) {
  return std::string(kEngine) + "\n" + kRequest + "\n" + seq;
}

std::string Nul(const std::string& body) {
  std::string bytes = body;
  bytes.push_back('\0');
  return bytes;
}

// Sends a payload the way the sidecar does: UTF-8 bytes + terminating NUL,
// cbData counting the NUL.
BOOL SendNotify(HWND wnd, const std::string& payload) {
  return SendRaw(wnd, WEASEL_PREDICT_NOTIFY_COPYDATA_ID, Nul(payload));
}

// Sends raw COPYDATA bytes with a chosen dwData.
BOOL SendRaw(HWND wnd, DWORD dw_data, const std::string& bytes) {
  COPYDATASTRUCT cds = {};
  cds.dwData = dw_data;
  cds.cbData = static_cast<DWORD>(bytes.size());
  cds.lpData = const_cast<char*>(bytes.c_str());
  return SendMessage(wnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
}

std::string ClassNameOf(HWND wnd) {
  wchar_t name[64] = {};
  int written = GetClassNameW(wnd, name, 64);
  if (written <= 0)
    return std::string();
  int len = WideCharToMultiByte(CP_UTF8, 0, name, written, nullptr, 0,
                                nullptr, nullptr);
  std::string result(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, name, written, &result[0], len, nullptr,
                      nullptr);
  return result;
}

BOOL CALLBACK FindIpcWindowClass(HWND wnd, LPARAM param) {
  auto* found = reinterpret_cast<bool*>(param);
  wchar_t name[64] = {};
  if (GetClassNameW(wnd, name, 64) > 0 &&
      wcscmp(name, WEASEL_IPC_WINDOW) == 0) {
    *found = true;
    return FALSE;
  }
  return TRUE;
}

void RunPureParserCases() {
  const std::string eng(kEngine);
  const std::string req(kRequest);

  // valid payloads, bounds included
  auto parsed = weasel::ParsePredictNotifyPayload(
      Nul(eng + "\n" + req + "\n1").data(), Nul(eng + "\n" + req + "\n1").size());
  CHECK(parsed.has_value());
  CHECK(parsed && parsed->engine_id == eng);
  CHECK(parsed && parsed->request_id == req);
  CHECK(parsed && parsed->seq == 1);
  const std::string max_seq = Nul(eng + "\n" + req + "\n9007199254740991");
  parsed = weasel::ParsePredictNotifyPayload(max_seq.data(), max_seq.size());
  CHECK(parsed && parsed->seq == 9007199254740991ull);

  // malformed bodies under correct NUL accounting
  const char* bad_bodies[] = {
      "", "x", "a\nb", "a\nb\n1\n2", "\n\n1", "a\nb\nx", "a\nb\n0",
      "a\nb\n01", "a\nb\n+1", "a\nb\n-1", "a\nb\n 1", "a\nb\n1 ",
      "a\nb\n9007199254740992",
      "a\nb\n00000000000000000000000000000000000000000000000000000000000001",
  };
  for (const auto* body : bad_bodies) {
    const std::string bytes = Nul(std::string(body));
    CHECK(!weasel::ParsePredictNotifyPayload(bytes.data(), bytes.size()));
  }

  // engine id format violations
  const std::string upper = ToUpper(eng);
  const std::string nonhex = [&eng] {
    std::string s = eng;
    s[0] = 'g';
    return s;
  }();
  for (const auto& variant : {std::string(eng.substr(0, 39)), eng + "a",
                              upper, nonhex}) {
    const std::string bytes = Nul(variant + "\n" + req + "\n1");
    CHECK(!weasel::ParsePredictNotifyPayload(bytes.data(), bytes.size()));
  }

  // request id format violations
  const std::string upper_req = ToUpper(req);
  for (const auto& variant : {std::string(req.substr(0, 31)), req + "a",
                              upper_req}) {
    const std::string bytes = Nul(eng + "\n" + variant + "\n1");
    CHECK(!weasel::ParsePredictNotifyPayload(bytes.data(), bytes.size()));
  }

  // NUL accounting: cbData counts exactly one terminating NUL
  const std::string body = eng + "\n" + req + "\n1";
  CHECK(!weasel::ParsePredictNotifyPayload(body.data(), body.size()));
  std::string mid_nul = body.substr(0, 3);
  mid_nul.push_back('\0');
  mid_nul += body.substr(3);
  mid_nul.push_back('\0');
  CHECK(!weasel::ParsePredictNotifyPayload(mid_nul.data(), mid_nul.size()));
  const std::string trailing = Nul(body) + "x";
  CHECK(!weasel::ParsePredictNotifyPayload(trailing.data(), trailing.size()));
  CHECK(!weasel::ParsePredictNotifyPayload(body.data(), 0));
  CHECK(!weasel::ParsePredictNotifyPayload(nullptr, 8));
  std::string oversized(body.size() + 200, 'a');
  CHECK(!weasel::ParsePredictNotifyPayload(oversized.data(), oversized.size()));

  // identity gate semantics
  weasel::PredictNotify notice{eng, req, 7};
  CHECK(weasel::PredictIdentityMatches(eng, req, "7", notice));
  CHECK(!weasel::PredictIdentityMatches(std::string(), req, "7", notice));
  CHECK(!weasel::PredictIdentityMatches(eng, std::string(), "7", notice));
  CHECK(!weasel::PredictIdentityMatches(eng, req, std::string(), notice));
  CHECK(!weasel::PredictIdentityMatches(eng, req, "8", notice));
  CHECK(!weasel::PredictIdentityMatches(eng, req, "007", notice));
  CHECK(!weasel::PredictIdentityMatches(upper, req, "7", notice));
  CHECK(!weasel::PredictIdentityMatches(nonhex, req, "7", notice));
}

int RunWindowCases() {
  RecordingPredictHandler handler;
  weasel::Server server;
  server.SetRequestHandler(&handler);
  if (!server.Start()) {
    std::cout << "SKIP: single-instance mutex held (a real WeaselServer is "
                 "running); window-level cases skipped." << std::endl;
    return 0;
  }
  HWND wnd = server.GetHWnd();
  CHECK(wnd != nullptr);
  CHECK(ClassNameOf(wnd) == "WeaselIPCWindow_1.0");

  // the receiver window must be discoverable by EnumWindows + GetClassName
  bool enumerated = false;
  EnumWindows(FindIpcWindowClass, reinterpret_cast<LPARAM>(&enumerated));
  CHECK(enumerated);

  handler.published_engine_id = kEngine;
  handler.published_request_id = kRequest;
  handler.published_seq = "7";

  // byte-exact valid delivery
  CHECK(SendNotify(wnd, PayloadWithSeq("7")));
  CHECK(handler.delivered.size() == 1);
  CHECK(handler.flips == 1);
  if (!handler.delivered.empty()) {
    const auto& n = handler.delivered.back();
    CHECK(n.engine_id == kEngine);
    CHECK(n.request_id == kRequest);
    CHECK(n.seq == 7);
  }

  // identity gate: stale, current, duplicate-of-current, unpublished,
  // mismatched, malformed published seq
  CHECK(SendNotify(wnd, PayloadWithSeq("9")));
  CHECK(handler.flips == 1);
  handler.published_seq = "8";
  CHECK(SendNotify(wnd, PayloadWithSeq("8")));
  CHECK(handler.flips == 2);
  CHECK(SendNotify(wnd, PayloadWithSeq("8")));
  CHECK(handler.flips == 3);  // stateless gate: current redelivery matches
  handler.published_engine_id.clear();
  CHECK(SendNotify(wnd, PayloadWithSeq("8")));
  CHECK(handler.flips == 3);
  handler.published_engine_id = kEngine;
  handler.published_request_id = std::string(32, '0');
  CHECK(SendNotify(wnd, PayloadWithSeq("8")));
  CHECK(handler.flips == 3);
  handler.published_request_id = kRequest;
  handler.published_seq = "not-a-number";
  CHECK(SendNotify(wnd, PayloadWithSeq("8")));
  CHECK(handler.flips == 3);
  handler.published_seq = "7";

  // failure silence under our COPYDATA id: consumed, no delivery, no crash
  const int flips_before = handler.flips;
  CHECK(SendRaw(wnd, WEASEL_PREDICT_NOTIFY_COPYDATA_ID,
                PayloadWithSeq("8")));  // missing terminating NUL
  CHECK(SendRaw(wnd, WEASEL_PREDICT_NOTIFY_COPYDATA_ID, std::string(1, '\0')));
  CHECK(SendRaw(wnd, WEASEL_PREDICT_NOTIFY_COPYDATA_ID, std::string(300, 'a')));
  CHECK(handler.flips == flips_before);
  CHECK(handler.delivered.size() == 3);

  // foreign dwData falls through untouched
  CHECK(!SendRaw(wnd, WEASEL_PREDICT_NOTIFY_COPYDATA_ID + 1,
                 Nul(PayloadWithSeq("7"))));
  CHECK(handler.flips == flips_before);

  server.Stop();
  return 0;
}

}  // namespace

int main() {
  HRESULT hRes = _Module.Init(NULL, GetModuleHandle(NULL));
  if (FAILED(hRes))
    return 2;
  RunPureParserCases();
  RunWindowCases();
  _Module.Term();
  if (g_failures) {
    std::cerr << g_failures << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "TestPredictNotify: all checks passed" << std::endl;
  return 0;
}
