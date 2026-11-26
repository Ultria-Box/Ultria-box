#pragma once
// Shared UI kit for the Ultria apps (checker + studio). Win32/GDI+ dark theme:
// palette, rounded shapes, gradients, text drawing, the U mark, DPI scaling.
// Header-only on purpose: both apps compile it directly, zero linkage coupling.
#include <windows.h>
typedef ULONG PROPID; // mingw gdiplus headers require it explicitly
#include <gdiplus.h>
#include <cstdint>
#include <string>

namespace gui {

// ---------------- palette ----------------
namespace col {
constexpr uint32_t bg      = 0x0D1017;
constexpr uint32_t panel   = 0x12161F;
constexpr uint32_t panel2  = 0x0F131B;
constexpr uint32_t border  = 0x212A3B;
constexpr uint32_t accent1 = 0x00E5FF;
constexpr uint32_t accent2 = 0x7B2FFF;
constexpr uint32_t text    = 0xE6ECF5;
constexpr uint32_t sub     = 0x8A93A6;
constexpr uint32_t dim     = 0x59627A;
constexpr uint32_t hit     = 0x2DD4A7;
constexpr uint32_t bad     = 0xFF5C74;
constexpr uint32_t wait_   = 0x6B7686;
constexpr uint32_t row_a   = 0x0E1219;
constexpr uint32_t row_b   = 0x0B0E14;
constexpr uint32_t sel     = 0x14314F;
constexpr uint32_t warn    = 0xF5B84C;
} // namespace col

inline Gdiplus::Color gd(uint32_t c, BYTE a = 255) {
    return Gdiplus::Color(a, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// ---------------- string helpers ----------------
inline std::wstring wstr(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}
inline std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// ---------------- shapes ----------------
inline void round_path(Gdiplus::GraphicsPath& p, Gdiplus::RectF r, float rad) {
    if (r.Width <= 0 || r.Height <= 0) return;
    float d = (std::min)(rad * 2, (std::min)(r.Width, r.Height));
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

inline void round_rect(Gdiplus::Graphics& gr, Gdiplus::RectF r, float rad, uint32_t fill, BYTE a = 255) {
    Gdiplus::GraphicsPath p;
    round_path(p, r, rad);
    Gdiplus::SolidBrush b(gd(fill, a));
    gr.FillPath(&b, &p);
}

inline void round_border(Gdiplus::Graphics& gr, Gdiplus::RectF r, float rad, uint32_t border, float width = 1.0f) {
    Gdiplus::GraphicsPath p;
    round_path(p, r, rad);
    Gdiplus::Pen pen(gd(border), width);
    gr.DrawPath(&pen, &p);
}

inline void grad_round(Gdiplus::Graphics& gr, Gdiplus::RectF r, float rad, bool vertical = false) {
    Gdiplus::GraphicsPath p;
    round_path(p, r, rad);
    Gdiplus::LinearGradientBrush b(r, gd(col::accent1), gd(col::accent2), vertical ? 90.0f : 0.0f);
    gr.FillPath(&b, &p);
}

// ---------------- text ----------------
inline void draw_text(Gdiplus::Graphics& gr, Gdiplus::Font* font, const std::wstring& s,
                      Gdiplus::RectF r, uint32_t color, int align = 0) {
    Gdiplus::SolidBrush b(gd(color));
    Gdiplus::StringFormat fmt;
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetAlignment(align == 0 ? Gdiplus::StringAlignmentNear
                                : (align == 1 ? Gdiplus::StringAlignmentCenter : Gdiplus::StringAlignmentFar));
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    gr.DrawString(s.c_str(), (INT)s.size(), font, r, &fmt, &b);
}

// ---------------- the U mark (mirrors assets/logo.png) ----------------
inline void draw_logo(Gdiplus::Graphics& gr, float x, float y, float size) {
    Gdiplus::GraphicsPath u;
    u.AddLine(x+size*.22f,y+size*.18f,x+size*.22f,y+size*.53f);
    u.AddArc(x+size*.22f,y+size*.25f,size*.56f,size*.56f,180,-180);
    u.AddLine(x+size*.78f,y+size*.53f,x+size*.78f,y+size*.12f);
    Gdiplus::LinearGradientBrush lb(Gdiplus::RectF(x,y,size,size), gd(col::accent1), gd(col::accent2), 18.0f);
    Gdiplus::Pen pen(&lb,size*.16f);
    gr.DrawPath(&pen, &u);
}

// ---------------- fonts / DPI ----------------
struct Kit {
    int dpi = 96;
    Gdiplus::Font *f_title = nullptr, *f_h1 = nullptr, *f_sub = nullptr,
                  *f_body = nullptr, *f_bold = nullptr, *f_small = nullptr, *f_number = nullptr;
    HFONT edit_font = nullptr;

    int dp(int v) const { return MulDiv(v, dpi, 96); }

    void clear() {
        auto del = [](Gdiplus::Font*& f) { delete f; f = nullptr; };
        del(f_title); del(f_h1); del(f_sub); del(f_body); del(f_bold); del(f_small); del(f_number);
        if (edit_font) DeleteObject(edit_font);
        edit_font = nullptr;
    }
    void make(int dpi_) {
        clear(); dpi = dpi_;
        auto mk = [&](int pt, int style) {
            return new Gdiplus::Font(L"Segoe UI", (Gdiplus::REAL)MulDiv(pt, dpi, 96), style,
                                     Gdiplus::UnitPixel);
        };
        f_title = mk(32, Gdiplus::FontStyleBold);
        f_number = mk(25, Gdiplus::FontStyleBold);
        f_h1 = mk(18, Gdiplus::FontStyleBold);
        f_sub = mk(13, Gdiplus::FontStyleRegular);
        f_body = mk(13, Gdiplus::FontStyleRegular);
        f_bold = mk(12, Gdiplus::FontStyleBold);
        f_small = mk(11, Gdiplus::FontStyleRegular);
        edit_font = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
};

} // namespace gui
