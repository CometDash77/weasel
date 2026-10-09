#include "stdafx.h"
#include <inputscope.h>

#include <StringAlgorithm.hpp>
#include <WeaselIPC.h>
#include <WeaselPredictContext.h>

#include <boost/interprocess/streams/bufferstream.hpp>

#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

CAppModule _Module;

using namespace weasel;
using namespace weasel::predict_context;
// disambiguate from weasel::Status (RIME status struct)

namespace {

struct Recorded {
  predict_context::Status status;
  std::wstring text;
  std::wstring revision;
};

// Mirrors the production gating of RimeWithWeaselHandler::PredictContext:
// parse first, drop unknown sessions, then apply in canonical order.
class RecordingHandler : public RequestHandler {
 public:
  DWORD FindSession(DWORD ipc_id) override {
    return ipc_id == 1 ? 1 : 0;
  }
  // Same line-oriented parse as RimeWithWeaselHandler::_ReadClientInfo, so the
  // body handed over with START_SESSION is covered end to end as well.
  DWORD AddSession(LPWSTR buffer, EatLine eat) override {
    boost::interprocess::wbufferstream bs(buffer, WEASEL_IPC_BUFFER_LENGTH);
    std::wstring line;
    while (bs.good()) {
      std::getline(bs, line);
      if (!bs.good())
        break;
      if (line == L".")
        break;
      const std::wstring kAppKey = L"session.client_app=";
      const std::wstring kTypeKey = L"session.client_type=";
      if (starts_with(line, kAppKey))
        client_app = line.substr(kAppKey.size());
      else if (starts_with(line, kTypeKey))
        client_type = line.substr(kTypeKey.size());
    }
    return 1;
  }
  void PredictContext(DWORD ipc_id, LPWSTR buffer) override {
    if (FindSession(ipc_id) == 0)
      return;
    ParsedBody parsed;
    if (!ParseBody(buffer, &parsed)) {
      ++parse_failures;
      return;
    }
    std::vector<std::string> order;
    bool ok = ApplyToSession(parsed, [&](const char* key, const char*) {
      order.push_back(key);
      return true;
    });
    assert(ok);
    records.push_back({parsed.status, parsed.preceding_text, parsed.revision});
    prop_order.push_back(order);
  }

  std::vector<Recorded> records;
  std::vector<std::vector<std::string>> prop_order;
  int parse_failures = 0;
  std::wstring client_app;
  std::wstring client_type;
};

int g_failures = 0;

void expect(bool cond, const char* what) {
  if (!cond) {
    std::wcout << L"FAIL: " << what << L"\n";
    ++g_failures;
  }
}

void expect_eq(const std::wstring& got,
               const std::wstring& want,
               const char* what) {
  if (got != want) {
    std::wcout << L"FAIL: " << what << L": got [" << got << L"] want ["
               << want << L"]\n";
    ++g_failures;
  }
}

// Poll for an asynchronous wire effect instead of racing it with a fixed
// sleep; a timeout is itself a failure.
template <typename _Pred>
void wait_until(_Pred pred, const char* what, int timeout_ms = 3000) {
  for (int waited = 0; waited < timeout_ms; waited += 20) {
    if (pred())
      return;
    Sleep(20);
  }
  std::wcout << L"TIMEOUT: " << what << std::endl;
  ++g_failures;
}

void test_scalar_primitives() {
  expect(ScalarLength(L"") == 0, "ScalarLength empty");
  expect(ScalarLength(L"abc") == 3, "ScalarLength ascii");
  std::wstring pairs;
  for (int i = 0; i < 3; ++i)
    pairs += static_cast<wchar_t>(0xD800), pairs += static_cast<wchar_t>(0xDC00);
  expect(ScalarLength(pairs) == 3, "ScalarLength surrogate pairs");

  std::wstring t = std::wstring(350, L'a');
  TrimToScalars(t, kMaxScalars);
  expect(t.size() == 300 && ScalarLength(t) == 300, "trim ascii to 300");

  // 301 astral scalars + 1 ascii = 302 scalars; keep last 300 on a clean
  // pair boundary
  std::wstring astral;
  for (int i = 0; i < 301; ++i)
    astral += static_cast<wchar_t>(0xD800), astral += static_cast<wchar_t>(0xDC00);
  astral += L"x";
  std::wstring a2 = astral;
  TrimToScalars(a2, kMaxScalars);
  expect(ScalarLength(a2) == 300, "trim astral keeps 300 scalars");
  expect(a2.size() == 600 - 2 + 1, "trim astral utf16 length");
  expect(a2[0] == 0xD800 && a2[1] == 0xDC00, "trim astral clean pair start");
  expect(a2[a2.size() - 1] == L'x', "trim astral keeps caret-side tail");

  // a leading lone low surrogate (ShiftStart splitting a pair) is its own
  // scalar and is dropped first
  std::wstring lone = std::wstring(1, static_cast<wchar_t>(0xDC00)) +
                      std::wstring(300, L'a');
  std::wstring l2 = lone;
  TrimToScalars(l2, kMaxScalars);
  expect(l2[0] == L'a' && ScalarLength(l2) == 300, "lone low surrogate dropped");

  expect_eq(Fnv1a64Hex(L""), L"cbf29ce484222325", "FNV basis for empty");
  expect(Fnv1a64Hex(L"a") != Fnv1a64Hex(L"b"), "FNV distinguishes inputs");
  expect(Fnv1a64Hex(L"a").size() == 16, "FNV hex width");
}

void test_wire_roundtrip() {
  const std::wstring rev = Fnv1a64Hex(L"hello");
  std::wstring body = BuildBody(predict_context::Status::kOk, escape_string<wchar_t>(L"a\\b\nc\rd\te"), rev);
  ParsedBody out;
  expect(ParseBody(body, &out), "parse escaped ok body");
  expect(out.status == predict_context::Status::kOk, "roundtrip status");
  expect_eq(out.preceding_text, L"a\\b\nc\rd\te", "roundtrip text");
  expect_eq(out.revision, rev, "roundtrip revision");

  body = BuildBody(predict_context::Status::kOk, L"", Fnv1a64Hex(L""));
  expect(ParseBody(body, &out) && out.status == predict_context::Status::kOk &&
             out.preceding_text.empty(),
         "confirmed empty context is ok with empty text");

  body = BuildBody(predict_context::Status::kUnavailable, L"", L"");
  expect(ParseBody(body, &out) && out.status == predict_context::Status::kUnavailable &&
             out.preceding_text.empty() && out.revision.empty(),
         "unavailable carries no fields");

  body = BuildBody(predict_context::Status::kProtected, L"", L"");
  expect(ParseBody(body, &out) && out.status == predict_context::Status::kProtected,
         "protected parses");

  // malformed bodies must be rejected
  expect(!ParseBody(L"", &out), "empty body rejected");
  expect(!ParseBody(L"action=predict_context\n.\n", &out),
         "missing status rejected");
  expect(!ParseBody(L"predict_context.status=hacked\n.\n", &out),
         "unknown status token rejected");
  expect(!ParseBody(L"predict_context.status=ok\n.\n", &out),
         "ok without text rejected");
  expect(!ParseBody(L"predict_context.status=ok\npredict_context.preceding_text=x\n.\n", &out),
         "ok without revision rejected");
  expect(!ParseBody(L"predict_context.status=ok\npredict_context.preceding_text=x\npredict_context.revision=" +
                        std::wstring(65, L'0') + L"\n.\n",
                    &out),
         "over-long revision rejected");
  // non-ok cannot smuggle text
  expect(ParseBody(L"predict_context.status=protected\npredict_context.preceding_text=secret\npredict_context.revision=abc\n.\n", &out) &&
             out.preceding_text.empty() && out.revision.empty(),
         "protected strips stray fields");

  // deepest defence: a hostile client that skips its own trim still cannot
  // push more than kMaxScalars past the server
  std::wstring overlong(700, L'a');
  body = BuildBody(predict_context::Status::kOk, overlong, Fnv1a64Hex(L"x"));
  expect(ParseBody(body, &out), "over-long ok body still parses");
  expect(ScalarLength(out.preceding_text) == kMaxScalars,
         "server re-trims an over-long push to 300 scalars");

  std::wstring astral_overlong;
  for (int i = 0; i < 400; ++i)
    astral_overlong += static_cast<wchar_t>(0xD800),
        astral_overlong += static_cast<wchar_t>(0xDC00);
  body = BuildBody(predict_context::Status::kOk, astral_overlong,
                   Fnv1a64Hex(L"x"));
  expect(ParseBody(body, &out) && ScalarLength(out.preceding_text) == kMaxScalars,
         "server re-trims astral pushes on a clean pair boundary");
  expect(out.preceding_text.size() == 600 &&
             out.preceding_text[0] == 0xD800 &&
             out.preceding_text[1] == 0xDC00,
         "server re-trim never splits a surrogate pair");

  // status is mandatory even when every other line is well formed
  expect(!ParseBody(L"predict_context.preceding_text=x\npredict_context.revision=y\n.\n", &out),
         "text without status rejected");

  // The line-oriented format makes the payload position-independent: a body
  // whose first line is damaged still parses. This is why a misaligned body
  // pointer in the pipe layer (the ReceiveBuffer() offset bug) could survive
  // unnoticed on the session-info path and only surface on the strict push
  // path, where the assertions compare the exact text and property order.
  expect(ParseBody(L"=predict_context\npredict_context.status=ok\n"
                   L"predict_context.preceding_text=x\n"
                   L"predict_context.revision=abc\n.\n",
                   &out) &&
             out.status == predict_context::Status::kOk &&
             out.preceding_text == L"x" && out.revision == L"abc",
         "a damaged first line does not derail the parse");
}

// AC4: the protection policy itself, with no COM involved. Every protected
// field kind must be recognised, and ordinary prose must not be.
void test_protection_policy() {
  const int kProtectedKinds[] = {IS_PASSWORD,          IS_PRIVATE,
                                 IS_NUMERIC_PASSWORD,  IS_NUMERIC_PIN,
                                 IS_ALPHANUMERIC_PIN,  IS_ALPHANUMERIC_PIN_SET};
  for (int kind : kProtectedKinds) {
    expect(IsProtectedScope(&kind, 1), "policy: protected scope recognised");
  }
  const int kAllowedKinds[] = {IS_DEFAULT, IS_URL, IS_EMAIL_SMTPEMAILADDRESS};
  for (int kind : kAllowedKinds) {
    expect(!IsProtectedScope(&kind, 1), "policy: ordinary scope allowed");
  }
  // any hit in the list protects the whole field
  const int mixed[] = {IS_DEFAULT, IS_URL, IS_PASSWORD};
  expect(IsProtectedScope(mixed, 3), "policy: mixed scope list is protected");
  const int only_allowed[] = {IS_DEFAULT, IS_URL};
  expect(!IsProtectedScope(only_allowed, 2), "policy: all-allowed list passes");
  // degenerate inputs must fail open as "not protected" without crashing
  expect(!IsProtectedScope(nullptr, 4), "policy: null scope list is not protected");
  expect(!IsProtectedScope(mixed, 0), "policy: empty scope list is not protected");
}

// Publisher-side rule, testable without a TSF host: only a byte-identical
// repeat of the last body the transport accepted is dropped.
void test_push_suppression_rule() {
  const std::wstring ok_empty =
      BuildBody(predict_context::Status::kOk, L"", Fnv1a64Hex(L""));
  const std::wstring ok_a =
      BuildBody(predict_context::Status::kOk, L"a", Fnv1a64Hex(L"a"));
  const std::wstring unavailable =
      BuildBody(predict_context::Status::kUnavailable, L"", L"");
  const std::wstring protected_ =
      BuildBody(predict_context::Status::kProtected, L"", L"");

  expect(!ShouldSuppressPush(L"", ok_empty),
         "suppress: first body is always published");
  expect(ShouldSuppressPush(ok_empty, ok_empty),
         "suppress: identical repeat is dropped");
  expect(!ShouldSuppressPush(ok_empty, ok_a),
         "suppress: new content is published");
  expect(!ShouldSuppressPush(ok_empty, unavailable) &&
             !ShouldSuppressPush(unavailable, protected_),
         "suppress: state transitions are always published");
  expect(ShouldSuppressPush(protected_, protected_),
         "suppress: repeated non-ok marker is dropped");
  expect(!ShouldSuppressPush(protected_, L""),
         "suppress: an empty candidate body is never treated as a repeat");
}

// one client session against an isolated server, covering the wire path
void run_ipc_tests() {
  wchar_t name[80];
  swprintf_s(name, L"\\\\.\\pipe\\WeaselTestPredict-%lu",
             static_cast<unsigned long>(GetCurrentProcessId()));
  const std::wstring pipe_name = name;

  // AC3/AC4: a push on a client with no live server is a silent false, never a
  // throw and never a block - ordinary key handling keeps working (it
  // reconnects on the next keystroke).
  {
    Client detached(pipe_name);
    expect(!detached.UpdatePredictContext(predict_context::Status::kOk, L"x",
                                          Fnv1a64Hex(L"x")),
           "wire: push without a connection fails silently");
  }

  RecordingHandler handler;
  Server server(pipe_name);
  server.SetRequestHandler(&handler);
  std::thread server_thread([&server] { server.Run(); });

  Client client(pipe_name);
  // The server thread needs a moment to create its first pipe instance; poll
  // for it instead of sleeping a guessed interval.
  wait_until(
      [&] {
        HANDLE probe = ::CreateFileW(pipe_name.c_str(),
                                     GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                     OPEN_EXISTING, 0, NULL);
        if (probe == INVALID_HANDLE_VALUE)
          return false;
        ::CloseHandle(probe);
        return true;
      },
      "server pipe instance created");

  // Echo() is false before StartSession by design (_Active() requires
  // session_id != 0), so only Connect() gates this loop
  bool connected = false;
  for (int i = 0; i < 100 && !connected; ++i) {
    Sleep(20);
    connected = client.Connect();
  }
  expect(connected, "client connected to isolated pipe");
  if (!connected) {
    PostThreadMessageW(GetThreadId(server_thread.native_handle()), WM_QUIT, 0, 0);
    server_thread.join();
    return;
  }
  client.StartSession();
  expect(client.Echo(), "echo roundtrip with active session");
  // START_SESSION carries its body over the very same request channel, so
  // parsing it end to end covers the request-body path independently
  expect(handler.client_app == L"testpredictcontext.exe" &&
             handler.client_type == L"tsf",
         "wire: session body parsed off the request channel");

  // A live push: the handler must observe exactly one push, applying text,
  // then revision, then status last
  const std::wstring rev = Fnv1a64Hex(L"");
  expect(client.UpdatePredictContext(predict_context::Status::kOk, L"", rev),
         "wire: push accepted by the server");
  wait_until([&] { return handler.records.size() >= 1; }, "first push arrives");
  expect(handler.records.size() == 1, "wire: exactly one push recorded");
  expect(handler.records[0].status == predict_context::Status::kOk &&
             handler.records[0].text.empty() &&
             handler.records[0].revision == rev,
         "wire: confirmed empty context");
  expect(handler.prop_order[0].size() == 3 &&
             handler.prop_order[0][0] == kTextProp &&
             handler.prop_order[0][1] == kRevisionProp &&
             handler.prop_order[0][2] == kStatusProp,
         "wire: status property applied last");

  // AC1: 300-scalar content survives the wire byte-for-byte
  std::wstring t = std::wstring(350, L'a');
  TrimToScalars(t, kMaxScalars);
  expect(ScalarLength(t) == 300, "fixture: 300 scalars after trim");
  expect(client.UpdatePredictContext(predict_context::Status::kOk, t, Fnv1a64Hex(t)),
         "wire: 300-scalar push accepted");
  wait_until([&] { return handler.records.size() >= 2; }, "300-scalar push arrives");
  expect(handler.records.size() == 2, "wire: second push recorded once");
  expect_eq(handler.records[1].text, t, "wire: 300 scalars roundtrip");
  expect(handler.records[1].revision == Fnv1a64Hex(t), "wire: revision");
  expect(handler.prop_order[1][0] == kTextProp &&
             handler.prop_order[1][1] == kRevisionProp,
         "wire: text then revision then status");

  // escaping roundtrip over the real pipe
  const std::wstring tricky = L"line1\nline2\ttab \\ back\r";
  expect(client.UpdatePredictContext(predict_context::Status::kOk, tricky,
                                     Fnv1a64Hex(tricky)),
         "wire: escaping push accepted");
  wait_until([&] { return handler.records.size() >= 3; }, "escaping push arrives");
  expect(handler.records.size() == 3, "wire: third push recorded once");
  expect_eq(handler.records[2].text, tricky, "wire: escaping roundtrip");

  // non-ok tri-states carry no text over the wire
  expect(client.UpdatePredictContext(predict_context::Status::kUnavailable, L"", L"") &&
             client.UpdatePredictContext(predict_context::Status::kProtected, L"", L""),
         "wire: non-ok pushes accepted");
  wait_until([&] { return handler.records.size() >= 5; }, "non-ok pushes arrive");
  expect(handler.records.size() == 5, "wire: both non-ok pushes recorded");
  expect(handler.records[3].status == predict_context::Status::kUnavailable &&
             handler.records[3].text.empty(),
         "wire: unavailable textless");
  expect(handler.records[4].status == predict_context::Status::kProtected &&
             handler.records[4].text.empty() &&
             handler.records[4].revision.empty(),
         "wire: protected textless");
  expect(handler.prop_order[4].size() == 1 &&
             handler.prop_order[4][0] == kStatusProp,
         "wire: protected only writes status");

  // The same non-ok push repeated is a real state change delivery: the server
  // records it again (the publisher is the layer that suppresses repeats)
  expect(client.UpdatePredictContext(predict_context::Status::kProtected, L"", L""),
         "wire: repeated non-ok push accepted");
  wait_until([&] { return handler.records.size() >= 6; }, "repeated non-ok push arrives");
  expect(handler.records.size() == 6 &&
             handler.records[5].status == predict_context::Status::kProtected,
         "wire: same non-ok status is re-delivered, not swallowed");

  // EndSession stops the client from pushing anything further
  client.EndSession();
  expect(!client.UpdatePredictContext(predict_context::Status::kOk, L"gone",
                                      Fnv1a64Hex(L"gone")),
         "wire: push refused after EndSession");
  Sleep(150);
  expect(handler.records.size() == 6, "wire: no push after EndSession");

  // unknown sessions are dropped before any property is written
  handler.PredictContext(999, const_cast<LPWSTR>(
                                  BuildBody(predict_context::Status::kOk, L"x", rev)
                                      .c_str()));
  expect(handler.records.size() == 6, "handler: unknown session dropped");

  // malformed push bodies fail parsing, never write properties
  handler.PredictContext(1, const_cast<LPWSTR>(L"garbage\n.\n"));
  expect(handler.parse_failures == 1 && handler.records.size() == 6,
         "handler: malformed body dropped");
  expect(handler.parse_failures == 1, "wire: live pushes all parsed cleanly");

  client.Disconnect();
  PostThreadMessageW(GetThreadId(server_thread.native_handle()), WM_QUIT, 0, 0);
  server_thread.join();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
  // make wcout visible when launched from a console host
  if (!AttachConsole(ATTACH_PARENT_PROCESS))
    AllocConsole();
  _Module.Init(NULL, hInst);
  test_scalar_primitives();
  test_wire_roundtrip();
  test_protection_policy();
  test_push_suppression_rule();
  run_ipc_tests();
  _Module.Term();
  if (g_failures) {
    std::wcout << g_failures << L" failure(s)\n";
    return 1;
  }
  std::wcout << L"all tests passed\n";
  return 0;
}
