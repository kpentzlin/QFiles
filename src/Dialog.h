#pragma once
// Dialoge ohne .rc-Datei: DialogTemplate baut eine DLGTEMPLATEEX im Speicher,
// DialogBase kapselt die Dialogprozedur (modal und nicht modal) und ein einfaches
// Verankerungs-Layout für in der Größe veränderbare Dialoge.
//
// Koordinaten in Dialogeinheiten (DLU), Schrift "Segoe UI" 9 pt.

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <cstdint>

namespace qf {

class DialogTemplate {
public:
    // style: Standard ist ein modaler Dialog mit Titelleiste. Für veränderbare Dialoge WS_THICKFRAME ergänzen
    // (Konstante kResizable).
    static constexpr DWORD kDefaultStyle = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_SETFONT;
    static constexpr DWORD kResizable = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | DS_CENTER | DS_SETFONT |
                                        WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

    DialogTemplate(const std::wstring& title, int cx, int cy, DWORD style = kDefaultStyle, DWORD exStyle = 0);

    // Allgemein: Klasse als Name (z. B. WC_LISTVIEWW) oder vordefinierte Atome über die Helfer unten.
    void Add(int id, const wchar_t* className, const std::wstring& text, int x, int y, int cx, int cy, DWORD style,
             DWORD exStyle = 0);

    void Label(int id, const std::wstring& text, int x, int y, int cx, int cy, DWORD extraStyle = 0);
    void Edit(int id, int x, int y, int cx, int cy, DWORD extraStyle = ES_AUTOHSCROLL, DWORD exStyle = WS_EX_CLIENTEDGE);
    void MultiEdit(int id, int x, int y, int cx, int cy, DWORD extraStyle = 0);
    void Button(int id, const std::wstring& text, int x, int y, int cx, int cy, DWORD extraStyle = 0);
    void DefButton(int id, const std::wstring& text, int x, int y, int cx, int cy);
    void Check(int id, const std::wstring& text, int x, int y, int cx, int cy, DWORD extraStyle = 0);
    void Radio(int id, const std::wstring& text, int x, int y, int cx, int cy, bool firstInGroup = false);
    void Group(int id, const std::wstring& text, int x, int y, int cx, int cy);
    void Combo(int id, int x, int y, int cx, int cy, bool editable = false, DWORD extraStyle = 0); // cy = Höhe inkl. Liste
    void List(int id, int x, int y, int cx, int cy, DWORD extraStyle = 0);
    void ListView(int id, int x, int y, int cx, int cy, DWORD extraStyle = LVS_REPORT | LVS_SHOWSELALWAYS);
    void TreeView(int id, int x, int y, int cx, int cy, DWORD extraStyle = 0);
    void Progress(int id, int x, int y, int cx, int cy);
    void DateTime(int id, int x, int y, int cx, int cy, DWORD extraStyle = 0); // DTS_*
    void UpDown(int id, int x, int y, int cx, int cy, DWORD extraStyle = UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS);
    void Tab(int id, int x, int y, int cx, int cy);
    void Custom(int id, const wchar_t* className, int x, int y, int cx, int cy, DWORD style, DWORD exStyle = 0);

    const DLGTEMPLATE* Data() const { return reinterpret_cast<const DLGTEMPLATE*>(buf_.data()); }

private:
    void Align();
    void Word(WORD w);
    void DWord(DWORD d);
    void Str(const std::wstring& s);
    void AddControl(int id, const wchar_t* className, WORD atom, const std::wstring& text, int x, int y, int cx,
                    int cy, DWORD style, DWORD exStyle);

    std::vector<uint8_t> buf_;
    size_t countOffset_ = 0;
    WORD count_ = 0;
};

// Anker für die Größenanpassung
enum Anchor : unsigned {
    AnchorNone = 0,
    AnchorLeft = 1,
    AnchorTop = 2,
    AnchorRight = 4,
    AnchorBottom = 8,
    AnchorAll = 15,
    AnchorTopLeft = AnchorLeft | AnchorTop,
    AnchorTopRight = AnchorRight | AnchorTop,
    AnchorBottomLeft = AnchorLeft | AnchorBottom,
    AnchorBottomRight = AnchorRight | AnchorBottom,
    AnchorTopLeftRight = AnchorLeft | AnchorTop | AnchorRight,
    AnchorBottomLeftRight = AnchorLeft | AnchorBottom | AnchorRight,
};

class DialogBase {
public:
    virtual ~DialogBase() = default;
    // Modal; liefert den an End() übergebenen Wert.
    INT_PTR DoModal(HWND owner, const DialogTemplate& t);
    // Nicht modal; das Objekt muss bis WM_NCDESTROY leben. Mit deleteOnClose=true löscht es sich selbst.
    HWND CreateModeless(HWND owner, const DialogTemplate& t, bool deleteOnClose = true);
    HWND Hwnd() const { return hwnd_; }

protected:
    virtual BOOL OnInit() { return TRUE; }
    // Standard: IDOK/IDCANCEL beenden den Dialog.
    virtual BOOL OnCommand(int id, int code, HWND ctl);
    virtual INT_PTR OnNotify(NMHDR* nm) { return 0; }
    virtual void OnSize(int cx, int cy) {}
    virtual void OnDestroy() {}
    // Für alle übrigen Nachrichten; Rückgabe TRUE = behandelt (Ergebnis über SetResult).
    virtual INT_PTR OnMessage(UINT msg, WPARAM wp, LPARAM lp) { return FALSE; }

    void End(INT_PTR result);
    HWND Item(int id) const { return GetDlgItem(hwnd_, id); }
    std::wstring GetText(int id) const;
    void SetText(int id, const std::wstring& text);
    long long GetInt(int id, long long def = 0) const;
    void SetInt(int id, long long v);
    bool IsChecked(int id) const { return IsDlgButtonChecked(hwnd_, id) == BST_CHECKED; }
    void SetCheck(int id, bool on) { CheckDlgButton(hwnd_, id, on ? BST_CHECKED : BST_UNCHECKED); }
    void Enable(int id, bool on) { EnableWindow(Item(id), on); }
    void Show(int id, bool on) { ShowWindow(Item(id), on ? SW_SHOW : SW_HIDE); }
    void SetResult(LRESULT r) { SetWindowLongPtrW(hwnd_, DWLP_MSGRESULT, r); }
    // Combo-/Listen-Helfer
    void ComboAdd(int id, const std::wstring& text, LPARAM data = 0);
    int ComboSel(int id) const;
    void ComboSetSel(int id, int index);
    LPARAM ComboData(int id, int index) const;
    // ListView-Helfer
    void LvAddColumn(int id, const std::wstring& title, int widthDlu, int fmt = LVCFMT_LEFT);
    int LvAddRow(int id, const std::vector<std::wstring>& cells, LPARAM data = 0);
    // Verankerung (nach dem Erzeugen, z. B. in OnInit) – merkt sich die aktuellen Abstände.
    void SetAnchor(int id, unsigned anchor);
    // Mindestgröße = Anfangsgröße
    void EnableResizing();

    HWND hwnd_ = nullptr;
    bool modeless_ = false;
    bool deleteOnClose_ = false;

private:
    static INT_PTR CALLBACK StaticProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    INT_PTR Proc(UINT msg, WPARAM wp, LPARAM lp);
    struct AnchorInfo {
        int id;
        unsigned anchor;
        RECT rc;      // Ausgangsrechteck (Client-Koordinaten)
    };
    std::vector<AnchorInfo> anchors_;
    SIZE initialClient_{};
    SIZE minTrack_{};
    bool resizing_ = false;
};

} // namespace qf
