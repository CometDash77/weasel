#pragma once
// S2 (issue #21): editor preceding-context channel.
//
// WeaselTSF publishes three RIME properties; the Lua filter refuses to act
// unless the status is exactly ok and a non-empty revision comes with it.
// "The user confirmed there is no preceding text" (ok, empty text) and
// "the editor could not be read" (unavailable) are different facts and must
// never collapse into one null.

#include <functional>
#include <string>

namespace weasel {
namespace predict_context {

// Wire property names (RIME session properties consumed by the Lua side).
const char* const kStatusProp = "model_predict_context_status";
const char* const kTextProp = "model_predict_preceding_text";
const char* const kRevisionProp = "model_predict_context_revision";

// Three-state context availability.
enum class Status {
  kUnavailable,  // nothing could be read (no document, read failure, ...)
  kOk,           // text read; empty text means confirmed empty context
  kProtected,    // protected field (password/private); text must not leak
};

// Maximum preceding text handed to the prediction chain, in Unicode
// scalar values (code points), NOT UTF-16 code units.
const size_t kMaxScalars = 300;

// UTF-16 code units fetched upstream of the caret; generous slack so that
// trimming to kMaxScalars scalars still yields a full window when the text
// is dense in supplementary planes (2 UTF-16 units per scalar).
const size_t kFetchUtf16Units = 600;

// Parse a status wire token; returns false when the token is unknown so
// callers can silently ignore malformed pushes.
bool StatusFromToken(const std::wstring& token, Status* out);
const wchar_t* StatusToken(Status status);

// Keep at most limit Unicode scalar values, taken from the END of the text
// (the part nearest the caret). Never splits a surrogate pair; a leading
// unpaired low surrogate is dropped together with its high half.
void TrimToScalars(std::wstring& text, size_t limit);

// Count Unicode scalar values (code points) in a UTF-16 string.
size_t ScalarLength(const std::wstring& text);

// True when any scope in the list marks a field whose text must never leave
// the application (password / private / PIN kinds). Deliberately free of COM
// so the protection policy itself is testable without a TSF host; scopes are
// passed as ints for the same reason.
bool IsProtectedScope(const int* scopes, size_t count);

// FNV-1a over the UTF-8 encoding of text; rendered as 16 lowercase hex
// digits. Opaque per-value revision marker for the Lua side; never derived
// from user content by anything that could invert it.
std::wstring Fnv1a64Hex(const std::wstring& text);

struct ParsedBody {
  Status status = Status::kUnavailable;
  std::wstring preceding_text;  // unescaped
  std::wstring revision;
};

// Parse the wire body written by the client for WEASEL_IPC_PREDICT_CONTEXT:
//   action=predict_context
//   predict_context.status=<token>
//   predict_context.preceding_text=<escaped>   (ok only)
//   predict_context.revision=<16 hex>          (ok only)
//   .
// Returns false when the body is malformed; callers must ignore the push
// and must never write partial properties.
bool ParseBody(const std::wstring& body, ParsedBody* out);

// Build the client-side wire body for a push. For non-ok states only the
// status line is emitted (no text, no revision) so the server can never
// overwrite stale ok text with a protected/unavailable marker.
std::wstring BuildBody(Status status,
                       const std::wstring& preceding_text_escaped,
                       const std::wstring& revision);

// True when body carries nothing new compared with the last body the
// transport accepted, so the push must be suppressed. Pure function: the
// suppression rule belongs to the publisher, not to the pipe, and is
// therefore testable without a TSF host.
bool ShouldSuppressPush(const std::wstring& last_sent_body,
                        const std::wstring& body);

// Apply a parsed push to a session in the canonical order:
// text, then revision, then status LAST so the Lua side never observes
// ok with text/revision from a previous push. set_property(key, utf8_value)
// receives UTF-8. Returns false when validation fails (nothing written).
bool ApplyToSession(const ParsedBody& parsed,
                    const std::function<bool(const char*, const char*)>&
                        set_property);

}  // namespace predict_context
}  // namespace weasel
