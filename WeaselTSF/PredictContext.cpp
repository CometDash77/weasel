#include "stdafx.h"
#include "WeaselTSF.h"
#include <inputscope.h>
#include <oleauto.h>
#include <WeaselPredictContext.h>

using namespace weasel;

// inputscope.h declares GUID_PROP_INPUTSCOPE extern; this project does not
// link the SDK lib that defines it, so provide the definition (inputscope.h).
EXTERN_C const GUID GUID_PROP_INPUTSCOPE = {
    0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

namespace {

/* TSF-native protection signal: apps publish the focused field kind as an
   InputScope property; password/PIN/private scopes must not be read. */
bool IsProtectedInputScope(com_ptr<ITfContext> pContext,
                           TfEditCookie ec,
                           ITfRange* pRange) {
  com_ptr<ITfProperty> pProperty;
  if (pContext->GetProperty(GUID_PROP_INPUTSCOPE, &pProperty) != S_OK ||
      pProperty == nullptr)
    return false;

  VARIANT var;
  VariantInit(&var);
  bool is_protected = false;
  if (pProperty->GetValue(ec, pRange, &var) == S_OK) {
    if (var.vt == VT_UNKNOWN && var.punkVal != nullptr) {
      ITfInputScope* pInputScope = nullptr;
      if (var.punkVal->QueryInterface(IID_ITfInputScope,
                                      (void**)&pInputScope) == S_OK &&
          pInputScope != nullptr) {
        InputScope* pScopes = nullptr;
        UINT cScopes = 0;
        if (pInputScope->GetInputScopes(&pScopes, &cScopes) == S_OK &&
            pScopes != nullptr) {
          is_protected = predict_context::IsProtectedScope(
              reinterpret_cast<const int*>(pScopes), cScopes);
          CoTaskMemFree(pScopes);
        }
        pInputScope->Release();
      }
    }
    VariantClear(&var);
  }
  return is_protected;
}

/* Fallback for classic edit controls that never publish InputScope:
   the ES_PASSWORD style marks masked edits. */
bool IsProtectedFocusWindow() {
  GUITHREADINFO gti = {sizeof(GUITHREADINFO)};
  if (!GetGUIThreadInfo(GetCurrentThreadId(), &gti) ||
      gti.hwndFocus == nullptr)
    return false;
  LONG_PTR style = GetWindowLongPtrW(gti.hwndFocus, GWL_STYLE);
  return (style & ES_PASSWORD) != 0;
}

}  // namespace

void WeaselTSF::_UpdatePredictContext(com_ptr<ITfContext> pContext,
                                      TfEditCookie ec) {
  if (pContext == nullptr)
    return;

  /* anchor: composition start while composing, caret otherwise */
  com_ptr<ITfRange> pRange;
  if (_IsComposing() && _pComposition != nullptr &&
      _pComposition->GetRange(&pRange) != S_OK)
    pRange.Release();

  if (pRange == nullptr) {
    TF_SELECTION sel = {};
    ULONG cFetched = 0;
    if (pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel,
                               &cFetched) != S_OK ||
        cFetched != 1) {
      _SendPredictContext(predict_context::Status::kUnavailable, L"", L"");
      return;
    }
    pRange.Attach(sel.range);
  }

  if (pRange->Collapse(ec, TF_ANCHOR_START) != S_OK) {
    _SendPredictContext(predict_context::Status::kUnavailable, L"", L"");
    return;
  }

  /* walk left up to kFetchUtf16Units UTF-16 units, trim to 300 scalars */
  LONG moved = 0;
  if (pRange->ShiftStart(ec, -(LONG)predict_context::kFetchUtf16Units, &moved,
                         nullptr) != S_OK) {
    _SendPredictContext(predict_context::Status::kUnavailable, L"", L"");
    return;
  }

  /* check protection before extracting any text */
  if (IsProtectedInputScope(pContext, ec, pRange) ||
      IsProtectedFocusWindow()) {
    _SendPredictContext(predict_context::Status::kProtected, L"", L"");
    return;
  }

  WCHAR buf[predict_context::kFetchUtf16Units + 1] = {};
  ULONG fetched = 0;
  if (pRange->GetText(ec, 0, buf, predict_context::kFetchUtf16Units,
                      &fetched) != S_OK) {
    _SendPredictContext(predict_context::Status::kUnavailable, L"", L"");
    return;
  }

  std::wstring text(buf, fetched);
  predict_context::TrimToScalars(text, predict_context::kMaxScalars);
  std::wstring revision = predict_context::Fnv1a64Hex(text);
  _SendPredictContext(predict_context::Status::kOk, text, revision);
}

void WeaselTSF::_SendPredictContext(
    predict_context::Status status,
    const std::wstring& preceding_text,
    const std::wstring& revision) {
  const std::wstring body = predict_context::BuildBody(
      status, escape_string<wchar_t>(preceding_text), revision);
  if (predict_context::ShouldSuppressPush(_lastPredictBody, body))
    return;
  /* keep the last successfully-sent body so failed pushes get retried */
  if (m_client.UpdatePredictContext(status, preceding_text, revision))
    _lastPredictBody = body;
}
