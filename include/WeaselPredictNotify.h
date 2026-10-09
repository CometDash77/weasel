#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

// S3 completion-notify receiver contract (RIME-PredictInput issue #22).
//
// After a prediction response lands, the sidecar sends one WM_COPYDATA to
// the server's private IPC window (class WeaselIPCWindow_1.0):
//
//   COPYDATASTRUCT{ dwData = WEASEL_PREDICT_NOTIFY_COPYDATA_ID,
//                   cbData = payload byte count (terminating NUL included),
//                   lpData = "<engine_id>\n<request_id>\n<seq>" UTF-8 + NUL }
//
// The payload carries the three request-identity IDs only: no user text,
// pinyin, candidates or model answers ever crosses this channel. On x64
// COPYDATASTRUCT is 24 bytes. Malformed or identity-mismatched notifications
// are rejected silently: the notice only accelerates the next recompose, it
// never carries nor alters composition content.

#define WEASEL_PREDICT_NOTIFY_COPYDATA_ID 0x52504D31

namespace weasel {

// Payload grammar: engine_id = 40 lowercase hex chars (byte-exact with the
// TS sender's HEX_40), request_id = 32 lowercase hex chars, seq = decimal in
// [1, 2^53-1] without sign or leading zeros. Three segments joined with
// '\n', UTF-8 bytes, exactly one terminating NUL counted in cbData.
constexpr size_t kPredictNotifyMaxCbData = 256;
constexpr uint64_t kPredictNotifyMaxSeq = 9007199254740991ull;  // 2^53 - 1

// RIME session properties that publish the pending request identity. The
// Lua component writes them when it assembles a request (the same property
// channel the editor-context feature uses); the receiver reads them back at
// notify time and keeps no state of its own.
inline constexpr char kPredictEngineIdProperty[] = "model_predict_engine_id";
inline constexpr char kPredictRequestIdProperty[] = "model_predict_request_id";
inline constexpr char kPredictRequestSeqProperty[] =
    "model_predict_request_seq";

// Private option flipped once per accepted notification. librime's
// ConcreteEngine::OnOptionUpdate refreshes the non-confirmed composition on
// any option update, which recomposes and lets the Lua filter surface the
// freshly landed response without waiting for the next keypress.
inline constexpr char kPredictRefreshOption[] = "model_predict_refresh";

struct PredictNotify {
  std::string engine_id;
  std::string request_id;
  uint64_t seq;
};

inline bool IsLowerHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

inline bool IsValidEngineId(std::string_view id) {
  if (id.size() != 40)
    return false;
  for (char c : id) {
    if (!IsLowerHexDigit(c))
      return false;
  }
  return true;
}

inline bool IsValidRequestId(std::string_view id) {
  if (id.size() != 32)
    return false;
  for (char c : id) {
    if (!IsLowerHexDigit(c))
      return false;
  }
  return true;
}

// Strict seq grammar: ^[1-9][0-9]{0,15}$ with value <= 2^53-1.
inline std::optional<uint64_t> ParsePredictSeq(std::string_view seq) {
  if (seq.empty() || seq.size() > 16 || seq.front() < '1' ||
      seq.front() > '9')
    return std::nullopt;
  uint64_t value = 0;
  for (char c : seq) {
    if (c < '0' || c > '9')
      return std::nullopt;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  if (value > kPredictNotifyMaxSeq)
    return std::nullopt;
  return value;
}

// Parses the COPYDATA payload. Strict: cbData must cover the payload plus
// exactly one terminating NUL (no embedded NUL, no trailing bytes), and the
// body must split into exactly three newline-separated valid segments.
inline std::optional<PredictNotify> ParsePredictNotifyPayload(
    const char* data,
    size_t cb_data) {
  if (!data || cb_data < 2 || cb_data > kPredictNotifyMaxCbData)
    return std::nullopt;
  if (data[cb_data - 1] != '\0')
    return std::nullopt;
  if (std::memchr(data, '\0', cb_data - 1) != nullptr)
    return std::nullopt;
  std::string_view payload(data, cb_data - 1);
  size_t first = payload.find('\n');
  if (first == std::string_view::npos)
    return std::nullopt;
  size_t second = payload.find('\n', first + 1);
  if (second == std::string_view::npos)
    return std::nullopt;
  if (payload.find('\n', second + 1) != std::string_view::npos)
    return std::nullopt;
  PredictNotify notify;
  notify.engine_id = std::string(payload.substr(0, first));
  notify.request_id =
      std::string(payload.substr(first + 1, second - first - 1));
  auto seq = ParsePredictSeq(payload.substr(second + 1));
  if (!IsValidEngineId(notify.engine_id) ||
      !IsValidRequestId(notify.request_id) || !seq)
    return std::nullopt;
  notify.seq = *seq;
  return notify;
}

// Stateless identity gate: true only when the published identity is itself
// well-formed and equals the notified identity. Expired or forged notices
// fail the comparison and are dropped by the caller; a redelivery of the
// current identity matches again (the fork deliberately keeps no state).
inline bool PredictIdentityMatches(std::string_view published_engine_id,
                                   std::string_view published_request_id,
                                   std::string_view published_seq,
                                   const PredictNotify& notify) {
  if (!IsValidEngineId(published_engine_id) ||
      published_engine_id != notify.engine_id)
    return false;
  if (!IsValidRequestId(published_request_id) ||
      published_request_id != notify.request_id)
    return false;
  auto seq = ParsePredictSeq(published_seq);
  return seq.has_value() && *seq == notify.seq;
}

}  // namespace weasel
