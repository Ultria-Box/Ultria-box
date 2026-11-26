// Ultria Checker: a local-only dataset review workspace.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include "gui.h"
#include "ui_kit.h"
#include "modules80.h"
#include "../core/sys.h"
#include "../common/combo.h"
#include <atomic>
#include <cstdint>
#include <fstream>
#include <unordered_set>

namespace gdip = Gdiplus;
namespace gui {
struct Row { combo::Entry e; int status=0, modules=0; };
struct App {
    HWND win{},list{},edit{}; Kit kit; HBRUSH editbrush{};
    std::vector<Row> rows; std::vector<int> view; std::unordered_set<uint64_t> seen;
    std::atomic<bool> importing{false}, import_cancel{false};
    std::atomic<unsigned long long> prog_bytes{0}, prog_total{1}, prog_lines{0}, prog_unique{0};
    size_t import_base = 0;
    std::string query; int mode=0; bool enabled[guimod::kModuleCount]{};
    bool running=false; int percent=0,hits=0,bad=0; size_t run_pos=0; ULONGLONG started=0;
    double run_acc=0.0; ULONGLONG last_tick=0;
    int page=0, pageSize=100; std::vector<std::array<std::wstring,4>> pageCells;
    RECT listbox{}; std::wstring toast; int hot=0,down=0,scroll=0,drag=0,listHover=-1;
} a;
int d(int n){return a.kit.dp(n);} int logical(int n){return MulDiv(n,96,a.kit.dpi);}
void refresh(){if(a.win)InvalidateRect(a.win,nullptr,FALSE);}
void notify(std::wstring s){a.toast=std::move(s);SetTimer(a.win,2,4500,nullptr);refresh();}
std::wstring fmt_count(unsigned long long n){std::wstring t=std::to_wstring(n),o;int c=0;for(auto it=t.rbegin();it!=t.rend();++it){if(c&&c%3==0)o.push_back(L' ');o.push_back(*it);++c;}std::reverse(o.begin(),o.end());return o;}

// The frame is laid out in logical pixels. Only native-control boundaries and
// painting primitives cross into device pixels.
struct Box {int x=0,y=0,w=0,h=0; int right()const{return x+w;} int bottom()const{return y+h;}
    bool has(int u,int v)const{return u>=x && u<right() && v>=y && v<bottom();}
    gdip::RectF rf()const{return {(float)d(x),(float)d(y),(float)(d(right())-d(x)),(float)(d(bottom())-d(y))};}
    RECT rc()const{return {d(x),d(y),d(right()),d(bottom())};}
};
enum Target{Nil,Import,Export,Search,Mode,All,None,Run,EmptyAction,Scroll,First,Prev,Next,Last,Size,Minimize,Maximize,Close,NTargets,Service=100};
int pages(){return std::max(1,(int)((a.view.size()+a.pageSize-1)/a.pageSize));}
bool pageDisabled(int id){return ((id==First||id==Prev)&&a.page==0)||((id==Next||id==Last)&&a.page>=pages()-1);}
struct Layout {int w,h; Box ledger,scope,items; std::array<Box,NTargets> b{};} l;
int active(){return (int)std::count(std::begin(a.enabled),std::end(a.enabled),true);}
int range(){return std::max(0,guimod::kModuleCount*36-l.items.h);}
Box thumb(){Box b=l.b[Scroll];int h=std::max(26,b.h*b.h/(36*guimod::kModuleCount));return {b.x+3,b.y+(range()?(b.h-h)*a.scroll/range():0),4,h};}
void create_list();void place_list();void table_dpi();LRESULT list_notify(NMHDR*);void table_header(gdip::Graphics&);
void start_import(std::vector<std::string>);void filter_rows();void change_page(int);void import_files();void export_rows();void start_run();void tick_run();
void layout(){
    RECT r{};GetClientRect(a.win,&r);int w=l.w=logical(r.right),h=l.h=logical(r.bottom); if(w<300||h<400)return;
    int sw=w>=1180?346:304; auto &b=l.b;
    l.ledger={24,173,w-sw-64,h-237};l.scope={w-24-sw,173,sw,h-237};Box p=l.ledger,s=l.scope;
    b[Import]={p.x+18,p.y+61,100,36};b[Export]={p.x+126,p.y+61,91,36};
    b[Mode]={p.right()-100,p.y+61,82,36};b[Search]={p.x+229,p.y+61,b[Mode].x-(p.x+229)-8,36};
    a.listbox=Box{p.x+1,p.y+143,p.w-2,p.h-199}.rc();
    int fy=p.bottom()-42;int end=p.right()-18;
    b[Last]={end-30,fy,30,28};b[Next]={end-64,fy,30,28};b[Prev]={end-98,fy,30,28};b[First]={end-132,fy,30,28};b[Size]={end-361,fy,96,28};
    b[All]={s.right()-108,s.y+53,39,23};b[None]={s.right()-65,s.y+53,47,23};
    l.items={s.x+8,s.y+97,s.w-20,s.h-187};b[Scroll]={s.right()-12,l.items.y,10,l.items.h};
    a.scroll=std::clamp(a.scroll,0,range());b[Run]={s.x+18,s.bottom()-54,s.w-36,38};
    b[EmptyAction]={p.x+(p.w-158)/2,(logical(a.listbox.top)+logical(a.listbox.bottom))/2+54,158,32};
    for(int i=0;i<3;i++) b[Minimize+i]={w-126+i*42,0,42,48};
    if(a.edit){Box e=b[Search];SetWindowPos(a.edit,nullptr,d(e.x+34),d(e.y+10),d(std::max(1,e.w-44)),d(18),SWP_NOZORDER);}
    if(a.list)place_list();
}

using G=gdip::Graphics;
void text(G&g,Box b,const std::wstring&s,gdip::Font*f,uint32_t c=col::text,int align=0){draw_text(g,f,s,b.rf(),c,align);}
void rect(G&g,Box b,uint32_t c){gdip::SolidBrush br(gd(c));g.FillRectangle(&br,b.rf());}
void round(G&g,Box b,int rad,uint32_t c){round_rect(g,b.rf(),(float)d(rad),c);}
void stroke(G&g,Box b,int rad,uint32_t c){round_border(g,b.rf(),(float)d(rad),c,(float)d(1));}
void line(G&g,int x,int y,int u,int v,uint32_t c,float width=1){gdip::Pen p(gd(c),width*d(1));g.DrawLine(&p,d(x),d(y),d(u),d(v));}
void dot(G&g,int x,int y,int size,uint32_t c){gdip::SolidBrush b(gd(c));g.FillEllipse(&b,d(x),d(y),d(size),d(size));}
void card(G&g,Box b){auto sub=b; sub.y+=3;round_rect(g,sub.rf(),d(14),0,40);sub.y-=2;round_rect(g,sub.rf(),d(14),0,36);round(g,b,14,col::panel);stroke(g,b,14,col::border);}
uint32_t state(int id,uint32_t base,uint32_t hover=0x242D3D){return a.down==id&&a.hot==id?0x303E52:a.hot==id?hover:base;}
enum Mark{Plus,Out,Find,Filter,Play,Stack,Scope,Shield,Check,Cross,Grid,Arrow,Dash,Window};
void icon(G&g,Mark m,int x,int y,uint32_t c){ // centered on a 16 px grid
    auto seg=[&](int x1,int y1,int x2,int y2){line(g,x+x1,y+y1,x+x2,y+y2,c,1.5f);};
    switch(m){
    case Plus:seg(8,3,8,13);seg(3,8,13,8);break;
    case Out:seg(8,2,8,10);seg(5,7,8,10);seg(8,10,11,7);seg(2,10,2,14);seg(2,14,14,14);seg(14,14,14,10);break;
    case Find:{gdip::Pen p(gd(c),1.5f*d(1));g.DrawEllipse(&p,d(x+2),d(y+2),d(9),d(9));seg(10,10,14,14);break;}
    case Filter:seg(2,4,14,4);seg(4,8,12,8);seg(6,12,10,12);break;
    case Play:seg(4,2,13,8);seg(13,8,4,14);seg(4,14,4,2);break;
    case Stack:seg(1,5,8,1);seg(8,1,15,5);seg(15,5,8,9);seg(8,9,1,5);seg(1,9,8,13);seg(8,13,15,9);break;
    case Scope:case Grid:for(int i=0;i<4;i++)stroke(g,{x+1+(i%2)*8,y+1+(i/2)*8,5,5},1,c);break;
    case Shield:seg(2,3,8,1);seg(8,1,14,3);seg(14,3,13,10);seg(13,10,8,15);seg(8,15,3,10);seg(3,10,2,3);break;
    case Check:seg(3,8,7,12);seg(7,12,14,4);break;
    case Cross:seg(4,4,12,12);seg(12,4,4,12);break;
    case Arrow:seg(2,8,13,8);seg(8,3,13,8);seg(13,8,8,13);break;
    case Dash:seg(3,8,13,8);break;case Window:stroke(g,{x+3,y+3,10,10},1,c);break;
    }
}
void control(G&g,int id,const wchar_t*s,Mark m,bool primary=false){Box b=l.b[id];
    if(primary){grad_round(g,b.rf(),d(10));if(a.hot==id)round_rect(g,b.rf(),d(10),a.down==id?0:0xffffff,32);}
    else {round(g,b,9,state(id,0x191F2A));stroke(g,b,9,0x30394A);}
    auto ink=primary?0x09131F:col::text; icon(g,m,b.x+12,b.y+(b.h-16)/2,ink);text(g,{b.x+32,b.y,b.w-36,b.h},s,a.kit.f_bold,ink);
}
void masthead(G&g){int w=l.w;
    rect(g,{0,0,w,48},0x10141C);rect(g,{0,48,w,1},col::border);
    draw_logo(g,d(23),d(9),d(31));text(g,{63,0,140,48},L"ULTRIA CHECKER",a.kit.f_bold);
    round(g,{187,14,39,20},7,0x1B2230);text(g,{187,14,39,20},L"v1.4",a.kit.f_small,col::sub,1);
    rect(g,{247,15,1,18},col::border);text(g,{264,0,170,48},L"Audit workspace",a.kit.f_sub,col::sub);
    icon(g,Shield,w-267,16,col::sub);text(g,{w-243,0,112,48},L"Offline session",a.kit.f_sub,col::sub);rect(g,{w-132,14,1,20},col::border);
    for(int i=0;i<3;i++){int id=Minimize+i;auto b=l.b[id];if(a.hot==id)round(g,{b.x+3,6,b.w-6,36},7,state(id,0x17212C));icon(g,i==0?Dash:i==1?Window:Cross,b.x+13,16,col::sub);}
    text(g,{24,66,w-510,52},L"Exposure review",a.kit.f_title);
    text(g,{25,121,w-504,21},L"Review connection records and estimate service exposure locally.",a.kit.f_sub,col::sub);
    const wchar_t* names[]={L"TOTAL",L"HITS",L"BAD"};int vals[]={(int)a.rows.size(),a.hits,a.bad};uint32_t inks[]={col::text,col::hit,col::bad};
    for(int i=0;i<3;i++){Box b{w-24-(3-i)*118-(2-i)*8,71,118,78};card(g,b);
        std::wstring sv=std::to_wstring(vals[i]);
        Gdiplus::Font* font=sv.size()<=5?a.kit.f_number:(sv.size()<=7?a.kit.f_h1:a.kit.f_body);
        text(g,{b.x+13,b.y+8,78,18},names[i],a.kit.f_small,col::sub);text(g,{b.x+12,b.y+27,100,41},sv,font,inks[i]);
        icon(g,i==0?Stack:i==1?Check:Cross,b.right()-30,b.y+12,i==0?col::sub:inks[i]);}
}
// A stack of source sheets on a registration grid, drawn at device resolution.
void illustration(G&g,int cx,int cy){
    for(int j=-4;j<=4;j++)for(int i=-12;i<=12;i++)if(abs(i)+abs(j)*2<17)dot(g,cx+8*i,cy+8*j,2,0x293344);
    for(int i=0;i<3;i++){Box b{cx-33+i*6,cy-32-i*5,54,60};round(g,b,7,0x171F2D+i*0x020304);stroke(g,b,7,0x344158+i*0x050607);
        if(i==2){draw_logo(g,d(b.x+6),d(b.y+6),d(21));for(int k=0;k<3;k++)round(g,{b.x+10,b.y+32+k*6,31-k*4,2},1,0x596881);}}
}
void ledger(G&g){auto p=l.ledger;card(g,p);icon(g,Stack,p.x+19,p.y+20,0xACBBD2);text(g,{p.x+44,p.y+8,200,40},L"Dataset records",a.kit.f_h1);
    std::wstring count=std::to_wstring(a.view.size())+L" shown";text(g,{p.right()-138,p.y+10,120,36},count,a.kit.f_sub,col::sub,2);
    control(g,Import,L"IMPORT",Plus,true);control(g,Export,L"EXPORT",Out);
    Box b=l.b[Search];round(g,b,10,col::panel2);stroke(g,b,10,GetFocus()==a.edit?col::accent1:col::border);icon(g,Find,b.x+11,b.y+10,col::sub);
    static constexpr const wchar_t* modes[]={L"ALL",L"URL",L"LOGIN",L"PASS"};control(g,Mode,modes[a.mode],Filter);
    table_header(g);
    if(a.view.empty()){int cx=p.x+p.w/2,cy=(logical(a.listbox.top)+logical(a.listbox.bottom))/2;
        illustration(g,cx,cy-49);bool empty=a.rows.empty();text(g,{p.x+16,cy-14,p.w-32,32},empty?L"Import a combo file to begin":L"No matches for “"+wstr(a.query)+L"”",a.kit.f_h1,col::text,1);
        text(g,{p.x+14,cy+19,p.w-28,25},empty?L"Choose a local text file containing connection records.":L"Try another search term or switch the field filter.",a.kit.f_sub,col::sub,1);
        auto q=l.b[EmptyAction];round(g,q,8,state(EmptyAction,0x1E2633));stroke(g,q,8,0x364153);text(g,q,empty?L"Choose files…":L"Clear search",a.kit.f_bold,col::text,1);
    }
    rect(g,{p.x+1,p.bottom()-56,p.w-2,1},col::border);
    int begin=a.page*a.pageSize;auto sb=l.b[Size];
    text(g,{p.x+18,sb.y,sb.x-p.x-28,sb.h},L"Showing "+std::to_wstring(a.view.empty()?0:begin+1)+L"–"+std::to_wstring(begin+a.pageCells.size())+L" of "+std::to_wstring(a.view.size()),a.kit.f_sub,col::sub);
    text(g,{sb.right()+4,sb.y,l.b[First].x-sb.right()-8,sb.h},L"Page "+std::to_wstring(a.page+1)+L" / "+std::to_wstring(pages()),a.kit.f_sub,col::sub,1);
    for(int id=First;id<=Size;id++){Box b=l.b[id];bool off=pageDisabled(id);round(g,b,7,off?col::panel2:state(id,0x1A2230));stroke(g,b,7,col::border);
        const wchar_t* arrows[]={L"«",L"‹",L"›",L"»"};text(g,b,id==Size?std::to_wstring(a.pageSize)+L" / page":arrows[id-First],id==Size?a.kit.f_small:a.kit.f_h1,off?col::dim:col::text,1);}
}
void scope(G&g){auto s=l.scope;card(g,s);icon(g,Scope,s.x+19,s.y+20,0xACBBD2);text(g,{s.x+44,s.y+8,s.w-62,40},L"Service scope",a.kit.f_h1);
    int on=active();text(g,{s.x+18,s.y+50,160,28},std::to_wstring(on)+L" / 80 active",a.kit.f_sub,col::sub);
    for(int id:{All,None}){auto b=l.b[id];round(g,b,7,state(id,0x1A202C));stroke(g,b,7,col::border);text(g,b,id==All?L"ALL":L"NONE",a.kit.f_small,col::text,1);}
    int span=s.w-36;for(int i=0;i<guimod::kModuleCount;i++){int x=s.x+18+span*i/80;rect(g,{x,s.y+84,std::max(1,span*(i+1)/80-span*i/80-1),3},a.enabled[i]?0x72849F:0x273040);}
    auto st=g.Save();g.SetClip(l.items.rf(),gdip::CombineModeIntersect);
    for(int i=a.scroll/36;i<guimod::kModuleCount;i++){int y=l.items.y+i*36-a.scroll;if(y>=l.items.bottom())break;int id=Service+i;
        if(a.hot==id)round(g,{l.items.x,y,l.items.w,35},7,state(id,col::panel));
        auto m=guimod::kModules[i];
        Box tile{s.x+18,y+7,23,23};round(g,tile,7,0x1B222E);dot(g,tile.x+8,tile.y+8,7,m.color);
        text(g,{s.x+50,y,s.w-176,36},wstr(m.name),a.kit.f_body,a.enabled[i]?col::text:col::sub);
        text(g,{s.right()-144,y,90,36},wstr(m.category),a.kit.f_small,col::sub,2);
        Box box{s.right()-38,y+10,16,16};round(g,box,4,a.enabled[i]?0xB7C5DA:col::panel2);stroke(g,box,4,a.enabled[i]?0xB7C5DA:0x465166);if(a.enabled[i])icon(g,Check,box.x,box.y,0x1C2531);
    }g.Restore(st);if(range())round(g,thumb(),2,state(Scroll,0x3B475C));
    int fy=s.bottom()-84;rect(g,{s.x+1,fy,s.w-2,1},col::border);
    text(g,{s.x+15,fy+4,s.w-30,22},L"Local simulation  ·  No sign-in attempts",a.kit.f_small,col::sub,1);
    auto b=l.b[Run];bool ready=!a.rows.empty()&&on;round(g,b,10,state(Run,ready?0xD2DCEF:0x202735));
    if(ready&&a.hot==Run)round(g,b,10,a.down==Run?0xB1BFD3:0xE3EAF6);
    if(a.running){auto sv=g.Save();g.SetClip(Box{b.x,b.y,b.w*a.percent/100,b.h}.rf(),gdip::CombineModeIntersect);grad_round(g,b.rf(),d(10));g.Restore(sv);}
    uint32_t ink=ready?0x162132:col::sub;text(g,b,a.running?L"ANALYZING "+std::to_wstring(a.percent)+L"%":L"RUN ASSESSMENT",a.kit.f_bold,ink,1);icon(g,Play,b.x+14,b.y+11,ink);
}
void status(G&g){int w=l.w,y=l.h-40;rect(g,{0,y,w,1},col::border);
    if(a.importing.load()){dot(g,24,y+17,7,col::accent1);text(g,{39,y,127,40},L"Importing",a.kit.f_small,col::sub);rect(g,{160,y+13,1,14},col::border);
        unsigned long long ln=a.prog_lines.load(),un=a.prog_unique.load(),tt=a.prog_total.load(),dn=a.prog_bytes.load();int pc=-1;if(tt>0)pc=(int)(100ull*dn/tt);
        text(g,{174,y,w-540,40},fmt_count(ln)+L" lines read  ·  "+fmt_count(un)+L" unique"+(pc>=0?L"  ·  ~"+std::to_wstring(pc)+L"%":L""),a.kit.f_small,col::sub);return;}
    dot(g,24,y+17,7,a.running?col::accent1:col::hit);text(g,{39,y,150,40},a.running?L"Simulating · 10/s":L"Local engine ready",a.kit.f_small,col::sub);
    rect(g,{160,y+13,1,14},col::border);text(g,{174,y,w-540,40},fmt_count(a.rows.size())+L" records   /   "+fmt_count(a.view.size())+L" matching",a.kit.f_small,col::sub);
    if(a.toast.empty())text(g,{w-290,y,266,40},L"Processed on this device",a.kit.f_small,col::sub,2);
    else{Box b{w-344,y+6,320,28};round(g,b,7,0x1B2331);round(g,{b.x+1,b.y+7,2,14},1,col::accent1);text(g,{b.x+8,b.y,b.w-19,b.h},a.toast,a.kit.f_small,col::text,2);}
}
void paint(){PAINTSTRUCT ps{};HDC dc=BeginPaint(a.win,&ps);RECT r{};GetClientRect(a.win,&r);HDC mem=CreateCompatibleDC(dc);auto bm=CreateCompatibleBitmap(dc,r.right,r.bottom);auto old=SelectObject(mem,bm);
    {G g(mem);g.SetSmoothingMode(gdip::SmoothingModeAntiAlias);g.SetTextRenderingHint(gdip::TextRenderingHintClearTypeGridFit);g.Clear(gd(col::bg));masthead(g);ledger(g);scope(g);status(g);}
    BitBlt(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,mem,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);SelectObject(mem,old);DeleteObject(bm);DeleteDC(mem);EndPaint(a.win,&ps);}
// Results table. The native control owns scrolling, selection, and accessibility;
// its hidden columns use the same geometry as the painted header and rows.
static const wchar_t* const table_names[] = {L"#", L"URL", L"LOGIN", L"PASSWORD", L"STATUS", L"MODULES"};
static int table_widths[6]{};
static int table_hover = -1;
static bool table_sizing = false;

static void table_measure(int width, int (&w)[6]) {
    // Reserve the compact fields; give remaining space to the two long strings.
    // On very narrow layouts collapse the password column before squeezing identity.
    w[0] = d(width < d(580) ? 32 : 44);
    w[3] = width < d(420) ? 0 : d(width < d(640) ? 68 : 86);
    w[4] = d(68);
    w[5] = d(62);
    if (width < d(300)) { w[0] = 0; w[5] = 0; }
    w[4] = (std::min)(w[4], width);
    const int flex = width - w[0] - w[3] - w[4] - w[5];
    w[1] = MulDiv(flex, 46, 100);
    w[2] = flex - w[1];
}

static void table_fit() {
    if (!a.list || table_sizing) return;
    table_sizing = true;
    bool changed = false;
    // Shrink before expanding: intermediate column widths must not leave an
    // unnecessary horizontal scrollbar after a resize or a filter operation.
    for (int attempt = 0; attempt < 3; ++attempt) {
        RECT rc{}; GetClientRect(a.list, &rc);
        int w[6]; table_measure((int)rc.right, w);
        for (int grow = 0; grow <= 1; ++grow) for (int c = 0; c < 6; ++c) {
            if (w[c] == table_widths[c] || (w[c] > table_widths[c]) != (grow != 0)) continue;
            table_widths[c] = w[c];
            ListView_SetColumnWidth(a.list, c, w[c]);
            changed = true;
        }
        RECT now{}; GetClientRect(a.list, &now);
        if (now.right == rc.right) break;
    }
    table_sizing = false;
    if (changed) {
        RECT header = a.listbox; header.bottom = header.top; header.top -= d(30);
        InvalidateRect(a.win, &header, FALSE);
        InvalidateRect(a.list, nullptr, FALSE);
    }
}

static const Row* table_row(int index) {
    if (index < 0 || (size_t)index >= a.pageCells.size()) return nullptr;
    const int source = a.view[a.page*a.pageSize+index];
    return source >= 0 && (size_t)source < a.rows.size() ? &a.rows[source] : nullptr;
}

static const wchar_t* table_status(int st) { return st == 1 ? L"HIT" : st == 2 ? L"BAD" : L"WAIT"; }

static std::wstring table_value(const Row& r, int index, int column, bool visual) {
    switch (column) {
        case 0:case 1:case 2:case 3:return a.pageCells[index][column];
        case 4: return table_status(r.status);
        case 5: return std::to_wstring(r.modules);
    }
    return {};
}

static gdip::RectF table_cell(int x, int y, int w, int h, int inset) {
    return {(float)(x + inset), (float)y, (float)(std::max)(0, w - 2 * inset), (float)h};
}

void table_header(gdip::Graphics& gr) {
    const RECT& r = a.listbox;
    int x = r.left, h = d(30), top = r.top - h, width = r.right - r.left;
    if (width <= 0 || h <= 0) return;
    gdip::SolidBrush bg(gd(col::panel2)), line(gd(col::border));
    gr.FillRectangle(&bg, x, top, width, h);
    int fallback[6]; table_measure(width, fallback);
    const int* widths = a.list ? table_widths : fallback;
    for (int c = 0; c < 6; ++c) {
        int w = widths[c];
        if(c && w>0)gr.FillRectangle(&line,x,top+d(9),d(1),d(12));
        if (w > 0) draw_text(gr, a.kit.f_small, table_names[c], table_cell(x, top, w, h, c >= 4 || c == 0 ? 0 : d(5)),
                             col::sub, c == 0 || c >= 4 ? 1 : 0);
        x += w;
    }
    gr.FillRectangle(&line, (int)r.left, (int)r.top - 1, width, 1);
}

static void table_badge(gdip::Graphics& gr, const Row& r, int x, int y, int w, int h, bool status) {
    int bw = (std::min)(w - d(6), d(status ? 54 : (r.modules > 99 ? 38 : 30)));
    int bh = (std::min)(h - d(4), d(19));
    if (bw < d(10) || bh < 2) return;
    gdip::RectF b((float)x + (w - bw) / 2.f, (float)y + (h - bh) / 2.f, (float)bw, (float)bh);
    uint32_t ink = status ? (r.status == 1 ? col::hit : r.status == 2 ? col::bad : 0xA4AEC1) :
                            (r.modules ? 0xB2C8FF : col::sub);
    uint32_t fill = status ? (r.status == 1 ? 0x15352E : r.status == 2 ? 0x3A202A : 0x232B39) :
                             (r.modules ? 0x202E48 : 0x1A212D);
    round_rect(gr, b, (std::min)(b.Width, b.Height) * .5f, fill);
    if (status && bw >= d(48)) {
        const float dot = (float)d(5);
        gdip::SolidBrush mark(gd(ink));
        gr.FillEllipse(&mark, b.X + (float)d(8), b.Y + (b.Height - dot) / 2.f, dot, dot);
        b.X += (float)d(12); b.Width -= (float)d(14);
    }
    draw_text(gr, a.kit.f_bold, status ? std::wstring(table_status(r.status)) : std::to_wstring(r.modules), b, ink, 1);
}

static LRESULT table_draw(NMLVCUSTOMDRAW& cd) {
    if (cd.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd.nmcd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    int i = (int)cd.nmcd.dwItemSpec;
    const Row* r = table_row(i);
    if (!r) return CDRF_SKIPDEFAULT;
    RECT item{}, rc{};
    if (!ListView_GetItemRect(a.list, i, &item, LVIR_BOUNDS)) return CDRF_SKIPDEFAULT;
    GetClientRect(a.list, &rc);
    int y = item.top, h = item.bottom - y, width = rc.right;
    UINT state = ListView_GetItemState(a.list, i, LVIS_SELECTED | LVIS_FOCUSED);
    bool selected = (state & LVIS_SELECTED) != 0;
    gdip::Graphics gr(cd.nmcd.hdc);
    gr.SetPageUnit(gdip::UnitPixel);
    gr.SetSmoothingMode(gdip::SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(gdip::TextRenderingHintClearTypeGridFit);
    gdip::SolidBrush bg(gd(selected ? col::sel : table_hover == i ? 0x1A2330 : i % 2 ? col::row_b : col::row_a));
    gr.FillRectangle(&bg, 0, y, width, h);
    for (int c = 0, x = 0; c < 6; x += table_widths[c++]) {
        int w = table_widths[c];
        if (w <= 0) continue;
        if (c >= 4) { table_badge(gr, *r, x, y, w, h, c == 4); continue; }
        draw_text(gr, c == 0 ? a.kit.f_small : a.kit.f_body, table_value(*r, i, c, true),
                  table_cell(x, y, w, h, c == 0 ? 0 : d(5)),
                   c == 0 ? col::dim : col::text, c == 0 ? 1 : 0);
    }
    if (selected) { gdip::SolidBrush mark(gd(col::accent1)); gr.FillRectangle(&mark, 0, y + 1, d(2), h - 2); }
    if ((state & LVIS_FOCUSED) && GetFocus() == a.list &&
        !(SendMessageW(a.list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) && width > 2 && h > 2) {
        gdip::Pen focus(gd(col::sub)); focus.SetDashStyle(gdip::DashStyleDot);
        gr.DrawRectangle(&focus, 1, y + 1, width - 3, h - 3);
    }
    return CDRF_SKIPDEFAULT;
}

static void table_set_hover(HWND hwnd, int index) {
    int old = table_hover;
    if (old == index) return;
    table_hover = index;
    if (old >= 0) ListView_RedrawItems(hwnd, old, old);
    if (index >= 0) ListView_RedrawItems(hwnd, index, index);
}

static void table_track(HWND hwnd, POINT pt) {
    LVHITTESTINFO hit{}; hit.pt = pt;
    RECT rc{}; GetClientRect(hwnd, &rc);
    table_set_hover(hwnd, PtInRect(&rc, pt) ? ListView_SubItemHitTest(hwnd, &hit) : -1);
}

// Some common-control implementations ignore DarkMode_Explorer. Keep the
// native scrollbar geometry and input handling, but paint its track ourselves.
static void table_scroll_chrome(HWND hwnd) {
    if (!(GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_VSCROLL)) return;
    RECT wr{},cr{};GetWindowRect(hwnd,&wr);GetClientRect(hwnd,&cr);
    const int x=cr.right,w=wr.right-wr.left-x,h=cr.bottom;
    if(w<3||h<=w*2)return;
    SCROLLINFO si{sizeof(si),SIF_ALL};if(!GetScrollInfo(hwnd,SB_VERT,&si))return;
    HDC dc=GetWindowDC(hwnd);if(!dc)return;
    {G g(dc);g.SetSmoothingMode(gdip::SmoothingModeAntiAlias);
     gdip::SolidBrush bg(gd(col::panel2));g.FillRectangle(&bg,x,0,w,h);
     const int track=h-2*w,span=std::max(1,si.nMax-si.nMin+1),page=(int)si.nPage;
     int ht=std::clamp(MulDiv(track,page,span),std::min(d(12),track),track);
     int pos=w+MulDiv(track-ht,si.nPos-si.nMin,std::max(1,span-page));
     round_rect(g,{float(x+w/2-d(2)),float(pos),float(d(4)),float(ht)},float(d(2)),0x495770);
     gdip::Pen p(gd(col::dim));for(int i=0;i<2;i++){int mid=i?h-w/2:w/2,dy=i?-2:2;
        g.DrawLine(&p,x+w/2-3,mid+dy,x+w/2,mid-dy);g.DrawLine(&p,x+w/2,mid-dy,x+w/2+3,mid+dy);}}
    ReleaseDC(hwnd,dc);
}
static LRESULT CALLBACK table_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, table_subclass, id);
        table_hover = -1;
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&t);
        table_track(hwnd, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
    } else if (msg == WM_MOUSELEAVE || (msg == WM_SHOWWINDOW && !wp)) table_set_hover(hwnd, -1);
    LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_SIZE || msg == LVM_SETITEMCOUNT) table_fit();
    if (msg == WM_MOUSEWHEEL || msg == WM_VSCROLL || msg == WM_KEYDOWN || msg == LVM_SETITEMCOUNT || msg == LVM_SCROLL) {
        POINT pt{};
        if (GetCursorPos(&pt) && WindowFromPoint(pt) == hwnd) { ScreenToClient(hwnd, &pt); table_track(hwnd, pt); }
        else table_set_hover(hwnd, -1);
    }
    if(msg==WM_NCPAINT||msg==WM_PAINT||msg==WM_VSCROLL||msg==WM_MOUSEWHEEL||msg==WM_NCMOUSEMOVE||msg==WM_NCLBUTTONUP)table_scroll_chrome(hwnd);
    return result;
}

void table_dpi() {
    if (!a.list) return;
    // A small-image list sets report item height without introducing fake items.
    // The control owns the current list; dispose of the one we just replaced.
    HIMAGELIST sizing = ImageList_Create(1, d(32), ILC_COLOR32, 1, 1);
    if (sizing) {
        HIMAGELIST old = ListView_SetImageList(a.list, sizing, LVSIL_SMALL);
        if (old) ImageList_Destroy(old);
    }
    table_fit();
    InvalidateRect(a.list, nullptr, FALSE);
}

void place_list() {
    if (!a.list) return;
    const RECT& r = a.listbox;
    SetWindowPos(a.list, nullptr, r.left, r.top, (std::max)(0L, r.right - r.left),
                 (std::max)(0L, r.bottom - r.top), SWP_NOZORDER | SWP_NOACTIVATE);
    table_fit();
}

void create_list() {
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&ic);
    a.list = CreateWindowExW(0, WC_LISTVIEWW, L"Results", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                            LVS_OWNERDATA | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                            0, 0, 1, 1, a.win, (HMENU)(INT_PTR)100, GetModuleHandleW(nullptr), nullptr);
    if (!a.list) return;
    SetWindowTheme(a.list, L"DarkMode_Explorer", nullptr);
    ListView_SetBkColor(a.list, gd(col::row_a).ToCOLORREF());
    ListView_SetTextBkColor(a.list, gd(col::row_a).ToCOLORREF());
    ListView_SetTextColor(a.list, gd(col::text).ToCOLORREF());
    ListView_SetExtendedListViewStyle(a.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetUnicodeFormat(a.list, TRUE);
    for (int c = 0; c < 6; ++c) {
        table_widths[c] = 0;
        LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_SUBITEM | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(table_names[c]); column.iSubItem = c;
        SendMessageW(a.list, LVM_INSERTCOLUMNW, c, (LPARAM)&column);
    }
    SetWindowSubclass(a.list, table_subclass, 1, 0);
    table_dpi(); place_list();
    ListView_SetItemCountEx(a.list, (int)a.pageCells.size(), 0);
}

LRESULT list_notify(NMHDR* nm) {
    if (!nm || !a.list || nm->hwndFrom != a.list) return 0;
    if (nm->code == NM_CUSTOMDRAW) return table_draw(*reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
    if (nm->code == LVN_GETDISPINFOW) {
        auto& item = reinterpret_cast<NMLVDISPINFOW*>(nm)->item;
        if ((item.mask & LVIF_TEXT) && item.pszText && item.cchTextMax > 0) {
            const Row* r = table_row(item.iItem);
            auto value = r ? table_value(*r, item.iItem, item.iSubItem, false) : std::wstring{};
            lstrcpynW(item.pszText, value.c_str(), item.cchTextMax);
        }
    } else if (nm->code == LVN_GETDISPINFOA) {
        auto& item = reinterpret_cast<NMLVDISPINFOA*>(nm)->item;
        if ((item.mask & LVIF_TEXT) && item.pszText && item.cchTextMax > 0) {
            item.pszText[0] = 0;
            if (const Row* r = table_row(item.iItem)) {
                auto value = table_value(*r, item.iItem, item.iSubItem, false);
                int n = WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (n > 0) {
                    std::string text(n, '\0');
                    WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, text.data(), n, nullptr, nullptr);
                    lstrcpynA(item.pszText, text.c_str(), item.cchTextMax);
                }
            }
        }
    }
    return 0;
}
// Local dataset operations. The search edit handler updates a.query before filtering.
namespace data_detail {
enum { waiting = 0, hit = 1, bad = 2 };

// Fast import path. Duplicate detection is a 64-bit FNV-1a hash of the three
// fields: O(1) per row, a fraction of the memory of a string set, and the hot
// loop never compares long strings.
inline uint64_t fnv_step(const char* d, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) { h ^= (unsigned char)d[i]; h *= 1099511628211ull; }
    return h;
}
inline uint64_t entry_hash(const combo::Entry& e) {
    uint64_t h = 1469598103934665603ull;
    h = fnv_step(e.url.data(), e.url.size(), h);
    h = fnv_step("\x01", 1, h);
    h = fnv_step(e.login.data(), e.login.size(), h);
    h = fnv_step("\x02", 1, h);
    h = fnv_step(e.pass.data(), e.pass.size(), h);
    return h;
}

// Only plausible account rows are kept: the login must be a well-formed
// email address (no phone numbers, no bare usernames) and the password must
// look like one (5-64 printable chars with at least one alphanumeric and no
// control characters). Everything else is counted as filtered.
bool valid_email(const std::string& s) {
    if (s.size() < 6 || s.size() > 254) return false;
    const size_t at = s.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= s.size()) return false;
    if (s.find('@', at + 1) != std::string::npos) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c == 0x7f) return false;
    }
    for (size_t i = 0; i < at; ++i) {
        char c = s[i];
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '%' ||
              c == '+' || c == '-' || c == '\''))
            return false;
    }
    const std::string dom = s.substr(at + 1);
    if (dom.empty() || dom.front() == '.' || dom.front() == '-' || dom.back() == '.' ||
        dom.back() == '-' || dom.find("..") != std::string::npos)
        return false;
    if (dom.find('.') == std::string::npos) return false;
    const size_t last = dom.rfind('.');
    const std::string tld = dom.substr(last + 1);
    if (tld.size() < 2) return false;
    for (char c : tld)
        if (!std::isalpha((unsigned char)c)) return false;
    for (char c : dom)
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-')) return false;
    return true;
}

bool valid_pass(const std::string& p) {
    if (p.size() < 5 || p.size() > 64) return false;
    bool alnum = false;
    for (unsigned char c : p) {
        if (c < 0x20 || c == 0x7f) return false;
        if (std::isalnum(c)) alnum = true;
    }
    return alnum;
}

bool add_entry(combo::Entry entry) {
    if (!a.seen.insert(entry_hash(entry)).second) return false;
    a.rows.push_back(Row{std::move(entry), waiting, 0});
    return true;
}

struct ImportStats { unsigned long long added = 0, duplicates = 0, skipped = 0, failed = 0, filtered = 0; };

// Streaming reader: big block reads over a wide-char path (unicode-safe on
// Windows), manual line split, zero per-byte overhead, periodic progress.
void read_rows_streaming(const std::string& path, ImportStats& stats) {
#ifdef _WIN32
    FILE* file = _wfopen(util::widen(path).c_str(), L"rb");
#else
    FILE* file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) { ++stats.failed; return; }
#ifdef _WIN32
    _fseeki64(file, 0, SEEK_END);
    long long fsize = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
#else
    std::fseek(file, 0, SEEK_END);
    long long fsize = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
#endif
    unsigned long long size = fsize > 0 ? (unsigned long long)fsize : 0;
    a.prog_total.fetch_add(size + 1);
    if (a.rows.capacity() < a.rows.size() + size / 96)
        a.rows.reserve(a.rows.size() + (size_t)(size / 96) + 1024);
    const unsigned long long lines_base = a.prog_lines.load();
    const unsigned long long bytes_base = a.prog_bytes.load();

    std::vector<char> buf(4u << 20);
    std::string carry;
    carry.reserve(8192);
    unsigned long long lines = 0, done_bytes = 0;

    auto process = [&](const char* data, size_t len) {
        if (len && data[len - 1] == '\r') --len;
        size_t first = 0;
        while (first < len && (data[first] == ' ' || data[first] == '\t')) ++first;
        if (first >= len || data[first] == '#') return;
        combo::Entry entry;
        std::string line(data + first, len - first);
        if (!combo::parse_line(line, entry)) { ++stats.skipped; return; }
        if (!valid_email(entry.login) || !valid_pass(entry.pass)) { ++stats.filtered; return; }
        if (add_entry(std::move(entry))) ++stats.added;
        else ++stats.duplicates;
    };

    for (;;) {
        if (a.import_cancel.load()) break;
        size_t got = std::fread(buf.data(), 1, buf.size(), file);
        if (!got) break;
        done_bytes += got;
        const char* p = buf.data();
        const char* end = p + got;
        while (p < end) {
            const char* nl = (const char*)std::memchr(p, '\n', (size_t)(end - p));
            if (!nl) { carry.append(p, (size_t)(end - p)); break; }
            size_t seg = (size_t)(nl - p);
            if (carry.empty()) process(p, seg);
            else { carry.append(p, seg); process(carry.data(), carry.size()); carry.clear(); }
            ++lines;
            p = nl + 1;
            if ((lines & 0x3FFF) == 0) {
                a.prog_lines.store(lines_base + lines);
                a.prog_unique.store(a.import_base + a.rows.size());
                a.prog_bytes.store(bytes_base + done_bytes);
            }
        }
    }
    if (!carry.empty()) { process(carry.data(), carry.size()); ++lines; }
    std::fclose(file);
    a.prog_lines.fetch_add(lines);
    a.prog_unique.store(a.import_base + a.rows.size());
    a.prog_bytes.fetch_add(done_bytes);
}

struct Job { std::vector<std::string> paths; unsigned long long t0 = 0; ImportStats stats; };

DWORD WINAPI import_proc(LPVOID param) {
    Job* job = static_cast<Job*>(param);
    for (const auto& p : job->paths) {
        if (a.import_cancel.load()) { delete job; return 0; }
        read_rows_streaming(p, job->stats);
    }
    if (!a.import_cancel.load()) PostMessageW(a.win, WM_APP + 10, 0, (LPARAM)job);
    else delete job;
    return 0;
}

std::mt19937 simulation_rng;
int run_modules = 0;
} // namespace data_detail

bool icontains(const std::string& hay, const std::string& lowNeedle) {
    const size_t n = lowNeedle.size();
    if (!n) return true;
    if (hay.size() < n) return false;
    const size_t last = hay.size() - n;
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        while (j < n && (char)std::tolower((unsigned char)hay[i + j]) == lowNeedle[j]) ++j;
        if (j == n) return true;
    }
    return false;
}

void filter_rows() {
    a.view.clear();
    a.view.reserve(a.rows.size());
    if (a.query.empty()) {
        for (size_t i = 0; i < a.rows.size(); ++i) a.view.push_back(static_cast<int>(i));
    } else {
        const std::string query = combo::lower(a.query);
        for (size_t i = 0; i < a.rows.size(); ++i) {
            const auto& e = a.rows[i].e;
            if (((a.mode == 0 || a.mode == 1) && icontains(e.url, query)) ||
                ((a.mode == 0 || a.mode == 2) && icontains(e.login, query)) ||
                ((a.mode == 0 || a.mode == 3) && icontains(e.pass, query)))
                a.view.push_back(static_cast<int>(i));
        }
    }
    change_page(0);
}

void change_page(int page) {
    a.page=std::clamp(page,0,pages()-1);
    a.pageCells.clear();
    const size_t start=(size_t)a.page*a.pageSize, end=std::min(start+a.pageSize,a.view.size());
    a.pageCells.reserve(end-start);
    for(size_t i=start;i<end;i++) {
        int source=a.view[i];const auto& e=a.rows[source].e;
        a.pageCells.push_back({std::to_wstring(source+1),wstr(e.url),wstr(e.login),e.pass.empty()?L"—":wstr(e.pass)});
    }
    if(a.list){table_hover=-1;ListView_SetItemState(a.list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemCountEx(a.list,(int)a.pageCells.size(),0);
        if(!a.pageCells.empty())ListView_EnsureVisible(a.list,0,FALSE);
        ShowWindow(a.list,a.pageCells.empty()?SW_HIDE:SW_SHOWNA);InvalidateRect(a.list,nullptr,FALSE);
    }refresh();
}

void import_files() {
    if (a.running) { notify(L"Import after the simulation finishes."); return; }
    std::vector<wchar_t> files(65536, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = a.win;
    ofn.lpstrTitle = L"Import dataset files";
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = files.data();
    ofn.nMaxFile = static_cast<DWORD>(files.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        if (CommDlgExtendedError()) notify(L"Could not open the selection. Try selecting fewer files.");
        return;
    }

    std::vector<std::string> paths;
    const std::wstring first(files.data());
    const std::string path = util::narrow(first);
    size_t pos = first.size() + 1;
    if (pos >= files.size() || !files[pos]) paths.push_back(path);
    else while (pos < files.size() && files[pos]) {
        const std::wstring name(files.data() + pos);
        paths.push_back(util::join(path, util::narrow(name)));
        pos += name.size() + 1;
    }
    start_import(std::move(paths));
}

// The heavy lifting runs on a worker thread so the window stays responsive on
// multi-million-line datasets: rows are appended there, the UI only reads the
// progress counters until the WM_APP+10 completion message arrives.
void start_import(std::vector<std::string> paths) {
    if (a.importing.load()) return;
    if (a.running) { notify(L"Import after the simulation finishes."); return; }
    auto* job = new data_detail::Job();
    job->paths = std::move(paths);
    job->t0 = GetTickCount64();
    a.import_base = a.rows.size();
    a.prog_bytes.store(0);
    a.prog_total.store(1);
    a.prog_lines.store(0);
    a.prog_unique.store(a.import_base);
    a.import_cancel.store(false);
    a.importing.store(true);
    EnableWindow(a.edit, FALSE);
    SetTimer(a.win, 3, 150, nullptr);
    HANDLE th = CreateThread(nullptr, 0, data_detail::import_proc, job, 0, nullptr);
    if (th) CloseHandle(th);
    else {
        a.importing.store(false);
        EnableWindow(a.edit, TRUE);
        KillTimer(a.win, 3);
        delete job;
        notify(L"Could not start the import worker.");
        return;
    }
    refresh();
}

void export_rows() {
    if (a.view.empty()) { notify(L"No matching rows to export."); return; }
    wchar_t file[MAX_PATH] = L"ultria_export.txt";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = a.win;
    ofn.lpstrTitle = L"Export visible rows with passwords and simulated status";
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        if (CommDlgExtendedError()) notify(L"Could not open the save dialog.");
        return;
    }
    std::vector<uint8_t> bytes;
    static const char* const names[] = {"WAIT", "HIT", "BAD"};
    for (int idx : a.view) {
        const Row& row = a.rows[idx];
        auto line = combo::to_line(row.e) + ":" + names[row.status] + "\r\n";
        bytes.insert(bytes.end(), line.begin(), line.end());
    }
    if (util::write_file(util::narrow(file), bytes))
        notify(std::to_wstring(a.view.size()) + L" rows exported with simulated status.");
    else notify(L"Could not write the export file.");
}

void start_run() {
    if (a.running) return;
    if (a.rows.empty()) { notify(L"Import rows before running a simulation."); return; }
    if (!SetTimer(a.win, 1, 20, nullptr)) { notify(L"Could not start the simulation timer."); return; }
    for (auto& row : a.rows) { row.status = data_detail::waiting; row.modules = 0; }
    a.hits = a.bad = a.percent = 0;
    a.run_pos = 0;
    a.run_acc = 0.0;
    a.last_tick = GetTickCount64();
    a.running = true;
    a.started = GetTickCount64();
    data_detail::simulation_rng.seed(static_cast<std::mt19937::result_type>(a.started));
    data_detail::run_modules = static_cast<int>(std::count(a.enabled, a.enabled + guimod::kModuleCount, true));
    if (a.list) InvalidateRect(a.list, nullptr, FALSE);
    refresh();
}

void tick_run() {
    if (!a.running) { KillTimer(a.win, 1); return; }
    // Wall-clock token bucket: at most 10 checks per second, so verdicts stay
    // believable on multi-million-row datasets (a real endpoint would throttle
    // long before that). A one-second clamp absorbs stalled ticks.
    const ULONGLONG now = GetTickCount64();
    ULONGLONG dt = now - a.last_tick;
    if (dt > 1000) dt = 1000;
    a.last_tick = now;
    a.run_acc += 10.0 * static_cast<double>(dt) / 1000.0;
    size_t budget = static_cast<size_t>(a.run_acc);
    a.run_acc -= static_cast<double>(budget);
    if (budget > 4096) budget = 4096;
    const size_t total = a.rows.size();
    const size_t end = (std::min)(a.run_pos + budget, total);
    const size_t before = a.run_pos;
    const int active = data_detail::run_modules;
    std::bernoulli_distribution is_hit(0.03);
    std::uniform_int_distribution<int> module_count(0, 4);
    for (; a.run_pos < end; ++a.run_pos) {
        Row& row = a.rows[a.run_pos];
        if (is_hit(data_detail::simulation_rng)) {
            row.status = data_detail::hit;
            row.modules = active ? (std::max)(1, active / 9) : 0;
            ++a.hits;
        } else {
            row.status = data_detail::bad;
            row.modules = (std::min)(active, module_count(data_detail::simulation_rng));
            ++a.bad;
        }
    }
    a.percent = total ? static_cast<int>(100ULL * a.run_pos / total) : 100;
    if (a.list && before != a.run_pos) InvalidateRect(a.list, nullptr, FALSE);
    if (a.run_pos == total) {
        a.percent = 100;
        a.running = false;
        KillTimer(a.win, 1);
        notify(L"Simulation finished · " + std::to_wstring(a.hits) + L" hits");
    } else if (before != a.run_pos) refresh();
}

int target(int x,int y){for(int i=1;i<NTargets;i++){if(!l.b[i].has(x,y))continue;if(i==EmptyAction&&!a.view.empty())continue;if(pageDisabled(i))return Nil;if((a.running||a.importing.load())&&(i==Import||i==All||i==None||i==Run))return Nil;return i;}
    if(l.items.has(x,y)&&!a.running){int n=(y-l.items.y+a.scroll)/36;if(n<guimod::kModuleCount)return Service+n;}return Nil;}
void scroll_to(int n){a.scroll=std::clamp(n,0,range());refresh();}
void drag_to(int y){int h=l.b[Scroll].h-thumb().h;if(h>0)scroll_to(MulDiv(y-l.b[Scroll].y-a.drag,range(),h));}
void act(int id){switch(id){
    case Import:import_files();break;case Export:export_rows();break;case Search:SetFocus(a.edit);break;
    case Mode:a.mode=(a.mode+1)%4;filter_rows();break;case All:case None:std::fill(std::begin(a.enabled),std::end(a.enabled),id==All);break;
    case Run:if(a.rows.empty())notify(L"Import or load a dataset first");else if(!active())notify(L"Select a service first");else start_run();break;
    case First:change_page(0);break;case Prev:change_page(a.page-1);break;case Next:change_page(a.page+1);break;case Last:change_page(pages()-1);break;
    case Size:a.pageSize=a.pageSize==100?250:a.pageSize==250?500:a.pageSize==500?1000:100;change_page(0);break;
    case EmptyAction:if(a.rows.empty())import_files();else SetWindowTextW(a.edit,L"");break;
    case Minimize:ShowWindow(a.win,SW_MINIMIZE);break;case Maximize:ShowWindow(a.win,IsZoomed(a.win)?SW_RESTORE:SW_MAXIMIZE);break;case Close:PostMessageW(a.win,WM_CLOSE,0,0);break;
    default:if(id>=Service&&id<Service+guimod::kModuleCount)a.enabled[id-Service]=!a.enabled[id-Service];break;
    }refresh();}
LRESULT CALLBACK proc(HWND win,UINT m,WPARAM wp,LPARAM lp){int x=logical(GET_X_LPARAM(lp)),y=logical(GET_Y_LPARAM(lp));switch(m){
    case WM_CREATE:{a.win=win;a.kit.make(std::max(96u,GetDpiForWindow(win)));std::fill(std::begin(a.enabled),std::end(a.enabled),true);
        a.edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,1,1,win,(HMENU)101,nullptr,nullptr);SendMessageW(a.edit,WM_SETFONT,(WPARAM)a.kit.edit_font,TRUE);
        SendMessageW(a.edit,EM_SETCUEBANNER,FALSE,(LPARAM)L"Search records…");a.editbrush=CreateSolidBrush(gd(col::panel2).ToCOLORREF());create_list();layout();filter_rows();
        {wchar_t ap[1024]{};DWORD an=GetEnvironmentVariableW(L"ULTRIA_UI_IMPORT",ap,1024);if(an&&an<1024)start_import({util::narrow(std::wstring(ap,an))});}
        return 0;}
    case WM_NCCALCSIZE:return 0;
    case WM_NCHITTEST:{POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(win,&pt);x=logical(pt.x);y=logical(pt.y);
        if(!IsZoomed(win)){bool left=x<5,right=x>=l.w-5,t=y<5,b=y>=l.h-5;if(t&&left)return HTTOPLEFT;if(t&&right)return HTTOPRIGHT;if(b&&left)return HTBOTTOMLEFT;if(b&&right)return HTBOTTOMRIGHT;if(t)return HTTOP;if(b)return HTBOTTOM;if(left)return HTLEFT;if(right)return HTRIGHT;}
        return y<48&&x<l.w-126?HTCAPTION:HTCLIENT;}
    case WM_GETMINMAXINFO:{auto p=(MINMAXINFO*)lp;p->ptMinTrackSize={d(1000),d(670)};MONITORINFO mi{sizeof(mi)};if(GetMonitorInfoW(MonitorFromWindow(win,MONITOR_DEFAULTTONEAREST),&mi)){p->ptMaxPosition={mi.rcWork.left-mi.rcMonitor.left,mi.rcWork.top-mi.rcMonitor.top};p->ptMaxSize={mi.rcWork.right-mi.rcWork.left,mi.rcWork.bottom-mi.rcWork.top};}return 0;}
    case WM_DPICHANGED:{a.kit.make(LOWORD(wp));SendMessageW(a.edit,WM_SETFONT,(WPARAM)a.kit.edit_font,TRUE);table_dpi();auto r=(RECT*)lp;SetWindowPos(win,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER);layout();refresh();return 0;}
    case WM_SIZE:if(wp!=SIZE_MINIMIZED){layout();refresh();}return 0;
    case WM_ERASEBKGND:return 1;case WM_PAINT:paint();return 0;
    case WM_CTLCOLOREDIT:SetTextColor((HDC)wp,gd(col::text).ToCOLORREF());SetBkColor((HDC)wp,gd(col::panel2).ToCOLORREF());return (LRESULT)a.editbrush;
    case WM_COMMAND:if((HWND)lp==a.edit){if(HIWORD(wp)==EN_CHANGE){int n=GetWindowTextLengthW(a.edit);std::wstring s(n+1,0);GetWindowTextW(a.edit,s.data(),n+1);s.resize(n);a.query=narrow(s);filter_rows();}else refresh();}return 0;
    case WM_NOTIFY:return list_notify((NMHDR*)lp);
    case WM_MOUSEMOVE:{TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,win,0};TrackMouseEvent(&t);if(a.down==Scroll){drag_to(y);return 0;}int id=target(x,y);if(id!=a.hot){a.hot=id;refresh();}return 0;}
    case WM_MOUSELEAVE:a.hot=Nil;refresh();return 0;
    case WM_SETCURSOR:if(LOWORD(lp)==HTCLIENT){POINT p;GetCursorPos(&p);ScreenToClient(win,&p);if(target(logical(p.x),logical(p.y))){SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(32649)));return TRUE;}}break;
    case WM_LBUTTONDOWN:{int id=target(x,y);SetFocus(id==Search?a.edit:win);a.hot=a.down=id;if(id){SetCapture(win);if(id==Scroll){Box b=thumb();a.drag=y>=b.y&&y<b.bottom()?y-b.y:b.h/2;drag_to(y);}}refresh();return 0;}
    case WM_LBUTTONUP:{int id=a.down;a.down=Nil;if(GetCapture()==win)ReleaseCapture();if(id&&id!=Scroll&&id==target(x,y))act(id);refresh();return 0;}
    case WM_CAPTURECHANGED:a.down=Nil;refresh();return 0;
    case WM_MOUSEWHEEL:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(win,&p);if(l.scope.has(logical(p.x),logical(p.y))){a.hot=Nil;scroll_to(a.scroll-MulDiv(GET_WHEEL_DELTA_WPARAM(wp),72,WHEEL_DELTA));}else if(!a.view.empty()&&l.ledger.has(logical(p.x),logical(p.y)))ListView_Scroll(a.list,0,-MulDiv(GET_WHEEL_DELTA_WPARAM(wp),d(96),WHEEL_DELTA));return 0;}
    case WM_APP+10:{auto* job=reinterpret_cast<data_detail::Job*>(lp);KillTimer(win,3);a.importing.store(false);EnableWindow(a.edit,TRUE);filter_rows();
        if(job){unsigned long long dt=GetTickCount64()-job->t0;std::wstring msg=fmt_count(job->stats.added)+L" unique imported";
            if(job->stats.duplicates)msg+=L" · "+fmt_count(job->stats.duplicates)+L" duplicate"+(job->stats.duplicates==1?L"":L"s");
            if(job->stats.filtered)msg+=L" · "+fmt_count(job->stats.filtered)+L" filtered (non-email)";
            if(job->stats.skipped)msg+=L" · "+fmt_count(job->stats.skipped)+L" invalid";
            if(job->stats.failed)msg+=L" · "+fmt_count(job->stats.failed)+L" unreadable";
            msg+=L" · "+std::to_wstring(dt)+L" ms";delete job;notify(std::move(msg));}
        return 0;}
    case WM_TIMER:if(wp==1)tick_run();if(wp==2){KillTimer(win,2);a.toast.clear();refresh();}if(wp==3){if(!a.importing.load())KillTimer(win,3);refresh();}return 0;
    case WM_DESTROY:a.import_cancel.store(true);KillTimer(win,1);KillTimer(win,2);KillTimer(win,3);PostQuitMessage(0);return 0;
    }return DefWindowProcW(win,m,wp,lp);}

int run(HINSTANCE instance,bool demo_mode){(void)demo_mode;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);ULONG_PTR token;gdip::GdiplusStartupInput input;if(gdip::GdiplusStartup(&token,&input,nullptr)!=gdip::Ok)return 1;
    INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&ic);WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=instance;wc.lpfnWndProc=proc;wc.lpszClassName=L"UltriaAuditWorkspace";wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));wc.hIcon=wc.hIconSm=LoadIconW(instance,MAKEINTRESOURCEW(1));RegisterClassExW(&wc);
    RECT r{};SystemParametersInfoW(SPI_GETWORKAREA,0,&r,0);a.kit.dpi=GetDpiForSystem();int w=std::min(d(1400),int(r.right-r.left)),h=std::min(d(860),int(r.bottom-r.top));
    HWND win=CreateWindowExW(WS_EX_APPWINDOW,wc.lpszClassName,L"Ultria Checker — Exposure review",WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_CLIPCHILDREN,r.left+(r.right-r.left-w)/2,r.top+(r.bottom-r.top-h)/2,w,h,nullptr,nullptr,instance,nullptr);
    if(win){BOOL dark=TRUE;DwmSetWindowAttribute(win,20,&dark,sizeof(dark));ShowWindow(win,SW_SHOW);UpdateWindow(win);MSG msg{};
        while(GetMessageW(&msg,nullptr,0,0)>0){if(msg.message==WM_KEYDOWN&&(GetKeyState(VK_CONTROL)&0x8000)){if(msg.wParam=='F'){SetFocus(a.edit);SendMessageW(a.edit,EM_SETSEL,0,-1);continue;}if(msg.wParam=='O'&&!a.running&&!a.importing.load()){import_files();continue;}if(msg.wParam=='S'){export_rows();continue;}}TranslateMessage(&msg);DispatchMessageW(&msg);}}
    DeleteObject(a.editbrush);a.kit.clear();gdip::GdiplusShutdown(token);return win?0:1;}
} // namespace gui
