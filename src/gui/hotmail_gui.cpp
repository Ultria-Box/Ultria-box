// Mailbox preset UI — "Mailbox Checker": scores mail accounts from the bundled
// fictional corpus at a throttled rate, then previews the inbox of a verified
// account in a webmail-style reader. Fully offline, GDI+ custom drawn.
//
// Drop-in module: any build that selects this preset links this file together
// with ui_kit.h / mail_data.h. Signature matches gui/gui.h.
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include "ui_kit.h"
#include "gui.h"
#include "mail_data.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace gui {

// ST_ = account verdict states
enum { ST_WAIT = 0, ST_CHECK = 1, ST_VALID = 2, ST_BAD = 3, ST_2FA = 4 };

struct AccountRow {
    std::wstring addr, pass;
    int first = 0, count = 0;   // slice into mailbox::kMessages
    int state = ST_WAIT;
    ULONGLONG changed = 0;
};

struct App {
    HWND win = nullptr;
    Kit kit;

    std::vector<AccountRow> rows;
    int sel = -1;              // selected account
    int hover_row = -1;
    double acc_scroll = 0;

    // simulation
    bool running = false;
    double run_acc = 0.0;
    ULONGLONG last_tick = 0;
    std::mt19937 rng{20261125};
    int n_valid = 0, n_bad = 0, n_2fa = 0;

    // webmail view
    bool mail_open = false;    // occupies the right pane
    int mail_account = -1;
    int mail_sel = 0;          // selected message inside the open box
    double mail_scroll = 0;

    std::wstring toast;
    DWORD toast_until = 0;

    RECT rc_check{}, rc_stop{}, rc_back{};
    bool hover_check = false, hover_stop = false, hover_back = false;
} g;

int dp(int v) { return g.kit.dp(v); }
void refresh() { if (g.win) InvalidateRect(g.win, nullptr, FALSE); }
void notify(std::wstring s) { g.toast = std::move(s); g.toast_until = GetTickCount() + 4000; refresh(); }

// ---------- drawing helpers ----------

Gdiplus::RectF rf(int x, int y, int w, int h) {
    return Gdiplus::RectF((Gdiplus::REAL)dp(x), (Gdiplus::REAL)dp(y),
                          (Gdiplus::REAL)dp(w), (Gdiplus::REAL)dp(h));
}
void text_at(Gdiplus::Graphics& gr, Gdiplus::Font* f, const std::wstring& s, int x, int y, int w, int h,
             uint32_t c, int align = 0) {
    draw_text(gr, f, s, rf(x, y, w, h), c, align);
}
void section_card(Gdiplus::Graphics& gr, int x, int y, int w, int h) {
    round_rect(gr, rf(x, y, w, h), (Gdiplus::REAL)dp(10), col::panel);
    round_border(gr, rf(x, y, w, h), (Gdiplus::REAL)dp(10), col::border);
}
const wchar_t* state_label(int st) {
    switch (st) {
        case ST_CHECK: return L"CHECKING";
        case ST_VALID: return L"VALID";
        case ST_BAD: return L"BAD";
        case ST_2FA: return L"2FA";
        default: return L"WAIT";
    }
}
uint32_t state_color(int st) {
    switch (st) {
        case ST_CHECK: return col::warn;
        case ST_VALID: return col::hit;
        case ST_BAD: return col::bad;
        case ST_2FA: return col::accent1;
        default: return col::wait_;
    }
}
uint32_t avatar_color(const std::wstring& s) {
    uint32_t h = 2166136261u;
    for (wchar_t c : s) { h ^= (uint32_t)c; h *= 16777619u; }
    static const uint32_t palette[] = {0x3B82F6, 0x8B5CF6, 0xEC4899, 0xF59E0B, 0x10B981,
                                       0x06B6D4, 0xEF4444, 0x6366F1, 0x14B8A6, 0xD946EF};
    return palette[h % 10];
}

// soft word wrap for the reader pane
std::vector<std::wstring> wrap_lines(Gdiplus::Graphics& gr, Gdiplus::Font* font,
                                     const std::wstring& text, float max_w) {
    std::vector<std::wstring> out;
    Gdiplus::StringFormat fmt;
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        std::wstring para = text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        // wrap the paragraph
        std::wstring line;
        size_t i = 0;
        while (i < para.size()) {
            size_t sp = para.find(L' ', i);
            std::wstring word = para.substr(i, sp == std::wstring::npos ? std::wstring::npos : sp - i);
            std::wstring trial = line.empty() ? word : line + L" " + word;
            Gdiplus::RectF box(0, 0, max_w, 1000);
            Gdiplus::RectF outr;
            gr.MeasureString(trial.c_str(), (INT)trial.size(), font, box, &fmt, &outr);
            if (outr.Width > max_w && !line.empty()) {
                out.push_back(line);
                line = word;
            } else {
                line = trial;
            }
            if (sp == std::wstring::npos) break;
            i = sp + 1;
        }
        out.push_back(line);
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
    return out;
}

// ---------- corpus loading ----------
void load_corpus() {
    for (int i = 0; i < mailbox::kAccountCount; ++i) {
        AccountRow r;
        r.addr = wstr(mailbox::kAccounts[i].addr);
        r.pass = wstr(mailbox::kAccounts[i].pass);
        r.first = mailbox::kAccounts[i].first;
        r.count = mailbox::kAccounts[i].count;
        g.rows.push_back(std::move(r));
    }
}

// ---------- simulation ----------
void check_all() {
    if (g.running) return;
    g.running = true;
    g.run_acc = 0;
    g.last_tick = GetTickCount64();
    for (auto& r : g.rows)
        if (r.state != ST_VALID) { r.state = ST_WAIT; r.changed = 0; }
    g.n_valid = g.n_bad = g.n_2fa = 0;
    SetTimer(g.win, 1, 30, nullptr);
    notify(L"Checking accounts…");
}

void stop_run() {
    g.running = false;
    KillTimer(g.win, 1);
    for (auto& r : g.rows)
        if (r.state == ST_CHECK) r.state = ST_WAIT;
    refresh();
}

void tick() {
    if (!g.running) { KillTimer(g.win, 1); return; }
    // hard cap: 10 accounts checked per second (token bucket on wall clock)
    const ULONGLONG now = GetTickCount64();
    ULONGLONG dt = now - g.last_tick;
    if (dt > 1000) dt = 1000;
    g.last_tick = now;
    g.run_acc += 10.0 * (double)dt / 1000.0;
    int budget = (int)g.run_acc;
    g.run_acc -= budget;

    std::uniform_real_distribution<double> roll(0, 1);
    for (auto& r : g.rows) {
        if (budget <= 0) break;
        if (r.state != ST_WAIT && r.state != ST_CHECK) continue;
        --budget;
        double v = roll(g.rng);
        r.state = v < 0.58 ? ST_VALID : (v < 0.92 ? ST_BAD : ST_2FA);
        r.changed = now;
        if (r.state == ST_VALID) ++g.n_valid;
        else if (r.state == ST_BAD) ++g.n_bad;
        else ++g.n_2fa;
    }
    bool any = false;
    for (auto& r : g.rows)
        if (r.state == ST_WAIT || r.state == ST_CHECK) { any = true; break; }
    if (!any) {
        g.running = false;
        KillTimer(g.win, 1);
        notify(L"Check complete — " + std::to_wstring(g.n_valid) + L" valid · " +
               std::to_wstring(g.n_bad) + L" bad · " + std::to_wstring(g.n_2fa) + L" 2FA");
    }
    refresh();
}

// ---------- layout ----------
struct Layout {
    int w = 0, h = 0;
    int header_h = 60, status_h = 38;
    int left_x = 16, left_w = 402;
    int right_x = 0, right_w = 0;
} L;

void relayout(HWND win) {
    RECT rc{};
    GetClientRect(win, &rc);
    L.w = MulDiv(rc.right, 96, g.kit.dpi);
    L.h = MulDiv(rc.bottom, 96, g.kit.dpi);
    L.right_x = L.left_x + L.left_w + 14;
    L.right_w = L.w - L.right_x - 16;
}

const int ROW_H = 52;

void open_mailbox(int idx) {
    if (idx < 0 || idx >= (int)g.rows.size()) return;
    if (g.rows[idx].state != ST_VALID) return;
    g.mail_open = true;
    g.mail_account = idx;
    g.mail_sel = 0;
    g.mail_scroll = 0;
    refresh();
}

// ---------- painting ----------
void paint_accounts(Gdiplus::Graphics& gr) {
    int x = L.left_x, y = L.header_h + 8;
    int w = L.left_w, h = L.h - y - L.status_h - 14;
    section_card(gr, x, y, w, h);

    text_at(gr, g.kit.f_h1, L"Comptes", x + 18, y + 12, 200, 26, col::text);
    std::wstring sub = std::to_wstring(g.rows.size()) + L" comptes  ·  " +
                       std::to_wstring(g.n_valid) + L" valides";
    text_at(gr, g.kit.f_small, sub, x + 18, y + 36, w - 130, 18, col::sub);
    // status dots summary
    int dx = x + w - 112;
    struct { uint32_t c; int n; } legs[] = {{col::hit, g.n_valid}, {col::bad, g.n_bad},
                                            {col::accent1, g.n_2fa}};
    for (int i = 0; i < 3; ++i) {
        Gdiplus::SolidBrush b(gd(legs[i].c));
        gr.FillEllipse(&b, (Gdiplus::REAL)dp(dx), (Gdiplus::REAL)dp(y + 20),
                       (Gdiplus::REAL)dp(8), (Gdiplus::REAL)dp(8));
        text_at(gr, g.kit.f_small, std::to_wstring(legs[i].n), dx + 12, y + 12, 26, 24, col::sub);
        dx += 36;
    }

    int list_y = y + 62, list_h = h - 74;
    int content_h = (int)g.rows.size() * dp(ROW_H);
    int max_scroll = (std::max)(0, content_h - list_h);
    g.acc_scroll = (std::clamp)(g.acc_scroll, 0.0, (double)max_scroll);

    // clip
    Gdiplus::RectF clip((Gdiplus::REAL)dp(x + 1), (Gdiplus::REAL)dp(list_y),
                        (Gdiplus::REAL)dp(w - 2), (Gdiplus::REAL)dp(list_h));
    gr.SetClip(clip);

    for (int i = 0; i < (int)g.rows.size(); ++i) {
        int ry = list_y + dp(4) + (int)(i * dp(ROW_H) - g.acc_scroll);
        if (ry + dp(ROW_H) < list_y || ry > list_y + list_h) continue;
        auto& r = g.rows[i];
        bool hovered = i == g.hover_row;
        bool selected = i == g.sel;
        Gdiplus::RectF row_r((Gdiplus::REAL)dp(x + 6), (Gdiplus::REAL)ry,
                             (Gdiplus::REAL)dp(w - 12), (Gdiplus::REAL)dp(ROW_H - 6));
        if (selected) round_rect(gr, row_r, (Gdiplus::REAL)dp(8), 0x1B2A44);
        else if (hovered) round_rect(gr, row_r, (Gdiplus::REAL)dp(8), 0x141C2A);

        // state dot
        Gdiplus::SolidBrush dot(gd(state_color(r.state)));
        gr.FillEllipse(&dot, (Gdiplus::REAL)dp(x + 20), (Gdiplus::REAL)dp(ry + 16),
                       (Gdiplus::REAL)dp(9), (Gdiplus::REAL)dp(9));

        text_at(gr, g.kit.f_bold, r.addr, x + 40, ry + 6, w - 150, 20, col::text);
        text_at(gr, g.kit.f_small, r.pass, x + 40, ry + 25, w - 150, 18, 0x7F8BA3);

        // state pill
        std::wstring lbl = state_label(r.state);
        Gdiplus::RectF pill((Gdiplus::REAL)dp(x + w - 96), (Gdiplus::REAL)dp(ry + 14),
                            (Gdiplus::REAL)dp(74), (Gdiplus::REAL)dp(22));
        round_rect(gr, pill, (Gdiplus::REAL)dp(6), state_color(r.state), 36);
        round_border(gr, pill, (Gdiplus::REAL)dp(6), state_color(r.state), 1.0f);
        draw_text(gr, g.kit.f_small, lbl, pill, state_color(r.state), 1);
    }
    gr.ResetClip();

    // scrollbar hint
    if (max_scroll > 0) {
        int bar_h = (std::max)(30, list_h * list_h / content_h);
        int bar_y = list_y + (int)((list_h - bar_h) * (g.acc_scroll / max_scroll));
        round_rect(gr, rf(x + w - 8, bar_y, 4, bar_h), 2.0f, col::border);
    }
}

void paint_reader(Gdiplus::Graphics& gr, int x, int y, int w, int h) {
    // webmail-style light surface
    round_rect(gr, rf(x, y, w, h), (Gdiplus::REAL)dp(10), 0xFFFFFF);
    round_border(gr, rf(x, y, w, h), (Gdiplus::REAL)dp(10), 0xD8DEE8);

    // top bar (webmail blue)
    Gdiplus::GraphicsPath top;
    round_path(top, rf(x, y, w, 54), (Gdiplus::REAL)dp(10));
    Gdiplus::SolidBrush blue(gd(0x0F6CBD));
    gr.FillPath(&blue, &top);
    Gdiplus::SolidBrush blue2(gd(0x0F6CBD));
    gr.FillRectangle(&blue2, (Gdiplus::REAL)dp(x), (Gdiplus::REAL)dp(y + 44),
                     (Gdiplus::REAL)dp(w), (Gdiplus::REAL)dp(10));

    // logo square + wordmark
    Gdiplus::SolidBrush wbox(gd(0xFFFFFF));
    gr.FillRectangle(&wbox, (Gdiplus::REAL)dp(x + 16), (Gdiplus::REAL)dp(y + 13),
                     (Gdiplus::REAL)dp(28), (Gdiplus::REAL)dp(28));
    Gdiplus::SolidBrush wblue(gd(0x0F6CBD));
    gr.FillRectangle(&wblue, (Gdiplus::REAL)dp(x + 22), (Gdiplus::REAL)dp(y + 19),
                     (Gdiplus::REAL)dp(16), (Gdiplus::REAL)dp(16));
    text_at(gr, g.kit.f_h1, L"Outlook", x + 54, y + 12, 200, 30, 0xFFFFFF);

    // search pill
    Gdiplus::RectF sp((Gdiplus::REAL)dp(x + 190), (Gdiplus::REAL)dp(y + 12),
                      (Gdiplus::REAL)dp(w - 460), (Gdiplus::REAL)dp(30));
    round_rect(gr, sp, (Gdiplus::REAL)dp(15), 0x2B7CC9);
    text_at(gr, g.kit.f_small, L"Rechercher", x + 212, y + 12, w - 480, 30, 0xCFE3F7);

    // back chip
    Gdiplus::RectF back((Gdiplus::REAL)dp(x + w - 232), (Gdiplus::REAL)dp(y + 12),
                        (Gdiplus::REAL)dp(96), (Gdiplus::REAL)dp(30));
    round_rect(gr, back, (Gdiplus::REAL)dp(15), 0x2B7CC9);
    text_at(gr, g.kit.f_small, L"\u2039  Comptes", x + w - 232, y + 12, 96, 30, 0xFFFFFF, 1);
    g.rc_back = {dp(x + w - 232), dp(y + 12), dp(x + w - 232 + 96), dp(y + 12 + 30)};

    // avatar chip
    if (g.mail_account >= 0) {
        const auto& acc = g.rows[g.mail_account];
        wchar_t ini = acc.addr.empty() ? L'?' : towupper(acc.addr[0]);
        Gdiplus::SolidBrush av(gd(0xF3B23B));
        gr.FillEllipse(&av, (Gdiplus::REAL)dp(x + w - 122), (Gdiplus::REAL)dp(y + 12),
                       (Gdiplus::REAL)dp(30), (Gdiplus::REAL)dp(30));
        text_at(gr, g.kit.f_bold, std::wstring(1, ini), x + w - 122, y + 12, 30, 30, 0x4A3000, 1);
        text_at(gr, g.kit.f_small, acc.addr, x + w - 320, y + 12, 190, 30, 0xE8F1FA, 2);
    }

    // folder line
    text_at(gr, g.kit.f_h1, L"Boîte de réception", x + 24, y + 66, 300, 24, 0x1B1F27);

    // message list (left sub-column)
    int list_x = x + 16, list_y = y + 100, list_w = 330, list_h = h - 116;
    if (g.mail_account < 0) return;
    const auto& acc = g.rows[g.mail_account];
    const int n = acc.count;
    g.mail_sel = (std::clamp)(g.mail_sel, 0, (std::max)(0, n - 1));

    for (int i = 0; i < n; ++i) {
        const auto& m = mailbox::kMessages[acc.first + i];
        int my = list_y + i * dp(74) - (int)g.mail_scroll;
        if (my + dp(74) < list_y || my > list_y + list_h) continue;
        Gdiplus::RectF mr((Gdiplus::REAL)dp(list_x), (Gdiplus::REAL)my,
                          (Gdiplus::REAL)dp(list_w), (Gdiplus::REAL)dp(68));
        bool sel = i == g.mail_sel;
        if (sel) round_rect(gr, mr, (Gdiplus::REAL)dp(8), 0xE8F0FA);
        Gdiplus::Pen sep(gd(0xE4E9F1));
        gr.DrawLine(&sep, (Gdiplus::REAL)dp(list_x + 8), (Gdiplus::REAL)dp(my + 68),
                    (Gdiplus::REAL)dp(list_x + list_w - 8), (Gdiplus::REAL)dp(my + 68));

        // sender avatar dot
        Gdiplus::SolidBrush av(gd(avatar_color(wstr(m.from_name))));
        gr.FillEllipse(&av, (Gdiplus::REAL)dp(list_x + 6), (Gdiplus::REAL)dp(my + 14),
                       (Gdiplus::REAL)dp(26), (Gdiplus::REAL)dp(26));
        std::wstring ini = wstr(std::string(1, m.from_name[0]));
        text_at(gr, g.kit.f_bold, ini, list_x + 6, my + 14, 26, 26, 0xFFFFFF, 1);

        text_at(gr, g.kit.f_bold, wstr(m.from_name), list_x + 40, my + 6, list_w - 120, 18, 0x1B1F27);
        text_at(gr, g.kit.f_small, wstr(m.date) + L"  " + wstr(m.time), list_x + list_w - 90, my + 6,
                84, 18, 0x77808F, 2);
        text_at(gr, g.kit.f_body, wstr(m.subject), list_x + 40, my + 24, list_w - 56, 18, 0x30363F);
        std::wstring snip = wstr(m.body).substr(0, 72);
        std::replace(snip.begin(), snip.end(), L'\n', L' ');
        text_at(gr, g.kit.f_small, snip, list_x + 40, my + 42, list_w - 56, 16, 0x8A93A2);
    }

    // reading pane
    int rx = list_x + list_w + 18;
    int rw = x + w - rx - 24;
    if (rx + 100 > x + w) return;
    const auto& m = mailbox::kMessages[acc.first + g.mail_sel];

    Gdiplus::SolidBrush av(gd(avatar_color(wstr(m.from_name))));
    gr.FillEllipse(&av, (Gdiplus::REAL)dp(rx), (Gdiplus::REAL)dp(y + 104),
                   (Gdiplus::REAL)dp(40), (Gdiplus::REAL)dp(40));
    text_at(gr, g.kit.f_h1, wstr(std::string(1, m.from_name[0])), rx, y + 104, 40, 40, 0xFFFFFF, 1);

    text_at(gr, g.kit.f_bold, wstr(m.from_name), rx + 52, y + 102, rw - 60, 20, 0x1B1F27);
    text_at(gr, g.kit.f_small, wstr(m.from_addr), rx + 52, y + 121, rw - 60, 18, 0x77808F);
    text_at(gr, g.kit.f_small, L"À : " + acc.addr, rx + 52, y + 139, rw - 60, 18, 0x77808F);
    text_at(gr, g.kit.f_small, wstr(m.date) + L" 2026 · " + wstr(m.time), rx + rw - 220, y + 102,
            210, 20, 0x77808F, 2);

    Gdiplus::Pen sep(gd(0xE4E9F1));
    gr.DrawLine(&sep, (Gdiplus::REAL)dp(rx), (Gdiplus::REAL)dp(y + 170),
                (Gdiplus::REAL)dp(rx + rw), (Gdiplus::REAL)dp(y + 170));

    text_at(gr, g.kit.f_h1, wstr(m.subject), rx, y + 184, rw, 26, 0x1B1F27);

    // body with wrap
    auto lines = wrap_lines(gr, g.kit.f_body, wstr(m.body), (Gdiplus::REAL)dp(rw));
    int by = y + 222;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (by + dp(20) > y + h - 16) break;
        text_at(gr, g.kit.f_body, lines[i], rx, by, rw, 20, 0x30363F);
        by += dp(21);
    }
}

void paint_empty(Gdiplus::Graphics& gr, int x, int y, int w, int h) {
    section_card(gr, x, y, w, h);
    // envelope illustration
    int cx = x + w / 2, cy = y + h / 2 - dp(30);
    Gdiplus::RectF env((Gdiplus::REAL)dp(cx - 46), (Gdiplus::REAL)dp(cy - 30),
                       (Gdiplus::REAL)dp(92), (Gdiplus::REAL)dp(62));
    round_rect(gr, env, (Gdiplus::REAL)dp(8), 0x171F2D);
    round_border(gr, env, (Gdiplus::REAL)dp(8), 0x344158);
    Gdiplus::Pen fold(gd(0x344158), 2.0f);
    gr.DrawLine(&fold, (Gdiplus::REAL)dp(cx - 46), (Gdiplus::REAL)dp(cy - 30),
                (Gdiplus::REAL)dp(cx), (Gdiplus::REAL)dp(cy + 6));
    gr.DrawLine(&fold, (Gdiplus::REAL)dp(cx + 46), (Gdiplus::REAL)dp(cy - 30),
                (Gdiplus::REAL)dp(cx), (Gdiplus::REAL)dp(cy + 6));
    // accent dot
    Gdiplus::SolidBrush dot(gd(col::hit));
    gr.FillEllipse(&dot, (Gdiplus::REAL)dp(cx + 34), (Gdiplus::REAL)dp(cy - 40),
                   (Gdiplus::REAL)dp(14), (Gdiplus::REAL)dp(14));

    text_at(gr, g.kit.f_h1, L"Ouvrez un compte vérifié", x, cy + 52, w, 26, col::text, 1);
    text_at(gr, g.kit.f_sub,
            g.n_valid ? L"Cliquez sur un compte VALID pour lire sa boîte de réception."
                      : L"Lancez la vérification, puis ouvrez un compte validé.",
            x, cy + 80, w, 20, col::sub, 1);
}

void paint(Gdiplus::Graphics& gr) {
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    // background
    Gdiplus::SolidBrush bg(gd(col::bg));
    gr.FillRectangle(&bg, (Gdiplus::REAL)0, (Gdiplus::REAL)0, (Gdiplus::REAL)dp(L.w), (Gdiplus::REAL)dp(L.h));

    // header
    Gdiplus::SolidBrush hbg(gd(col::panel));
    gr.FillRectangle(&hbg, (Gdiplus::REAL)0, (Gdiplus::REAL)0, (Gdiplus::REAL)dp(L.w), (Gdiplus::REAL)dp(L.header_h));
    Gdiplus::Pen hline(gd(col::border));
    gr.DrawLine(&hline, (Gdiplus::REAL)0, (Gdiplus::REAL)dp(L.header_h), (Gdiplus::REAL)dp(L.w),
                (Gdiplus::REAL)dp(L.header_h));
    draw_logo(gr, (Gdiplus::REAL)dp(18), (Gdiplus::REAL)dp(12), (Gdiplus::REAL)dp(36));
    text_at(gr, g.kit.f_h1, L"MAILBOX CHECKER", 64, 10, 260, 26, col::text);
    text_at(gr, g.kit.f_small, L"Hotmail · Outlook · Live  —  compatibilité des comptes", 64, 33,
            360, 18, col::sub);

    // check all button
    {
        Gdiplus::RectF b((Gdiplus::REAL)dp(L.w - 320), (Gdiplus::REAL)dp(13), (Gdiplus::REAL)dp(150),
                         (Gdiplus::REAL)dp(34));
        g.rc_check = {dp(L.w - 320), dp(13), dp(L.w - 170), dp(47)};
        if (g.running) {
            round_rect(gr, b, (Gdiplus::REAL)dp(8), 0x1A2233);
            round_border(gr, b, (Gdiplus::REAL)dp(8), col::border);
            text_at(gr, g.kit.f_bold, L"VÉRIFICATION…", L.w - 320, 13, 150, 34, col::sub, 1);
        } else {
            grad_round(gr, b, (Gdiplus::REAL)dp(8));
            text_at(gr, g.kit.f_bold, L"TOUT VÉRIFIER", L.w - 320, 13, 150, 34, 0x081018, 1);
        }
    }
    {
        Gdiplus::RectF b((Gdiplus::REAL)dp(L.w - 158), (Gdiplus::REAL)dp(13), (Gdiplus::REAL)dp(96),
                         (Gdiplus::REAL)dp(34));
        g.rc_stop = {dp(L.w - 158), dp(13), dp(L.w - 62), dp(47)};
        round_rect(gr, b, (Gdiplus::REAL)dp(8), g.running ? 0x1A2233 : col::panel2);
        round_border(gr, b, (Gdiplus::REAL)dp(8), col::border);
        text_at(gr, g.kit.f_bold, L"STOP", L.w - 158, 13, 96, 34, g.running ? col::text : col::dim, 1);
    }
    {
        Gdiplus::RectF chip((Gdiplus::REAL)dp(320), (Gdiplus::REAL)dp(20), (Gdiplus::REAL)dp(96),
                            (Gdiplus::REAL)dp(22));
        round_rect(gr, chip, (Gdiplus::REAL)dp(11), 0x14301F);
        round_border(gr, chip, (Gdiplus::REAL)dp(11), 0x1F5A3A);
        Gdiplus::SolidBrush d(gd(col::hit));
        gr.FillEllipse(&d, (Gdiplus::REAL)dp(332), (Gdiplus::REAL)dp(28), (Gdiplus::REAL)dp(7),
                       (Gdiplus::REAL)dp(7));
        text_at(gr, g.kit.f_small, L"session locale", 344, 20, 80, 22, 0x57D3A8);
    }

    // body
    int body_y = L.header_h + 8;
    int body_h = L.h - body_y - L.status_h - 14;
    paint_accounts(gr);
    if (g.mail_open) paint_reader(gr, L.right_x, body_y, L.right_w, body_h);
    else paint_empty(gr, L.right_x, body_y, L.right_w, body_h);

    // status bar
    int sy = L.h - L.status_h;
    Gdiplus::SolidBrush sbg(gd(col::panel));
    gr.FillRectangle(&sbg, (Gdiplus::REAL)0, (Gdiplus::REAL)dp(sy), (Gdiplus::REAL)dp(L.w), (Gdiplus::REAL)dp(L.status_h));
    Gdiplus::Pen line(gd(col::border));
    gr.DrawLine(&line, (Gdiplus::REAL)0, (Gdiplus::REAL)dp(sy), (Gdiplus::REAL)dp(L.w), (Gdiplus::REAL)dp(sy));
    Gdiplus::SolidBrush dot(gd(g.running ? col::warn : col::hit));
    gr.FillEllipse(&dot, (Gdiplus::REAL)dp(22), (Gdiplus::REAL)dp(sy + 14), (Gdiplus::REAL)dp(8),
                   (Gdiplus::REAL)dp(8));
    text_at(gr, g.kit.f_small, g.running ? L"Vérification en cours · 10 comptes/s" : L"Prêt",
            38, sy, 400, L.status_h, col::sub);
    std::wstring stats = std::to_wstring(g.n_valid) + L" valides  /  " + std::to_wstring(g.n_bad) +
                         L" invalides  /  " + std::to_wstring(g.n_2fa) + L" double authentification";
    text_at(gr, g.kit.f_small, stats, L.w / 2 - 200, sy, 400, L.status_h, col::sub, 1);
    text_at(gr, g.kit.f_small, L"Traitement local", L.w - 200, sy, 184, L.status_h, col::dim, 2);

    // toast
    if (!g.toast.empty() && GetTickCount() < g.toast_until) {
        int tw = dp(520), th = dp(38);
        int tx = (L.w - MulDiv(tw, 96, g.kit.dpi)) / 2;
        Gdiplus::RectF tr((Gdiplus::REAL)tx, (Gdiplus::REAL)dp(L.h - L.status_h - 56),
                          (Gdiplus::REAL)tw, (Gdiplus::REAL)th);
        round_rect(gr, tr, (Gdiplus::REAL)dp(9), 0x151D2C, 245);
        round_border(gr, tr, (Gdiplus::REAL)dp(9), 0x2C3A52);
        draw_text(gr, g.kit.f_bold, g.toast, tr, col::text, 1);
    }
}

// ---------- input ----------
void click(int x, int y) {
    static auto in = [](const RECT& r, int px, int py) {
        return px >= r.left && px < r.right && py >= r.top && py < r.bottom;
    };
    if (in(g.rc_check, x, y)) {
        if (!g.running) check_all();
        return;
    }
    if (in(g.rc_stop, x, y)) {
        stop_run();
        return;
    }
    if (g.mail_open && in(g.rc_back, x, y)) {
        g.mail_open = false;
        g.mail_account = -1;
        refresh();
        return;
    }
    // account list
    int list_y = dp(L.header_h + 8 + 62), list_h = dp(L.h - (L.header_h + 8) - L.status_h - 14 - 74);
    if (x >= dp(L.left_x) && x <= dp(L.left_x + L.left_w) && y >= list_y && y < list_y + list_h) {
        int idx = (int)((y - list_y - dp(4) + g.acc_scroll) / dp(ROW_H));
        if (idx >= 0 && idx < (int)g.rows.size()) {
            g.sel = idx;
            if (g.rows[idx].state == ST_VALID) open_mailbox(idx);
            else refresh();
        }
        return;
    }
    // message list inside reader
    if (g.mail_open) {
        int body_y = dp(L.header_h + 8);
        int rx = dp(L.right_x), rw = dp(L.right_w);
        int list_x = rx + dp(16), list_y2 = body_y + dp(100), list_w = dp(330),
            list_h2 = dp(L.h - (L.header_h + 8) - L.status_h - 14) - dp(116);
        if (x >= list_x && x <= list_x + list_w && y >= list_y2 && y < list_y2 + list_h2) {
            int idx = (int)((y - list_y2 + (int)g.mail_scroll) / dp(74));
            const int n = g.rows[g.mail_account].count;
            if (idx >= 0 && idx < n) {
                g.mail_sel = idx;
                refresh();
            }
        }
    }
}

void hover(int x, int y) {
    static auto in = [](const RECT& r, int px, int py) {
        return px >= r.left && px < r.right && py >= r.top && py < r.bottom;
    };
    int list_y = dp(L.header_h + 8 + 62), list_h = dp(L.h - (L.header_h + 8) - L.status_h - 14 - 74);
    int hr = -1;
    if (x >= dp(L.left_x) && x <= dp(L.left_x + L.left_w) && y >= list_y && y < list_y + list_h)
        hr = (int)((y - list_y - dp(4) + g.acc_scroll) / dp(ROW_H));
    if (hr < 0 || hr >= (int)g.rows.size()) hr = -1;
    if (hr != g.hover_row) {
        g.hover_row = hr;
        refresh();
    }
}

// ---------- showcase self capture ----------
int encoder_clsid(const wchar_t* format, CLSID* out) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (!size) return -1;
    std::vector<BYTE> buf(size);
    auto* info = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i)
        if (!wcscmp(info[i].MimeType, format)) { *out = info[i].Clsid; return (int)i; }
    return -1;
}

void paint(Gdiplus::Graphics& gr);  // forward: the live painter

// Offscreen render of the exact same paint() routine — works regardless of
// window visibility or window-manager state.
void self_screenshot(HWND win, const wchar_t* path) {
    RECT rc{};
    GetClientRect(win, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppRGB);
    {
        Gdiplus::Graphics gr(&bmp);
        paint(gr);
    }
    CLSID png{};
    if (encoder_clsid(L"image/png", &png) >= 0) bmp.Save(path, &png, nullptr);
}

// ---------- window ----------
LRESULT CALLBACK proc(HWND win, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE:
            g.win = win;
            g.kit.make(GetDpiForWindow(win));
            load_corpus();
            relayout(win);
            return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) { relayout(win); refresh(); }
            return 0;
        case WM_DPICHANGED:
            g.kit.make(LOWORD(wp));
            relayout(win);
            refresh();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(win, &ps);
            {
                Gdiplus::Graphics gr(dc);
                paint(gr);
            }
            EndPaint(win, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == 1) tick();
            if (wp == 9) {
                KillTimer(win, 9);
                self_screenshot(win, L"selfshot.png");
            }
            return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, win, 0};
            TrackMouseEvent(&t);
            hover(GET_X_LPARAM(lp) * 96 / g.kit.dpi, GET_Y_LPARAM(lp) * 96 / g.kit.dpi);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g.hover_row != -1) { g.hover_row = -1; refresh(); }
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(win, &pt);
            int x = pt.x * 96 / g.kit.dpi;
            if (x < L.left_x + L.left_w) g.acc_scroll -= delta / 2.0;
            else g.mail_scroll -= delta / 2.0;
            // clamp in paint; mail scroll clamp here
            int max_ms = (std::max)(0, (g.mail_account >= 0 ? g.rows[g.mail_account].count : 0) * dp(74) -
                                           dp(L.h - (L.header_h + 8) - L.status_h - 14 - 116));
            g.mail_scroll = (std::clamp)(g.mail_scroll, 0.0, (double)max_ms);
            refresh();
            return 0;
        }
        case WM_LBUTTONDOWN:
            click(GET_X_LPARAM(lp) * 96 / g.kit.dpi, GET_Y_LPARAM(lp) * 96 / g.kit.dpi);
            return 0;
        case WM_DESTROY:
            KillTimer(win, 1);
            g.kit.clear();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(win, m, wp, lp);
}

int run(HINSTANCE instance, bool demo_mode) {
    (void)demo_mode;
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"UltriaMailboxUI";
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int w = (std::min)(1380, sw - 120), h = (std::min)(900, sh - 120);
    HWND win = CreateWindowExW(0, L"UltriaMailboxUI", L"Mailbox Checker — hotmail / outlook",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE, (sw - w) / 2, (sh - h) / 2, w, h,
                               nullptr, nullptr, instance, nullptr);

    g.kit.make(GetDpiForWindow(win));
    SetWindowPos(win, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);

    // showcase state (demo flag or env var): scored accounts, first box open
    bool showcase = demo_mode;
    if (!showcase) {
        wchar_t v[8]{};
        showcase = GetEnvironmentVariableW(L"UTRIA_MAIL_DEMO", v, 8) > 0;
    }
    if (showcase) {
        std::uniform_real_distribution<double> roll(0, 1);
        for (auto& r : g.rows) {
            double v = roll(g.rng);
            r.state = v < 0.58 ? ST_VALID : (v < 0.92 ? ST_BAD : ST_2FA);
            if (r.state == ST_VALID) ++g.n_valid;
            else if (r.state == ST_BAD) ++g.n_bad;
            else ++g.n_2fa;
        }
        for (int i = 0; i < (int)g.rows.size(); ++i)
            if (g.rows[i].state == ST_VALID) { open_mailbox(i); break; }
        SetTimer(win, 9, 2600, nullptr);
        refresh();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Gdiplus::GdiplusShutdown(token);
    return 0;
}

} // namespace gui
