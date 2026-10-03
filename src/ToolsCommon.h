#pragma once
// Gemeinsame Hilfen für Modul E (Werkzeuge, Optionen, Funktionstasten, Archive, Druckliste).
// Nur von Tools.cpp, Attributes.cpp, FunctionKeys.cpp, Archive.cpp, OptionsDialog.cpp und PrintList.cpp benutzt.
//
// - RunWithProgress: führt eine längere Arbeit in einem eigenen Thread aus und zeigt dabei einen modalen
//   Fortschrittsdialog mit Abbrechen-Schaltfläche (Rückmeldung per PostMessageW/Timer).
// - Umrechnung lokale Zeit <-> FILETIME (UTC) unter Berücksichtigung der Sommerzeit des jeweiligen Datums.

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "App.h"
#include "Dialog.h"
#include "Util.h"

namespace qf {
namespace toolsdetail {

// ---------- Fortschritt ----------

// Schnittstelle für den Arbeitsthread (thread-sicher).
class ProgressSink {
public:
    bool Cancelled() const { return cancel_.load(); }
    void SetText(const std::wstring& text) {
        std::lock_guard<std::mutex> lock(m_);
        text_ = text;
        dirty_ = true;
    }
    // 0–100; negativ = unbestimmt (Laufbalken)
    void SetPercent(int percent) {
        std::lock_guard<std::mutex> lock(m_);
        percent_ = percent;
        dirty_ = true;
    }

private:
    friend class ProgressDialog;
    std::atomic<bool> cancel_{false};
    std::mutex m_;
    std::wstring text_;
    int percent_ = -1;
    bool dirty_ = false;
};

class ProgressDialog : public DialogBase {
public:
    static constexpr int kText = 101;
    static constexpr int kBar = 102;
    static constexpr UINT kMsgDone = WM_APP + 77;
    static constexpr UINT_PTR kTimer = 1;

    std::function<void(ProgressSink&)> work;
    ProgressSink sink;
    std::wstring initialText;

protected:
    BOOL OnInit() override {
        SetText(kText, initialText);
        SetMarquee(true);
        SetTimer(hwnd_, kTimer, 100, nullptr);
        HWND h = hwnd_;
        thread_ = std::thread([this, h]() {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try {
                if (work) work(sink);
            } catch (...) {
            }
            if (SUCCEEDED(hr)) CoUninitialize();
            PostMessageW(h, kMsgDone, 0, 0);
        });
        return TRUE;
    }
    BOOL OnCommand(int id, int code, HWND ctl) override {
        if (id == IDOK) return TRUE;   // Eingabetaste ignorieren
        if (id == IDCANCEL) {
            // Nur Abbruch anfordern – der Dialog endet, wenn der Thread fertig ist.
            sink.cancel_ = true;
            Enable(IDCANCEL, false);
            SetText(kText, L"Wird abgebrochen …");
            return TRUE;
        }
        return FALSE;
    }
    INT_PTR OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == kMsgDone) {
            KillTimer(hwnd_, kTimer);
            if (thread_.joinable()) thread_.join();
            End(sink.Cancelled() ? IDCANCEL : IDOK);
            return TRUE;
        }
        if (msg == WM_TIMER && wp == kTimer) {
            std::wstring text;
            int percent = -1;
            bool dirty = false;
            {
                std::lock_guard<std::mutex> lock(sink.m_);
                if (sink.dirty_) {
                    text = sink.text_;
                    percent = sink.percent_;
                    dirty = true;
                    sink.dirty_ = false;
                }
            }
            if (dirty && !sink.Cancelled()) {
                SetText(kText, text);
                if (percent >= 0) {
                    SetMarquee(false);
                    SendMessageW(Item(kBar), PBM_SETPOS, (WPARAM)percent, 0);
                } else {
                    SetMarquee(true);
                }
            }
            return TRUE;
        }
        if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE) {
            OnCommand(IDCANCEL, BN_CLICKED, nullptr);
            return TRUE;
        }
        return FALSE;
    }
    void OnDestroy() override {
        // Sicherheitsnetz: der Thread muss beendet sein, bevor das Objekt verschwindet.
        sink.cancel_ = true;
        if (thread_.joinable()) thread_.join();
    }

private:
    void SetMarquee(bool on) {
        if (on == marquee_) return;
        HWND bar = Item(kBar);
        LONG_PTR st = GetWindowLongPtrW(bar, GWL_STYLE);
        if (on) {
            SetWindowLongPtrW(bar, GWL_STYLE, st | PBS_MARQUEE);
            SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
        } else {
            SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtrW(bar, GWL_STYLE, st & ~(LONG_PTR)PBS_MARQUEE);
            SendMessageW(bar, PBM_SETRANGE32, 0, 100);
        }
        marquee_ = on;
    }
    std::thread thread_;
    bool marquee_ = false;
};

// Führt work in einem Thread aus (COM ist dort als STA initialisiert). Rückgabe false bei Abbruch.
inline bool RunWithProgress(HWND owner, const std::wstring& title, const std::wstring& text,
                            std::function<void(ProgressSink&)> work) {
    DialogTemplate t(title, 260, 62, DS_MODALFRAME | WS_POPUP | WS_CAPTION | DS_CENTER | DS_SETFONT);
    t.Label(ProgressDialog::kText, L"", 7, 7, 246, 10, SS_ENDELLIPSIS);
    t.Progress(ProgressDialog::kBar, 7, 21, 246, 10);
    t.Button(IDCANCEL, L"Abbrechen", 203, 40, 50, 14);
    ProgressDialog dlg;
    dlg.work = std::move(work);
    dlg.initialText = text;
    return dlg.DoModal(owner, t) == IDOK;
}

// ---------- Zeit ----------

// Lokale Zeit (SYSTEMTIME) -> FILETIME (UTC)
inline bool LocalToFileTime(const SYSTEMTIME& local, FILETIME& ft) {
    SYSTEMTIME utc;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return false;
    return SystemTimeToFileTime(&utc, &ft) != FALSE;
}

// FILETIME (UTC) -> lokale Zeit (SYSTEMTIME)
inline bool FileTimeToLocal(const FILETIME& ft, SYSTEMTIME& local) {
    SYSTEMTIME utc;
    if (!FileTimeToSystemTime(&ft, &utc)) return false;
    return SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) != FALSE;
}

// Datum + Uhrzeit (lokal) als Text gemäß Gebietsschema
inline std::wstring FormatLocalSystemTime(const SYSTEMTIME& st, bool withSeconds = false) {
    wchar_t date[64] = L"", time[64] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, withSeconds ? 0 : TIME_NOSECONDS, &st, nullptr, time, 64);
    return std::wstring(date) + L" " + time;
}

// Setzt Pfad in Anführungszeichen, wenn er Leerzeichen enthält und noch nicht gequotet ist.
inline std::wstring QuoteIfNeeded(const std::wstring& s) {
    if (s.empty()) return L"\"\"";
    if (s.front() == L'"') return s;
    if (s.find_first_of(L" \t&()^,;=") == std::wstring::npos) return s;
    return L"\"" + s + L"\"";
}

} // namespace toolsdetail
} // namespace qf
