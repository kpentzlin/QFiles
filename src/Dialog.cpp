#include "Dialog.h"
#include "App.h"
#include "Util.h"

namespace qf {

// DLGTEMPLATEEX-Aufbau:
// WORD dlgVer=1, WORD signature=0xFFFF, DWORD helpID, DWORD exStyle, DWORD style, WORD cDlgItems,
// short x,y,cx,cy, menu(sz_Or_Ord), windowClass(sz_Or_Ord), title(WCHAR[]),
// [DS_SETFONT] WORD pointsize, WORD weight, BYTE italic, BYTE charset, WCHAR typeface[]
// Elemente (DWORD-ausgerichtet): DWORD helpID, DWORD exStyle, DWORD style, short x,y,cx,cy, DWORD id,
// windowClass(sz_Or_Ord), title(sz_Or_Ord), WORD extraCount

DialogTemplate::DialogTemplate(const std::wstring& title, int cx, int cy, DWORD style, DWORD exStyle) {
    Word(1);
    Word(0xFFFF);
    DWord(0);
    DWord(exStyle);
    DWord(style | DS_SETFONT);
    countOffset_ = buf_.size();
    Word(0);
    Word(0);
    Word(0);
    Word((WORD)cx);
    Word((WORD)cy);
    Word(0); // kein Menü
    Word(0); // Standard-Dialogklasse
    Str(title);
    Word(9);       // Punktgröße
    Word(FW_NORMAL);
    buf_.push_back(0); // italic
    buf_.push_back(DEFAULT_CHARSET);
    Str(L"Segoe UI");
}

void DialogTemplate::Align() {
    while (buf_.size() % 4) buf_.push_back(0);
}
void DialogTemplate::Word(WORD w) {
    buf_.push_back((uint8_t)(w & 0xFF));
    buf_.push_back((uint8_t)(w >> 8));
}
void DialogTemplate::DWord(DWORD d) {
    Word((WORD)(d & 0xFFFF));
    Word((WORD)(d >> 16));
}
void DialogTemplate::Str(const std::wstring& s) {
    for (wchar_t c : s) Word((WORD)c);
    Word(0);
}

void DialogTemplate::AddControl(int id, const wchar_t* className, WORD atom, const std::wstring& text, int x, int y,
                                int cx, int cy, DWORD style, DWORD exStyle) {
    Align();
    DWord(0);
    DWord(exStyle);
    DWord(style | WS_CHILD | WS_VISIBLE);
    Word((WORD)x);
    Word((WORD)y);
    Word((WORD)cx);
    Word((WORD)cy);
    DWord((DWORD)id);
    if (className) {
        Str(className);
    } else {
        Word(0xFFFF);
        Word(atom);
    }
    Str(text);
    Word(0);
    ++count_;
    buf_[countOffset_] = (uint8_t)(count_ & 0xFF);
    buf_[countOffset_ + 1] = (uint8_t)(count_ >> 8);
}

void DialogTemplate::Add(int id, const wchar_t* className, const std::wstring& text, int x, int y, int cx, int cy,
                         DWORD style, DWORD exStyle) {
    AddControl(id, className, 0, text, x, y, cx, cy, style, exStyle);
}

void DialogTemplate::Label(int id, const std::wstring& text, int x, int y, int cx, int cy, DWORD extraStyle) {
    AddControl(id, nullptr, 0x0082, text, x, y, cx, cy, SS_LEFT | SS_NOPREFIX | extraStyle, 0);
}
void DialogTemplate::Edit(int id, int x, int y, int cx, int cy, DWORD extraStyle, DWORD exStyle) {
    AddControl(id, nullptr, 0x0081, L"", x, y, cx, cy, WS_TABSTOP | extraStyle, exStyle);
}
void DialogTemplate::MultiEdit(int id, int x, int y, int cx, int cy, DWORD extraStyle) {
    AddControl(id, nullptr, 0x0081, L"", x, y, cx, cy,
               WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | extraStyle, WS_EX_CLIENTEDGE);
}
void DialogTemplate::Button(int id, const std::wstring& text, int x, int y, int cx, int cy, DWORD extraStyle) {
    AddControl(id, nullptr, 0x0080, text, x, y, cx, cy, WS_TABSTOP | BS_PUSHBUTTON | extraStyle, 0);
}
void DialogTemplate::DefButton(int id, const std::wstring& text, int x, int y, int cx, int cy) {
    AddControl(id, nullptr, 0x0080, text, x, y, cx, cy, WS_TABSTOP | BS_DEFPUSHBUTTON, 0);
}
void DialogTemplate::Check(int id, const std::wstring& text, int x, int y, int cx, int cy, DWORD extraStyle) {
    AddControl(id, nullptr, 0x0080, text, x, y, cx, cy, WS_TABSTOP | BS_AUTOCHECKBOX | extraStyle, 0);
}
void DialogTemplate::Radio(int id, const std::wstring& text, int x, int y, int cx, int cy, bool firstInGroup) {
    AddControl(id, nullptr, 0x0080, text, x, y, cx, cy, BS_AUTORADIOBUTTON | (firstInGroup ? (WS_GROUP | WS_TABSTOP) : 0),
               0);
}
void DialogTemplate::Group(int id, const std::wstring& text, int x, int y, int cx, int cy) {
    AddControl(id, nullptr, 0x0080, text, x, y, cx, cy, BS_GROUPBOX, 0);
}
void DialogTemplate::Combo(int id, int x, int y, int cx, int cy, bool editable, DWORD extraStyle) {
    AddControl(id, nullptr, 0x0085, L"", x, y, cx, cy,
               WS_TABSTOP | WS_VSCROLL | (editable ? CBS_DROPDOWN | CBS_AUTOHSCROLL : CBS_DROPDOWNLIST) | extraStyle, 0);
}
void DialogTemplate::List(int id, int x, int y, int cx, int cy, DWORD extraStyle) {
    AddControl(id, nullptr, 0x0083, L"", x, y, cx, cy, WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | extraStyle,
               WS_EX_CLIENTEDGE);
}
void DialogTemplate::ListView(int id, int x, int y, int cx, int cy, DWORD extraStyle) {
    AddControl(id, WC_LISTVIEWW, 0, L"", x, y, cx, cy, WS_TABSTOP | WS_BORDER | extraStyle, 0);
}
void DialogTemplate::TreeView(int id, int x, int y, int cx, int cy, DWORD extraStyle) {
    AddControl(id, WC_TREEVIEWW, 0, L"", x, y, cx, cy,
               WS_TABSTOP | WS_BORDER | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | extraStyle, 0);
}
void DialogTemplate::Progress(int id, int x, int y, int cx, int cy) {
    AddControl(id, PROGRESS_CLASSW, 0, L"", x, y, cx, cy, 0, 0);
}
void DialogTemplate::DateTime(int id, int x, int y, int cx, int cy, DWORD extraStyle) {
    AddControl(id, DATETIMEPICK_CLASSW, 0, L"", x, y, cx, cy, WS_TABSTOP | extraStyle, 0);
}
void DialogTemplate::UpDown(int id, int x, int y, int cx, int cy, DWORD extraStyle) {
    AddControl(id, UPDOWN_CLASSW, 0, L"", x, y, cx, cy, extraStyle, 0);
}
void DialogTemplate::Tab(int id, int x, int y, int cx, int cy) {
    AddControl(id, WC_TABCONTROLW, 0, L"", x, y, cx, cy, WS_TABSTOP | WS_CLIPSIBLINGS, 0);
}
void DialogTemplate::Custom(int id, const wchar_t* className, int x, int y, int cx, int cy, DWORD style, DWORD exStyle) {
    AddControl(id, className, 0, L"", x, y, cx, cy, style, exStyle);
}

// ===================== DialogBase =====================

INT_PTR DialogBase::DoModal(HWND owner, const DialogTemplate& t) {
    modeless_ = false;
    deleteOnClose_ = false;
    return DialogBoxIndirectParamW(App::Instance(), t.Data(), owner, StaticProc, (LPARAM)this);
}

HWND DialogBase::CreateModeless(HWND owner, const DialogTemplate& t, bool deleteOnClose) {
    modeless_ = true;
    deleteOnClose_ = deleteOnClose;
    HWND h = CreateDialogIndirectParamW(App::Instance(), t.Data(), owner, StaticProc, (LPARAM)this);
    if (h) {
        App::RegisterModeless(h);
        ShowWindow(h, SW_SHOW);
    }
    return h;
}

void DialogBase::End(INT_PTR result) {
    if (modeless_)
        DestroyWindow(hwnd_);
    else
        EndDialog(hwnd_, result);
}

BOOL DialogBase::OnCommand(int id, int code, HWND ctl) {
    if (id == IDOK || id == IDCANCEL) {
        End(id);
        return TRUE;
    }
    return FALSE;
}

std::wstring DialogBase::GetText(int id) const { return GetWindowTextStr(Item(id)); }
void DialogBase::SetText(int id, const std::wstring& text) { SetDlgItemTextW(hwnd_, id, text.c_str()); }
long long DialogBase::GetInt(int id, long long def) const { return StrToInt(GetText(id), def); }
void DialogBase::SetInt(int id, long long v) { SetText(id, std::to_wstring(v)); }

void DialogBase::ComboAdd(int id, const std::wstring& text, LPARAM data) {
    int i = (int)SendDlgItemMessageW(hwnd_, id, CB_ADDSTRING, 0, (LPARAM)text.c_str());
    SendDlgItemMessageW(hwnd_, id, CB_SETITEMDATA, i, data);
}
int DialogBase::ComboSel(int id) const { return (int)SendDlgItemMessageW(hwnd_, id, CB_GETCURSEL, 0, 0); }
void DialogBase::ComboSetSel(int id, int index) { SendDlgItemMessageW(hwnd_, id, CB_SETCURSEL, index, 0); }
LPARAM DialogBase::ComboData(int id, int index) const {
    return SendDlgItemMessageW(hwnd_, id, CB_GETITEMDATA, index, 0);
}

void DialogBase::LvAddColumn(int id, const std::wstring& title, int widthDlu, int fmt) {
    HWND lv = Item(id);
    RECT r{0, 0, widthDlu, 0};
    MapDialogRect(hwnd_, &r);
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = fmt;
    c.cx = r.right;
    c.pszText = const_cast<wchar_t*>(title.c_str());
    int n = Header_GetItemCount(ListView_GetHeader(lv));
    ListView_InsertColumn(lv, n, &c);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
}

int DialogBase::LvAddRow(int id, const std::vector<std::wstring>& cells, LPARAM data) {
    HWND lv = Item(id);
    LVITEMW it{};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = ListView_GetItemCount(lv);
    it.pszText = cells.empty() ? const_cast<wchar_t*>(L"") : const_cast<wchar_t*>(cells[0].c_str());
    it.lParam = data;
    int row = ListView_InsertItem(lv, &it);
    for (size_t i = 1; i < cells.size(); ++i) ListView_SetItemText(lv, row, (int)i, const_cast<wchar_t*>(cells[i].c_str()));
    return row;
}

void DialogBase::SetAnchor(int id, unsigned anchor) {
    HWND h = Item(id);
    if (!h) return;
    RECT rc;
    GetWindowRect(h, &rc);
    MapWindowPoints(nullptr, hwnd_, (POINT*)&rc, 2);
    for (auto& a : anchors_)
        if (a.id == id) {
            a.anchor = anchor;
            a.rc = rc;
            return;
        }
    anchors_.push_back({id, anchor, rc});
}

void DialogBase::EnableResizing() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    initialClient_ = {rc.right, rc.bottom};
    GetWindowRect(hwnd_, &rc);
    minTrack_ = {rc.right - rc.left, rc.bottom - rc.top};
    resizing_ = true;
}

INT_PTR CALLBACK DialogBase::StaticProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    DialogBase* self = nullptr;
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<DialogBase*>(lp);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)self);
        // Programmsymbol für alle Dialoge
        SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)App::SmallIcon());
        SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)App::BigIcon());
    } else {
        self = reinterpret_cast<DialogBase*>(GetWindowLongPtrW(h, DWLP_USER));
    }
    if (!self) return FALSE;
    INT_PTR r = self->Proc(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(h, DWLP_USER, 0);
        if (self->modeless_) App::UnregisterModeless(h);
        self->hwnd_ = nullptr;
        if (self->deleteOnClose_) delete self;
    }
    return r;
}

INT_PTR DialogBase::Proc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG:
        return OnInit();
    case WM_COMMAND:
        if (OnCommand(LOWORD(wp), HIWORD(wp), (HWND)lp)) return TRUE;
        // Nicht-modale Dialoge: Schließen über Esc/X
        if (modeless_ && LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd_);
            return TRUE;
        }
        break;
    case WM_NOTIFY: {
        INT_PTR r = OnNotify(reinterpret_cast<NMHDR*>(lp));
        if (r) {
            SetResult(r);
            return TRUE;
        }
        break;
    }
    case WM_GETMINMAXINFO:
        if (resizing_) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = minTrack_.cx;
            mmi->ptMinTrackSize.y = minTrack_.cy;
            return TRUE;
        }
        break;
    case WM_SIZE: {
        int cx = LOWORD(lp), cy = HIWORD(lp);
        if (resizing_ && wp != SIZE_MINIMIZED && !anchors_.empty()) {
            int dx = cx - initialClient_.cx, dy = cy - initialClient_.cy;
            HDWP dwp = BeginDeferWindowPos((int)anchors_.size());
            for (auto& a : anchors_) {
                RECT r = a.rc;
                if (a.anchor & AnchorRight) {
                    if (a.anchor & AnchorLeft)
                        r.right += dx;
                    else {
                        r.left += dx;
                        r.right += dx;
                    }
                }
                if (a.anchor & AnchorBottom) {
                    if (a.anchor & AnchorTop)
                        r.bottom += dy;
                    else {
                        r.top += dy;
                        r.bottom += dy;
                    }
                }
                HWND c = Item(a.id);
                if (c && dwp)
                    dwp = DeferWindowPos(dwp, c, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                         SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (dwp) EndDeferWindowPos(dwp);
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        OnSize(cx, cy);
        break;
    }
    case WM_DESTROY:
        OnDestroy();
        break;
    case WM_CLOSE:
        if (modeless_) {
            DestroyWindow(hwnd_);
            return TRUE;
        }
        EndDialog(hwnd_, IDCANCEL);
        return TRUE;
    }
    return OnMessage(msg, wp, lp);
}

} // namespace qf
