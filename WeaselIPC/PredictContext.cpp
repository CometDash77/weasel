#include "stdafx.h"
#include <WeaselPredictContext.h>
#include <StringAlgorithm.hpp>
#include <WeaselUtility.h>
#include <inputscope.h>

namespace weasel {
namespace predict_context {

const wchar_t* StatusToken(Status status) {
  switch (status) {
    case Status::kOk:
      return L"ok";
    case Status::kProtected:
      return L"protected";
    case Status::kUnavailable:
    default:
      return L"unavailable";
  }
}

bool StatusFromToken(const std::wstring& token, Status* out) {
  if (token == L"ok") {
    *out = Status::kOk;
    return true;
  }
  if (token == L"unavailable") {
    *out = Status::kUnavailable;
    return true;
  }
  if (token == L"protected") {
    *out = Status::kProtected;
    return true;
  }
  return false;
}

bool IsProtectedScope(const int* scopes, size_t count) {
  // Field kinds whose text must not be published (SDK inputscope.h).
  static const int kProtected[] = {
      IS_PASSWORD,           IS_PRIVATE,          IS_NUMERIC_PASSWORD,
      IS_NUMERIC_PIN,        IS_ALPHANUMERIC_PIN, IS_ALPHANUMERIC_PIN_SET,
  };
  if (scopes == nullptr)
    return false;
  for (size_t i = 0; i < count; ++i) {
    for (int protected_scope : kProtected) {
      if (scopes[i] == protected_scope)
        return true;
    }
  }
  return false;
}

size_t ScalarLength(const std::wstring& text) {
  size_t scalars = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    wchar_t ch = text[i];
    if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < text.size() &&
        text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
      ++i;  // surrogate pair counts as one scalar
    }
    ++scalars;
  }
  return scalars;
}

void TrimToScalars(std::wstring& text, size_t limit) {
  if (limit == 0) {
    text.clear();
    return;
  }
  size_t scalars = ScalarLength(text);
  if (scalars <= limit)
    return;
  // Drop (scalars - limit) scalars from the FRONT with pair discipline so
  // the kept suffix is exactly limit scalars on a clean boundary.
  size_t start = 0;
  size_t dropped = 0;
  while (dropped < scalars - limit && start < text.size()) {
    wchar_t ch = text[start];
    if (ch >= 0xD800 && ch <= 0xDBFF && start + 1 < text.size() &&
        text[start + 1] >= 0xDC00 && text[start + 1] <= 0xDFFF) {
      start += 2;  // consume the pair as one scalar
    } else {
      start += 1;
    }
    ++dropped;
  }
  text.erase(0, start);
}

std::wstring Fnv1a64Hex(const std::wstring& text) {
  const std::string utf8 = wtou8(text);
  const uint64_t kPrime = 0x100000001b3ULL;
  uint64_t hash = 0xcbf29ce484222325ULL;  // FNV offset basis
  for (unsigned char c : utf8) {
    hash ^= c;
    hash *= kPrime;
  }
  wchar_t buf[17];
  for (int i = 0; i < 16; ++i) {
    unsigned nibble = static_cast<unsigned>((hash >> ((15 - i) * 4)) & 0xF);
    buf[i] = nibble < 10 ? L'0' + nibble : L'a' + (nibble - 10);
  }
  buf[16] = L'\0';
  return std::wstring(buf);
}

bool ParseBody(const std::wstring& body, ParsedBody* out) {
  if (body.empty())
    return false;
  bool have_status = false;
  bool have_text = false;
  bool have_revision = false;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t nl = body.find(L'\n', pos);
    if (nl == std::wstring::npos)
      nl = body.size();
    const std::wstring line = body.substr(pos, nl - pos);
    pos = nl + 1;
    if (line == L".")
      break;
    if (line.empty())
      continue;
    const std::wstring kAction = L"action=predict_context";
    const std::wstring kStatus = L"predict_context.status=";
    const std::wstring kText = L"predict_context.preceding_text=";
    const std::wstring kRevision = L"predict_context.revision=";
    if (line == kAction) {
      continue;
    } else if (starts_with(line, kStatus)) {
      if (!StatusFromToken(line.substr(kStatus.size()), &out->status))
        return false;
      have_status = true;
    } else if (starts_with(line, kText)) {
      out->preceding_text = unescape_string<wchar_t>(line.substr(kText.size()));
      have_text = true;
    } else if (starts_with(line, kRevision)) {
      out->revision = line.substr(kRevision.size());
      have_revision = true;
    } else {
      // unknown line: ignore, mirrors _ReadClientInfo
    }
  }
  if (!have_status)
    return false;
  if (out->status == Status::kOk) {
    // ok pushes must carry text and a bounded non-empty revision
    if (!have_text || !have_revision)
      return false;
    if (out->revision.empty() || out->revision.size() > 64)
      return false;
    // defensive re-trim on the server side; pushes are already trimmed
    if (ScalarLength(out->preceding_text) > kMaxScalars)
      TrimToScalars(out->preceding_text, kMaxScalars);
  } else {
    // non-ok pushes carry only the status line; strip stray fields so a
    // malicious client cannot piggyback text onto an unavailable marker
    out->preceding_text.clear();
    out->revision.clear();
  }
  return true;
}

std::wstring BuildBody(Status status,
                       const std::wstring& preceding_text_escaped,
                       const std::wstring& revision) {
  std::wstring body;
  body += L"action=predict_context\n";
  body += L"predict_context.status=";
  body += StatusToken(status);
  body += L"\n";
  if (status == Status::kOk) {
    body += L"predict_context.preceding_text=";
    body += preceding_text_escaped;
    body += L"\n";
    body += L"predict_context.revision=";
    body += revision;
    body += L"\n";
  }
  body += L".\n";
  return body;
}

bool ShouldSuppressPush(const std::wstring& last_sent_body,
                         const std::wstring& body) {
  // No accepted body yet, or an empty candidate: never suppress. BuildBody
  // always emits a non-empty body, so an empty value means "nothing to say".
  return !body.empty() && body == last_sent_body;
}

bool ApplyToSession(
    const ParsedBody& parsed,
    const std::function<bool(const char*, const char*)>& set_property) {
  if (!set_property)
    return false;
  if (parsed.status == Status::kOk) {
    if (parsed.revision.empty() || parsed.revision.size() > 64)
      return false;
    const std::string text_utf8 = wtou8(parsed.preceding_text);
    const std::string revision_utf8 = wtou8(parsed.revision);
    // canonical order: text, revision, status LAST so the Lua side never
    // observes ok with text/revision left over from a previous push
    if (!set_property(kTextProp, text_utf8.c_str()))
      return false;
    if (!set_property(kRevisionProp, revision_utf8.c_str()))
      return false;
  }
  return set_property(kStatusProp, wtou8(StatusToken(parsed.status)).c_str());
}

}  // namespace predict_context
}  // namespace weasel
