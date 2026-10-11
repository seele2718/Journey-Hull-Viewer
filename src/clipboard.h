#ifndef HV_CLIPBOARD_H
#define HV_CLIPBOARD_H
/* Fetch on demand, with the owning window. Never return an earlier paste. */
static BOOL clipboard_read(HWND owner, wchar_t *text, size_t capacity) {
  text[0] = 0;
  if (!OpenClipboard(owner)) return FALSE;
  HGLOBAL data = GetClipboardData(CF_UNICODETEXT);
  SIZE_T size = data ? GlobalSize(data) / sizeof(wchar_t) : 0;
  const wchar_t *source = size ? GlobalLock(data) : NULL;
  BOOL ok = FALSE;
  if (source) {
    size_t n = 0;
    while (n < size && n < capacity && source[n]) ++n;
    if (n < size && n < capacity) {
      memcpy(text, source, (n + 1) * sizeof(wchar_t));
      ok = TRUE;
    }
    GlobalUnlock(data);
  }
  CloseClipboard();
  return ok;
}
static BOOL clipboard_write(HWND owner, const wchar_t *text) {
  size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
  HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!data) return FALSE;
  void *destination = GlobalLock(data);
  if (!destination) { GlobalFree(data); return FALSE; }
  memcpy(destination, text, bytes);
  GlobalUnlock(data);
  if (!OpenClipboard(owner)) { GlobalFree(data); return FALSE; }
  BOOL ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data);
  CloseClipboard();
  if (!ok) GlobalFree(data);
  return ok;
}
#endif
