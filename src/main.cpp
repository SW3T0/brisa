#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <psapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "fpdfview.h"
#include "fpdf_progressive.h"
#include "fpdf_text.h"
#include "fpdf_annot.h"
#include "fpdf_edit.h"
#include "fpdf_save.h"
#include <deque>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <limits>

// PDFium is owned exclusively by the worker. UI never calls its API.
constexpr UINT READY = WM_APP + 1;
constexpr UINT EXPORT_READY = WM_APP + 2;
constexpr double PIXEL_LIMIT = 12000000; // 48 MB per page raster.
struct Glyph { float l,t,r,b; unsigned code; bool vertical=false; };
struct Image {
    int page=0,count=0,w=0,h=0; double scale=1;
    std::vector<unsigned char> pixels;
    std::vector<Glyph> glyphs;
    std::wstring error;
};
struct Item { std::wstring path; int page=0,width=1,height=1; double zoom=1; bool fit=true, render=true; };
struct Request { LONG generation=0; int panes=0; Item items[2]; };
struct Result { LONG generation; Image* images[2]{}; };
struct Engine {
    HANDLE file = INVALID_HANDLE_VALUE;
    FPDF_DOCUMENT doc = nullptr;
    FPDF_FILEACCESS access{};
    wchar_t path[32768]{};
    void close() {
        if (doc) FPDF_CloseDocument(doc);
        doc = nullptr;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        file = INVALID_HANDLE_VALUE; path[0] = 0;
    }
    static int read(void* p, unsigned long pos, unsigned char* data, unsigned long size) {
        LARGE_INTEGER at; at.QuadPart = pos; DWORD got = 0;
        return SetFilePointerEx((HANDLE)p, at, nullptr, FILE_BEGIN) &&
            ReadFile((HANDLE)p, data, size, &got, nullptr) && got == size;
    }
    bool open(const wchar_t* p, wchar_t* error) {
        close();
        file = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER length{};
        if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &length)) {
            wcscpy(error, L"No se puede abrir este archivo."); close(); return false;
        }
        if (length.QuadPart <= 0 || length.QuadPart > 0xffffffffLL) {
            wcscpy(error, L"Archivo vacío o mayor de 4 GB."); close(); return false;
        }
        access = {(unsigned long)length.QuadPart, read, file};
        doc = FPDF_LoadCustomDocument(&access, nullptr);
        if (!doc) {
            wcscpy(error, FPDF_GetLastError() == FPDF_ERR_PASSWORD ?
                L"Este PDF requiere contraseña. El MVP no admite contraseñas." : L"No se puede leer el PDF. Puede estar dañado.");
            close(); return false;
        }
        wcscpy(path, p); return true;
    }
};

// UI owns workspaces and annotations; worker owns all PDFium handles.
struct InkPoint { int page,start,tool; float x,y; };
struct Highlight { int page,group; float l,t,r,b; };
struct InkHeader { char magic[8]; unsigned count; DWORD sizeLow,sizeHigh; FILETIME modified; };
struct Document {
    std::wstring path;
    std::vector<InkPoint> ink;
    std::vector<Highlight> highlights;
    bool dirty=false;
    int nextGroup=1,lastTool=1;
};
struct ExportJob { Document document; std::wstring destination; };
struct ExportResult { std::wstring destination,error; };
std::deque<ExportJob> exportQueue;
bool exportBusy=false;
struct Pane {
    int doc=-1,page=0,count=0,panX=0,panY=0;
    double zoom=1; bool fit=true,busy=false;
    std::unique_ptr<Image> image;
};
struct Workspace { Pane panes[2]; int count=1,active=0; };
HWND window;
HMENU menus,workspaceMenu;
HFONT uiFont,headingFont,smallFont;
HCURSOR penCursor;
ULONG_PTR gdiplusToken;
HDC canvas=nullptr;
HBITMAP canvasBitmap=nullptr,originalBitmap=nullptr;
int canvasWidth=0,canvasHeight=0;
double dpiScale=1;
bool menuVisible=false,sidebarVisible=false,fullscreen=false;
WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
std::vector<Document> documents;
std::vector<std::unique_ptr<Workspace>> workspaces;
int activeWorkspace=-1,tool=0,tabOffset=0;
bool dragging=false,drawing=false,selecting=false;
int dragX=0,dragY=0,interactionPane=-1,selectionStart=-1,selectionEnd=-1;
std::wstring toast;
CRITICAL_SECTION mutex;
HANDLE wakeEvent,worker;
Request pending;
volatile LONG generation=0,stopping=0;
constexpr unsigned POINT_LIMIT=200000,HIGHLIGHT_LIMIT=100000,WORKSPACE_MENU=1000;
enum { OPEN=100,EXIT,FIT,PLUS,MINUS,FULL,HAND,MARKER,PEN,UNDO,SAVE,SIDEBAR,SPLIT };
constexpr unsigned EXPORT_PDF=113;
int px(int v) { return (int)round(v*dpiScale); }
Workspace* workspace() { return activeWorkspace>=0?workspaces[activeWorkspace].get():nullptr; }
Pane* focusedPane() { auto* w=workspace(); return w?&w->panes[w->active]:nullptr; }
const wchar_t* filename(int doc) {
    if(doc<0) return L"";
    const wchar_t* path=documents[doc].path.c_str(); const wchar_t* n=wcsrchr(path,L'\\'); return n?n+1:path;
}
void repaint() { InvalidateRect(window,nullptr,FALSE); }
void notify(const wchar_t* text) { toast=text; SetTimer(window,2,3600,nullptr); repaint(); }
RECT client() { RECT r{}; GetClientRect(window,&r); return r; }
RECT paneArea(int n) {
    RECT r=client(); r.top=px(44); if(sidebarVisible) r.left=px(58);
    auto* w=workspace();
    if(w && w->count==2) { int middle=r.left+(r.right-r.left)/2; if(n==0) r.right=middle-px(6); else r.left=middle+px(6); }
    return r;
}
RECT contentArea(int n) { RECT r=paneArea(n); r.top+=px(35); r.bottom-=px(12); return r; }
void clampPan(Pane& p,RECT area) {
    if(!p.image) return;
    int mx=std::max<int>(0,(p.image->w-(area.right-area.left)+px(32))/2);
    int my=std::max<int>(0,(p.image->h-(area.bottom-area.top)+px(32))/2);
    p.panX=std::clamp(p.panX,-mx,mx); p.panY=std::clamp(p.panY,-my,my);
}
RECT pageRect(int n) {
    RECT r=contentArea(n); auto* w=workspace(); if(!w || !w->panes[n].image) return {};
    Pane& p=w->panes[n]; clampPan(p,r);
    int x=r.left+(r.right-r.left-p.image->w)/2+p.panX;
    int y=r.top+(r.bottom-r.top-p.image->h)/2+p.panY;
    return {x,y,x+p.image->w,y+p.image->h};
}
int paneAt(int x,int y) {
    auto* w=workspace(); if(!w) return -1;
    for(int n=0;n<w->count;n++) { RECT r=paneArea(n); if(PtInRect(&r,{x,y})) return n; } return -1;
}
void fill(HDC dc,RECT r,COLORREF c) { HBRUSH b=CreateSolidBrush(c); FillRect(dc,&r,b); DeleteObject(b); }
void label(HDC dc,const wchar_t* s,RECT r,COLORREF c,UINT flags=DT_SINGLELINE|DT_VCENTER|DT_CENTER) {
    SetTextColor(dc,c); DrawTextW(dc,s,-1,&r,flags|DT_NOPREFIX);
}
void updateTitle() {
    auto* w=workspace(); std::wstring s=L"Brisa";
    if(w) {
        s+=L" — "; s+=filename(w->panes[0].doc);
        if(w->count==2) { s+=L" + "; s+=filename(w->panes[1].doc); }
        s+=L" · "+std::to_wstring(activeWorkspace+1)+L"/"+std::to_wstring(workspaces.size());
        Pane& p=w->panes[w->active]; s+=L" · "+std::to_wstring(p.page+1)+L"/"+std::to_wstring(p.count);
    }
    SetWindowTextW(window,s.c_str());
}
bool saveDocument(int doc) {
    Document& d=documents[doc]; if(!d.dirty) return true;
    std::wstring path=d.path+L".brisa-ink",temp=path+L".tmp";
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if(!GetFileAttributesExW(d.path.c_str(),GetFileExInfoStandard,&attr)) return false;
    InkHeader h{{'B','R','I','S','A','0','2',0},(unsigned)d.ink.size(),attr.nFileSizeLow,attr.nFileSizeHigh,attr.ftLastWriteTime};
    HANDLE f=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return false;
    auto write=[&](const void* p,DWORD size) { DWORD done=0; return WriteFile(f,p,size,&done,nullptr)&&done==size; };
    unsigned count=(unsigned)d.highlights.size();
    bool ok=write(&h,sizeof(h))&&write(d.ink.data(),h.count*sizeof(InkPoint))&&write(&count,sizeof(count))&&write(d.highlights.data(),count*sizeof(Highlight))&&FlushFileBuffers(f);
    CloseHandle(f); ok=ok&&MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if(ok) d.dirty=false; else DeleteFileW(temp.c_str()); return ok;
}
void loadDocument(Document& d) {
    HANDLE f=CreateFileW((d.path+L".brisa-ink").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return;
    auto read=[&](void* p,DWORD size) { DWORD got=0; return ReadFile(f,p,size,&got,nullptr)&&got==size; };
    InkHeader h{}; WIN32_FILE_ATTRIBUTE_DATA a{};
    bool ok=read(&h,sizeof(h))&&(!memcmp(h.magic,"BRISA01",8)||!memcmp(h.magic,"BRISA02",8))&&h.count<=POINT_LIMIT;
    ok=ok&&GetFileAttributesExW(d.path.c_str(),GetFileExInfoStandard,&a)&&h.sizeLow==a.nFileSizeLow&&h.sizeHigh==a.nFileSizeHigh&&CompareFileTime(&h.modified,&a.ftLastWriteTime)==0;
    if(ok) {
        d.ink.resize(h.count); ok=read(d.ink.data(),h.count*sizeof(InkPoint));
        for(auto& p:d.ink) ok=ok&&p.page>=0&&(p.start==0||p.start==1)&&(p.tool==1||p.tool==2)&&isfinite(p.x)&&isfinite(p.y)&&p.x>=0&&p.x<=1&&p.y>=0&&p.y<=1;
        if(ok&&!memcmp(h.magic,"BRISA02",8)) {
            unsigned count=0; ok=read(&count,sizeof(count))&&count<=HIGHLIGHT_LIMIT;
            if(ok) { d.highlights.resize(count); ok=read(d.highlights.data(),count*sizeof(Highlight)); }
            for(auto& r:d.highlights) {
                ok=ok&&r.page>=0&&r.group>0&&r.group<100000000&&isfinite(r.l)&&isfinite(r.t)&&isfinite(r.r)&&isfinite(r.b)&&r.l>=0&&r.t>=0&&r.r<=1&&r.b<=1&&r.l<r.r&&r.t<r.b;
                d.nextGroup=std::max(d.nextGroup,r.group+1);
            }
        }
    }
    CloseHandle(f); if(!ok) { d.ink.clear(); d.highlights.clear(); d.nextGroup=1; }
    if(d.highlights.empty()&&!d.ink.empty()) d.lastTool=2;
}
bool saveAll(bool closing=false) {
    bool ok=true; for(int n=0;n<(int)documents.size();n++) if(!saveDocument(n)) ok=false;
    if(!ok && closing) return MessageBoxW(window,L"No se pudieron guardar algunas marcas. ¿Salir y descartarlas?",L"Brisa",MB_YESNO|MB_ICONWARNING)==IDYES;
    if(!ok) notify(L"No se pudieron guardar las marcas. Comprueba los permisos de la carpeta."); return ok;
}
bool stale(LONG g) { return InterlockedCompareExchange(&stopping,0,0)||InterlockedCompareExchange(&generation,0,0)!=g; }
int pauseRender(IFSDK_PAUSE* p) { return stale(*(LONG*)p->user); }
Image* render(Engine& e,const Item& r,LONG g,bool cancellable=true) {
    auto out=std::make_unique<Image>(); wchar_t error[160]{};
    if((!e.doc||wcscmp(e.path,r.path.c_str()))&&!e.open(r.path.c_str(),error)) { out->error=error; return out.release(); }
    out->count=FPDF_GetPageCount(e.doc);
    if(out->count<1) { out->error=L"El PDF no contiene páginas."; return out.release(); }
    out->page=std::clamp(r.page,0,out->count-1);
    FPDF_PAGE p=FPDF_LoadPage(e.doc,out->page);
    if(!p) { out->error=L"No se puede cargar esta página."; return out.release(); }
    double w=FPDF_GetPageWidth(p),h=FPDF_GetPageHeight(p);
    if(!isfinite(w)||!isfinite(h)||w<=0||h<=0) { out->error=L"Tamaño de página inválido."; FPDF_ClosePage(p); return out.release(); }
    double scale=r.fit?std::min(r.width/w,r.height/h):r.zoom;
    scale=std::min(std::max(.02,scale),std::min(sqrt(PIXEL_LIMIT/(w*h)),std::min(16000/w,16000/h)));
    out->w=std::max<int>(1,(int)floor(w*scale)); out->h=std::max<int>(1,(int)floor(h*scale)); out->scale=scale;
    out->pixels.resize((size_t)out->w*out->h*4);
    FPDF_BITMAP bitmap=FPDFBitmap_CreateEx(out->w,out->h,FPDFBitmap_BGRx,out->pixels.data(),out->w*4);
    if(!bitmap) out->error=L"No hay memoria suficiente para renderizar.";
    else {
        FPDFBitmap_FillRect(bitmap,0,0,out->w,out->h,0xffffffff);
        if(cancellable) {
            IFSDK_PAUSE pause{1,pauseRender,&g};
            int status=FPDF_RenderPageBitmap_Start(bitmap,p,0,0,out->w,out->h,0,FPDF_ANNOT,&pause);
            while(status==FPDF_RENDER_TOBECONTINUED&&!stale(g)) status=FPDF_RenderPage_Continue(p,&pause);
            FPDF_RenderPage_Close(p); if(status==FPDF_RENDER_FAILED) out->error=L"No se puede renderizar esta página.";
        } else FPDF_RenderPageBitmap(bitmap,p,0,0,out->w,out->h,0,FPDF_ANNOT);
        FPDFBitmap_Destroy(bitmap);
    }
    if(!cancellable||!stale(g)) {
        FPDF_TEXTPAGE text=FPDFText_LoadPage(p);
        if(text) {
            int count=std::clamp(FPDFText_CountChars(text),0,200000); out->glyphs.resize(count);
            for(int n=0;n<count;n++) {
                if(cancellable&&n%256==0&&stale(g)) break;
                auto& glyph=out->glyphs[n]; glyph.code=FPDFText_GetUnicode(text,n);
                double l,r,b,t;
                if(glyph.code<32||!FPDFText_GetCharBox(text,n,&l,&r,&b,&t)||r<=l||t<=b) continue;
                int xs[4],ys[4]; double xx[4]={l,r,r,l},yy[4]={t,t,b,b};
                for(int k=0;k<4;k++) FPDF_PageToDevice(p,0,0,out->w,out->h,0,xx[k],yy[k],&xs[k],&ys[k]);
                bool pageVertical=abs(ys[1]-ys[0])>abs(xs[1]-xs[0]);
                glyph.vertical=pageVertical!=(fabs(cos(FPDFText_GetCharAngle(text,n)))<.5);
                glyph.l=std::clamp(*std::min_element(xs,xs+4)/(float)out->w,0.f,1.f);
                glyph.r=std::clamp(*std::max_element(xs,xs+4)/(float)out->w,0.f,1.f);
                glyph.t=std::clamp(*std::min_element(ys,ys+4)/(float)out->h,0.f,1.f);
                glyph.b=std::clamp(*std::max_element(ys,ys+4)/(float)out->h,0.f,1.f);
            }
            FPDFText_ClosePage(text);
        }
    }
    FPDF_ClosePage(p); return out.release();
}
void dispose(Result* result) { if(result) { delete result->images[0]; delete result->images[1]; delete result; } }
// Export opens its own document on the same worker thread. Rendering caches and
// the original file stay untouched, even if export is cancelled or fails.
bool sameFile(const std::wstring& first,const std::wstring& second) {
    if(!_wcsicmp(first.c_str(),second.c_str())) return true;
    HANDLE a=CreateFileW(first.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE b=CreateFileW(second.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    BY_HANDLE_FILE_INFORMATION x{},y{};
    bool same=a!=INVALID_HANDLE_VALUE&&b!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(a,&x)&&GetFileInformationByHandle(b,&y)&&
        x.dwVolumeSerialNumber==y.dwVolumeSerialNumber&&x.nFileIndexHigh==y.nFileIndexHigh&&x.nFileIndexLow==y.nFileIndexLow;
    if(a!=INVALID_HANDLE_VALUE) CloseHandle(a); if(b!=INVALID_HANDLE_VALUE) CloseHandle(b); return same;
}
FS_POINTF pdfPoint(FPDF_PAGE page,float x,float y) {
    double a=0,b=0;
    FPDF_DeviceToPage(page,0,0,1000000,1000000,0,(int)round(x*1000000),(int)round(y*1000000),&a,&b);
    return {(float)a,(float)b};
}
FS_RECTF bounds(const std::vector<FS_POINTF>& points,float padding=0) {
    FS_RECTF r{points[0].x,points[0].y,points[0].x,points[0].y};
    for(auto p:points) { r.left=std::min(r.left,p.x); r.right=std::max(r.right,p.x); r.top=std::max(r.top,p.y); r.bottom=std::min(r.bottom,p.y); }
    r.left-=padding; r.right+=padding; r.top+=padding; r.bottom-=padding; return r;
}
bool embedMarks(FPDF_DOCUMENT pdf,const Document& d) {
    int pages=FPDF_GetPageCount(pdf);
    // Visit only marked pages, rather than loading a large document in full.
    std::vector<int> marked;
    for(auto h:d.highlights) marked.push_back(h.page);
    for(auto p:d.ink) if(p.start) marked.push_back(p.page);
    std::sort(marked.begin(),marked.end()); marked.erase(std::unique(marked.begin(),marked.end()),marked.end());
    for(int number:marked) {
        if(number<0||number>=pages) return false;
        FPDF_PAGE page=FPDF_LoadPage(pdf,number); if(!page) return false;
        bool ok=true;
        for(size_t i=0;i<d.highlights.size()&&ok;) {
            const auto& h=d.highlights[i]; if(h.page!=number) {i++;continue;}
            int group=h.group; FPDF_ANNOTATION annot=FPDFPage_CreateAnnot(page,FPDF_ANNOT_HIGHLIGHT);
            if(!annot) {ok=false;break;}
            std::vector<FS_POINTF> corners;
            ok=FPDFAnnot_SetColor(annot,FPDFANNOT_COLORTYPE_Color,250,201,38,90)&&FPDFAnnot_SetFlags(annot,FPDF_ANNOT_FLAG_PRINT);
            do {
                const auto& r=d.highlights[i];
                auto a=pdfPoint(page,r.l,r.t),b=pdfPoint(page,r.r,r.t),c=pdfPoint(page,r.l,r.b),e=pdfPoint(page,r.r,r.b);
                FS_QUADPOINTSF quad{a.x,a.y,b.x,b.y,c.x,c.y,e.x,e.y};
                ok=ok&&FPDFAnnot_AppendAttachmentPoints(annot,&quad);
                corners.insert(corners.end(),{a,b,c,e}); i++;
            } while(i<d.highlights.size()&&d.highlights[i].page==number&&d.highlights[i].group==group);
            auto rect=bounds(corners); ok=ok&&FPDFAnnot_SetRect(annot,&rect); FPDFPage_CloseAnnot(annot);
        }
        for(size_t i=0;i<d.ink.size()&&ok;) {
            const auto& p=d.ink[i]; size_t end=i+1; while(end<d.ink.size()&&!d.ink[end].start&&d.ink[end].page==p.page) end++;
            if(p.page!=number) {i=end;continue;}
            std::vector<FS_POINTF> points; points.reserve(end-i+1);
            for(size_t j=i;j<end;j++) points.push_back(pdfPoint(page,d.ink[j].x,d.ink[j].y));
            if(points.size()==1) points.push_back({points[0].x+.01f,points[0].y});
            float width=p.tool==1?12.f:1.6f; unsigned alpha=p.tool==1?68:230;
            unsigned red=p.tool==1?247:48,green=p.tool==1?201:74,blue=p.tool==1?49:61;
            FPDF_ANNOTATION annot=FPDFPage_CreateAnnot(page,FPDF_ANNOT_INK);
            if(!annot) {ok=false;break;}
            auto rect=bounds(points,width);
            ok=FPDFAnnot_SetRect(annot,&rect)&&FPDFAnnot_SetBorder(annot,0,0,width)&&
                FPDFAnnot_SetColor(annot,FPDFANNOT_COLORTYPE_Color,red,green,blue,alpha)&&FPDFAnnot_SetFlags(annot,FPDF_ANNOT_FLAG_PRINT)&&
                FPDFAnnot_AddInkStroke(annot,points.data(),points.size())>=0;
            // Explicit appearance path also renders in readers that do not
            // synthesize an InkList appearance on opening the file.
            FPDF_PAGEOBJECT path=FPDFPageObj_CreateNewPath(points[0].x,points[0].y);
            if(!path) ok=false;
            else {
                bool appearance=FPDFPageObj_SetStrokeColor(path,red,green,blue,alpha)&&FPDFPageObj_SetStrokeWidth(path,width)&&FPDFPath_SetDrawMode(path,FPDF_FILLMODE_NONE,1);
                for(size_t j=1;j<points.size();j++) appearance=appearance&&FPDFPath_LineTo(path,points[j].x,points[j].y);
                if(!ok||!appearance||!FPDFAnnot_AppendObject(annot,path)) {FPDFPageObj_Destroy(path);ok=false;}
            }
            FPDFPage_CloseAnnot(annot); i=end;
        }
        if(ok) {
            // PDFium's annotation render pass promotes new dictionaries to
            // indirect objects and generates missing appearances. A 1px
            // target prepares interoperable annotations without a page image.
            FPDF_BITMAP bitmap=FPDFBitmap_Create(1,1,0);
            if(!bitmap) ok=false;
            else { FPDF_RenderPageBitmap(bitmap,page,0,0,1,1,0,FPDF_ANNOT); FPDFBitmap_Destroy(bitmap); }
        }
        FPDF_ClosePage(page); if(!ok) return false;
    }
    return true;
}
struct PdfWriter {
    FPDF_FILEWRITE api{1,write}; HANDLE file=INVALID_HANDLE_VALUE;
    static int write(FPDF_FILEWRITE* self,const void* data,unsigned long size) {
        auto* writer=reinterpret_cast<PdfWriter*>(self); DWORD done=0;
        return WriteFile(writer->file,data,size,&done,nullptr)&&done==size;
    }
};
ExportResult* exportPdf(const ExportJob& job) {
    auto result=std::make_unique<ExportResult>(); result->destination=job.destination;
    if(sameFile(job.document.path,job.destination)) {result->error=L"Elige otro archivo para conservar el PDF original.";return result.release();}
    Engine engine; wchar_t error[160]{};
    if(!engine.open(job.document.path.c_str(),error)) {result->error=error;return result.release();}
    if(!embedMarks(engine.doc,job.document)) {result->error=L"No se pudieron incorporar las anotaciones al PDF.";engine.close();return result.release();}
    // Create a unique temporary file in the destination directory, then rename
    // it atomically: a failed export never truncates an existing destination.
    std::wstring temporary; HANDLE file=INVALID_HANDLE_VALUE;
    for(int n=0;n<20&&file==INVALID_HANDLE_VALUE;n++) {
        temporary=job.destination+L".brisa-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(n)+L".tmp";
        file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    }
    if(file==INVALID_HANDLE_VALUE) result->error=L"No se puede escribir en la carpeta de destino.";
    else {
        PdfWriter writer; writer.file=file;
        bool ok=FPDF_SaveAsCopy(engine.doc,&writer.api,FPDF_NO_INCREMENTAL)&&FlushFileBuffers(file);
        CloseHandle(file); engine.close();
        if(ok) ok=MoveFileExW(temporary.c_str(),job.destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
        if(!ok) {DeleteFileW(temporary.c_str());result->error=L"No se pudo guardar el PDF. Comprueba los permisos o si está abierto en otro programa.";}
    }
    engine.close(); return result.release();
}
DWORD WINAPI workerMain(void*) {
    FPDF_InitLibrary(); Engine engines[2];
    while(true) {
        WaitForSingleObject(wakeEvent,INFINITE); if(InterlockedCompareExchange(&stopping,0,0)) break;
        EnterCriticalSection(&mutex); Request r=pending;
        std::unique_ptr<ExportJob> job;
        if(!exportQueue.empty()) {job=std::make_unique<ExportJob>(std::move(exportQueue.front()));exportQueue.pop_front();}
        LeaveCriticalSection(&mutex);
        if(job) {
            auto* outcome=exportPdf(*job);
            if(!PostMessageW(window,EXPORT_READY,0,(LPARAM)outcome)) delete outcome;
        }
        auto result=new Result{}; result->generation=r.generation;
        for(int n=0;n<r.panes&&!stale(r.generation);n++) if(r.items[n].render) result->images[n]=render(engines[n],r.items[n],r.generation);
        if(r.panes<1) engines[0].close();
        if(r.panes<2) engines[1].close();
        if(stale(r.generation)||!PostMessageW(window,READY,0,(LPARAM)result)) dispose(result);
    }
    engines[0].close(); engines[1].close(); FPDF_DestroyLibrary(); return 0;
}
void requestRender(int only=-1) {
    auto* w=workspace();
    Request r; r.generation=InterlockedIncrement(&generation); r.panes=w?w->count:0;
    for(int n=0;n<r.panes;n++) {
        Pane& p=w->panes[n]; RECT area=contentArea(n); Item& item=r.items[n];
        item.path=documents[p.doc].path; item.page=p.page; item.fit=p.fit; item.zoom=p.zoom;
        item.width=std::max<int>(1,area.right-area.left-px(32)); item.height=std::max<int>(1,area.bottom-area.top-px(32));
        item.render=only<0||only==n||p.busy||!p.image;
        if(item.render) p.busy=true;
    }
    EnterCriticalSection(&mutex); pending=std::move(r); LeaveCriticalSection(&mutex);
    SetEvent(wakeEvent); repaint();
}
void finishInteraction(bool commit=true);
std::wstring workspaceName(int index) {
    auto& w=*workspaces[index]; std::wstring s=filename(w.panes[0].doc);
    if(w.count==2) { s+=L" + "; s+=filename(w.panes[1].doc); } return s;
}
int tabWidth() { int available=client().right-px(124); return std::clamp(available/std::max<int>(1,(int)workspaces.size()),px(142),px(240)); }
int visibleTabs() { return std::max<int>(1,(client().right-px(124))/tabWidth()); }
RECT tabRect(int slot) { int x=px(44)+(slot-tabOffset)*tabWidth(); return {x,px(5),x+tabWidth()-px(4),px(40)}; }
void ensureVisible() {
    if(activeWorkspace<tabOffset) tabOffset=std::max<int>(0,activeWorkspace);
    if(activeWorkspace>=tabOffset+visibleTabs()) tabOffset=activeWorkspace-visibleTabs()+1;
    tabOffset=std::clamp(tabOffset,0,std::max<int>(0,(int)workspaces.size()-visibleTabs()));
}
void refreshWorkspaceMenu() {
    while(GetMenuItemCount(workspaceMenu)>0) DeleteMenu(workspaceMenu,0,MF_BYPOSITION);
    for(int n=0;n<(int)workspaces.size();n++) {
        std::wstring name=workspaceName(n),escaped;
        for(wchar_t c:name) { escaped+=c; if(c==L'&') escaped+=L'&'; }
        if(n<9) escaped+=L"\tCtrl+"+std::to_wstring(n+1);
        AppendMenuW(workspaceMenu,MF_STRING|(n==activeWorkspace?MF_CHECKED:0),WORKSPACE_MENU+n,escaped.c_str());
    }
}
void switchWorkspace(int slot) {
    if(slot<0||slot>=(int)workspaces.size()||slot==activeWorkspace) return;
    finishInteraction(); saveAll();
    if(auto* old=workspace()) for(int n=0;n<old->count;n++) { old->panes[n].image.reset(); old->panes[n].busy=false; }
    activeWorkspace=slot; ensureVisible(); refreshWorkspaceMenu(); updateTitle(); requestRender();
}
Pane copyState(const Pane& p) {
    Pane next; next.doc=p.doc; next.page=p.page; next.count=p.count; next.panX=p.panX; next.panY=p.panY; next.zoom=p.zoom; next.fit=p.fit; return next;
}
void openPath(const wchar_t* path) {
    wchar_t absolute[32768]; DWORD len=GetFullPathNameW(path,32768,absolute,nullptr); if(!len||len>=32768) return;
    int doc=-1;
    for(int n=0;n<(int)documents.size();n++) if(!_wcsicmp(documents[n].path.c_str(),absolute)) { doc=n; break; }
    if(doc<0) {
        if(documents.size()>=64) { notify(L"Límite de 64 documentos por sesión."); return; }
        Document d; d.path=absolute; loadDocument(d); documents.push_back(std::move(d)); doc=(int)documents.size()-1;
    }
    for(int n=0;n<(int)workspaces.size();n++) if(workspaces[n]->count==1&&workspaces[n]->panes[0].doc==doc) { switchWorkspace(n); return; }
    if(workspaces.size()>=128) { notify(L"Límite de 128 workspaces por sesión."); return; }
    auto w=std::make_unique<Workspace>(); w->panes[0].doc=doc;
    workspaces.push_back(std::move(w)); switchWorkspace((int)workspaces.size()-1);
}
void openDialog() {
    wchar_t paths[32768]{}; OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner=window; ofn.lpstrFilter=L"Documentos PDF\0*.pdf\0Todos los archivos\0*.*\0";
    ofn.lpstrFile=paths; ofn.nMaxFile=32768; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_ALLOWMULTISELECT|OFN_EXPLORER;
    if(!GetOpenFileNameW(&ofn)) return;
    const wchar_t* name=paths+wcslen(paths)+1;
    if(!*name) openPath(paths); else for(;*name;name+=wcslen(name)+1) { std::wstring full=std::wstring(paths)+L"\\"+name; openPath(full.c_str()); }
}
void beginExport(const std::wstring& destination) {
    if(exportBusy) { notify(L"Espera a que termine el guardado del PDF."); return; }
    finishInteraction(); auto* pane=focusedPane(); if(!pane) return;
    for(auto& document:documents) if(sameFile(document.path,destination)) {
        notify(L"Elige una copia nueva para conservar el PDF original."); return;
    }
    ExportJob job{documents[pane->doc],destination};
    EnterCriticalSection(&mutex); exportQueue.push_back(std::move(job)); LeaveCriticalSection(&mutex);
    exportBusy=true; SetEvent(wakeEvent); notify(L"Guardando PDF anotado…");
}
void exportDialog() {
    auto* pane=focusedPane(); if(!pane) return;
    if(exportBusy) { notify(L"Espera a que termine el guardado del PDF."); return; }
    std::wstring name=documents[pane->doc].path;
    auto dot=name.find_last_of(L'.'); if(dot!=std::wstring::npos) name.resize(dot);
    name+=L"-anotado.pdf";
    wchar_t path[32768]{}; wcsncpy(path,name.c_str(),32767);
    OPENFILENAMEW ofn{sizeof(ofn)}; ofn.hwndOwner=window;
    ofn.lpstrFilter=L"Documento PDF\0*.pdf\0"; ofn.lpstrDefExt=L"pdf";
    ofn.lpstrFile=path; ofn.nMaxFile=32768;
    ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(GetSaveFileNameW(&ofn)) beginExport(path);
}
void createPair(int other) {
    auto* current=workspace(); if(!current||other<0||other>=(int)documents.size()) return;
    int first=current->panes[current->active].doc; if(first==other) return;
    for(int n=0;n<(int)workspaces.size();n++) if(workspaces[n]->count==2&&workspaces[n]->panes[0].doc==first&&workspaces[n]->panes[1].doc==other) { switchWorkspace(n); return; }
    if(workspaces.size()>=128) { notify(L"Límite de 128 workspaces por sesión."); return; }
    auto w=std::make_unique<Workspace>(); w->count=2; w->panes[0]=copyState(current->panes[current->active]); w->panes[1].doc=other;
    for(auto& solo:workspaces) if(solo->count==1&&solo->panes[0].doc==other) { w->panes[1]=copyState(solo->panes[0]); break; }
    for(auto& pane:w->panes) { pane.fit=true; pane.panX=pane.panY=0; }
    workspaces.push_back(std::move(w)); switchWorkspace((int)workspaces.size()-1);
}
void pairMenu() {
    if(!workspace()||documents.size()<2) { notify(L"Abre un segundo PDF con Ctrl+O para crear una vista doble."); return; }
    HMENU popup=CreatePopupMenu(); int first=focusedPane()->doc;
    AppendMenuW(popup,MF_STRING|MF_DISABLED,0,L"Mostrar al lado de este PDF"); AppendMenuW(popup,MF_SEPARATOR,0,nullptr);
    for(int n=0;n<(int)documents.size();n++) if(n!=first) {
        std::wstring escaped; for(const wchar_t* p=filename(n);*p;p++) { escaped+=*p; if(*p==L'&') escaped+=*p; }
        AppendMenuW(popup,MF_STRING,3000+n,escaped.c_str());
    }
    POINT pt{client().right-px(78),px(44)}; ClientToScreen(window,&pt);
    UINT choice=TrackPopupMenu(popup,TPM_RETURNCMD|TPM_RIGHTALIGN,pt.x,pt.y,0,window,nullptr);
    DestroyMenu(popup); if(choice>=3000) createPair(choice-3000);
}
void closeWorkspace(int slot) {
    if(slot<0||slot>=(int)workspaces.size()) return;
    finishInteraction(); saveAll();
    bool current=slot==activeWorkspace; workspaces.erase(workspaces.begin()+slot);
    if(current) {
        activeWorkspace=-1; InterlockedIncrement(&generation);
        if(!workspaces.empty()) switchWorkspace(std::min(slot,(int)workspaces.size()-1));
        else { ensureVisible(); updateTitle(); refreshWorkspaceMenu(); requestRender(); repaint(); }
    } else { if(slot<activeWorkspace) activeWorkspace--; ensureVisible(); updateTitle(); refreshWorkspaceMenu(); repaint(); }
}
void setActivePane(int n) { auto* w=workspace(); if(!w||n<0||n>=w->count) return; w->active=n; updateTitle(); repaint(); }
void go(int next,int pane=-1) {
    auto* w=workspace(); if(!w) return; if(pane<0) pane=w->active; Pane& p=w->panes[pane];
    if(p.count<1) return; next=std::clamp(next,0,p.count-1); if(next==p.page) return;
    finishInteraction(); p.page=next; p.panX=p.panY=0; requestRender(pane); updateTitle();
}
void zoomBy(double factor,int pane=-1) {
    auto* w=workspace(); if(!w) return; if(pane<0) pane=w->active; Pane& p=w->panes[pane];
    finishInteraction(); p.zoom=p.fit&&p.image?p.image->scale:p.zoom; p.zoom=std::clamp(p.zoom*factor,.1,8.0);
    p.fit=false; p.panX=p.panY=0; requestRender(pane);
}
void fitPage() { if(auto* p=focusedPane()) { finishInteraction(); p->fit=true; p->panX=p->panY=0; requestRender(workspace()->active); } }
void toggleSidebar() { finishInteraction(); sidebarVisible=!sidebarVisible; requestRender(); repaint(); }
void setTool(int next) { finishInteraction(); tool=next; CheckMenuRadioItem(GetSubMenu(menus,2),HAND,PEN,HAND+tool,MF_BYCOMMAND); SendMessageW(window,WM_SETCURSOR,(WPARAM)window,HTCLIENT); repaint(); }

int glyphAt(int n,int x,int y,bool strict) {
    Pane& p=workspace()->panes[n]; if(!p.image||p.busy||!p.image->error.empty()) return -1;
    RECT rect=pageRect(n); float nx=(x-rect.left)/(float)p.image->w,ny=(y-rect.top)/(float)p.image->h;
    double best=std::numeric_limits<double>::max(); int index=-1;
    for(int j=0;j<(int)p.image->glyphs.size();j++) {
        const Glyph& g=p.image->glyphs[j]; if(g.code<32||g.l>=g.r||g.t>=g.b) continue;
        double dx=std::max({g.l-nx,0.f,nx-g.r})*p.image->w,dy=std::max({g.t-ny,0.f,ny-g.b})*p.image->h;
        double distance=dx*dx+dy*dy;
        if(distance<best) { best=distance; index=j; }
    }
    return strict&&best>px(8)*px(8)?-1:index;
}
std::vector<Highlight> selectionRects(int n) {
    std::vector<Highlight> rectangles; auto* w=workspace(); if(!w||selectionStart<0||selectionEnd<0) return rectangles;
    Pane& p=w->panes[n]; if(!p.image) return rectangles;
    int first=std::min(selectionStart,selectionEnd),last=std::max(selectionStart,selectionEnd);
    bool previousVertical=false;
    for(int j=first;j<=last&&j<(int)p.image->glyphs.size();j++) {
        const Glyph& g=p.image->glyphs[j]; if(g.code<32||g.r<=g.l||g.b<=g.t) continue;
        Highlight r{p.page,0,g.l,g.t,g.r,g.b};
        if(!rectangles.empty()) {
            Highlight& prev=rectangles.back();
            float height=std::max(prev.b-prev.t,g.b-g.t);
            bool sameLine=fabs((prev.t+prev.b-g.t-g.b)/2)<height*.55;
            bool nearby=g.l<=prev.r+height*2&&g.r>=prev.l-height*2;
            if(g.vertical) {
                float width=std::max(prev.r-prev.l,g.r-g.l);
                sameLine=fabs((prev.l+prev.r-g.l-g.r)/2)<width*.55;
                nearby=g.t<=prev.b+width*2&&g.b>=prev.t-width*2;
            }
            if(previousVertical==g.vertical&&sameLine&&nearby) { prev.l=std::min(prev.l,g.l); prev.t=std::min(prev.t,g.t); prev.r=std::max(prev.r,g.r); prev.b=std::max(prev.b,g.b); continue; }
        }
        rectangles.push_back(r); previousVertical=g.vertical;
    }
    return rectangles;
}
bool addInk(int n,int x,int y,bool start) {
    auto* w=workspace(); if(!w) return false; Pane& p=w->panes[n]; if(!p.image||p.busy||p.image->pixels.empty()||!p.image->error.empty()) return false;
    RECT r=pageRect(n); if(!PtInRect(&r,{x,y})) return false;
    Document& d=documents[p.doc]; if(d.ink.size()>=POINT_LIMIT) return false;
    d.ink.push_back({p.page,start?1:0,2,(x-r.left)/(float)p.image->w,(y-r.top)/(float)p.image->h}); d.dirty=true; d.lastTool=2; repaint(); return true;
}
void finishInteraction(bool commit) {
    if(selecting&&commit&&interactionPane>=0&&workspace()) {
        Pane& p=workspace()->panes[interactionPane]; Document& d=documents[p.doc]; auto rectangles=selectionRects(interactionPane);
        if(!rectangles.empty()&&rectangles.size()+d.highlights.size()<=HIGHLIGHT_LIMIT) {
            int group=d.nextGroup++; for(auto& r:rectangles) { r.group=group; d.highlights.push_back(r); } d.dirty=true; d.lastTool=1;
        }
    }
    bool changed=drawing||selecting; dragging=drawing=selecting=false; interactionPane=-1; selectionStart=selectionEnd=-1;
    if(GetCapture()==window) ReleaseCapture();
    if(changed) saveAll(); repaint();
}
void undo() {
    Pane* p=focusedPane(); if(!p) return; Document& d=documents[p->doc]; bool changed=false;
    // Tool-aware undo avoids deleting a highlight when undoing a pen stroke.
    if(tool==1||(tool==0&&d.lastTool==1)) {
        for(int n=(int)d.highlights.size()-1;n>=0;n--) if(d.highlights[n].page==p->page) {
            int group=d.highlights[n].group;
            d.highlights.erase(std::remove_if(d.highlights.begin(),d.highlights.end(),[&](const Highlight& h){return h.page==p->page&&h.group==group;}),d.highlights.end()); changed=true; break;
        }
    } else {
        for(int n=(int)d.ink.size()-1;n>=0;n--) if(d.ink[n].page==p->page&&d.ink[n].start) {
            int end=n+1; while(end<(int)d.ink.size()&&!d.ink[end].start) end++;
            d.ink.erase(d.ink.begin()+n,d.ink.begin()+end); changed=true; break;
        }
    }
    if(changed) { d.dirty=true; saveAll(); repaint(); }
}
void icon(HDC dc,int kind,RECT box,COLORREF color) {
    Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)),(float)(1.45*dpiScale));
    float x=(box.left+box.right)/2.f,y=(box.top+box.bottom)/2.f,s=(float)(8*dpiScale);
    auto line=[&](float a,float b,float c,float d){g.DrawLine(&pen,x+a*s,y+b*s,x+c*s,y+d*s);};
    switch(kind) {
        case 0: g.DrawRectangle(&pen,x-s,y-s,2*s,2*s); line(0,-1,0,1); break;
        case 1: line(-.7,.6,.6,-.7); line(-.4,.8,.8,-.4); line(.6,-.7,.8,-.4); line(-.7,.6,-.4,.8); line(-.9,1,.4,1); break;
        case 2: line(-.7,.7,.6,-.6); line(-.4,.9,.8,-.4); line(.6,-.6,.8,-.4); line(-.7,.7,-.8,1); line(-.8,1,-.4,.9); break;
        case 3: line(.55,-.55,-.1,0); line(-.1,0,.55,.55); break;
        case 4: line(-.55,-.55,.1,0); line(.1,0,-.55,.55); break;
        case 5: g.DrawRectangle(&pen,x-s,y-s,2*s,2*s); line(-.35,-1,-.35,1); break;
        case 6: line(-.7,0,.7,0); line(0,-.7,0,.7); break;
        case 7: line(-.4,-.4,.4,.4); line(-.4,.4,.4,-.4); break;
        case 8: line(-.6,-.3,-.6,.3); line(-.6,-.3,.2,-.3); line(-.6,-.3,-.15,-.7); g.DrawArc(&pen,x-.6f*s,y-.3f*s,1.5f*s,1.2f*s,270,170); break;
        case 9: line(-.6,-.9,-.6,.7); line(-.6,-.9,.5,.25); line(-.6,.7,-.1,.1); line(-.1,.1,.5,.25); break;
    }
}
void rounded(HDC dc,RECT r,COLORREF c,int radius=7) {
    HBRUSH b=CreateSolidBrush(c); HGDIOBJ old=SelectObject(dc,b),pen=SelectObject(dc,GetStockObject(NULL_PEN));
    RoundRect(dc,r.left,r.top,r.right,r.bottom,px(radius),px(radius)); SelectObject(dc,old); SelectObject(dc,pen); DeleteObject(b);
}
RECT headerButton(int pane,bool next) {
    RECT r=paneArea(pane); int right=r.right-px(next?7:37); return {right-px(28),r.top+px(3),right,r.top+px(31)};
}
void paintPane(HDC dc,int n) {
    auto& w=*workspace(); Pane& p=w.panes[n]; RECT area=paneArea(n),content=contentArea(n);
    RECT header{area.left+px(14),area.top,area.right-px(7),area.top+px(35)};
    if(w.count==2&&w.active==n) { RECT active{area.left+px(14),area.top+px(33),area.right-px(14),area.top+px(35)}; fill(dc,active,RGB(174,192,166)); }
    RECT name=header; name.right-=px(165); SelectObject(dc,smallFont);
    label(dc,filename(p.doc),name,RGB(85,97,84),DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
    wchar_t count[64]; swprintf(count,64,L"%d / %d",p.page+1,p.count);
    RECT number=header; number.left=header.right-px(165); number.right=header.right-px(69);
    label(dc,p.count?count:L"…",number,RGB(112,122,108));
    icon(dc,3,headerButton(n,false),p.page>0?RGB(79,96,76):RGB(184,191,178));
    icon(dc,4,headerButton(n,true),p.count&&p.page+1<p.count?RGB(79,96,76):RGB(184,191,178));
    SaveDC(dc); IntersectClipRect(dc,content.left,content.top,content.right,content.bottom);
    if(p.image&&!p.image->pixels.empty()&&p.image->error.empty()) {
        RECT page=pageRect(n),shadow=page; OffsetRect(&shadow,px(2),px(3)); fill(dc,shadow,RGB(216,220,210));
        BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=p.image->w; bi.bmiHeader.biHeight=-p.image->h;
        bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32;
        SetDIBitsToDevice(dc,page.left,page.top,p.image->w,p.image->h,0,0,0,p.image->h,p.image->pixels.data(),&bi,DIB_RGB_COLORS);
        Gdiplus::Graphics graphics(dc); graphics.SetClip(Gdiplus::Rect(page.left,page.top,p.image->w,p.image->h)); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Document& d=documents[p.doc];
        auto rectangle=[&](const Highlight& h,bool preview) {
            Gdiplus::SolidBrush brush(preview?Gdiplus::Color(72,74,125,183):Gdiplus::Color(90,250,201,38));
            float x=page.left+h.l*p.image->w,y=page.top+h.t*p.image->h;
            graphics.FillRectangle(&brush,x-1,y-1,std::max(1.f,(h.r-h.l)*p.image->w+2),std::max(1.f,(h.b-h.t)*p.image->h+2));
        };
        for(auto& h:d.highlights) if(h.page==p.page) rectangle(h,false);
        if(selecting&&interactionPane==n) for(auto& h:selectionRects(n)) rectangle(h,true);
        for(unsigned i=0;i<d.ink.size();) {
            const auto& point=d.ink[i]; unsigned end=i+1; while(end<d.ink.size()&&!d.ink[end].start&&d.ink[end].page==point.page) end++;
            if(point.page!=p.page) {i=end;continue;}
            Gdiplus::Pen pen(point.tool==1?Gdiplus::Color(68,247,201,49):Gdiplus::Color(230,48,74,61),(float)(p.image->scale*(point.tool==1?12:1.6)));
            pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound); pen.SetLineJoin(Gdiplus::LineJoinRound);
            Gdiplus::GraphicsPath path; float x=page.left+point.x*p.image->w,y=page.top+point.y*p.image->h;
            if(end==i+1) path.AddLine(x,y,x+.01f,y+.01f);
            for(unsigned j=i+1;j<end;j++) path.AddLine(page.left+d.ink[j-1].x*p.image->w,page.top+d.ink[j-1].y*p.image->h,page.left+d.ink[j].x*p.image->w,page.top+d.ink[j].y*p.image->h);
            graphics.DrawPath(&pen,&path); i=end;
        }
    } else {
        RECT text=content; InflateRect(&text,-px(22),-px(22));
        std::wstring message=p.image&&!p.image->error.empty()?p.image->error:L"Abriendo…";
        SelectObject(dc,uiFont); label(dc,message.c_str(),text,RGB(106,118,101),DT_CENTER|DT_WORDBREAK|DT_VCENTER);
    }
    RestoreDC(dc,-1);
    if(p.busy&&p.image) { RECT r{area.right-px(112),area.bottom-px(27),area.right-px(12),area.bottom-px(7)}; rounded(dc,r,RGB(249,250,246)); label(dc,L"Cargando…",r,RGB(113,123,108)); }
}
void paint(HDC supplied=nullptr) {
    PAINTSTRUCT ps{}; HDC screen=supplied?supplied:BeginPaint(window,&ps); RECT rc=client();
    if(!canvas) canvas=CreateCompatibleDC(screen);
    if(canvasWidth!=rc.right||canvasHeight!=rc.bottom) {
        if(canvasBitmap) { SelectObject(canvas,originalBitmap); DeleteObject(canvasBitmap); }
        canvasWidth=rc.right; canvasHeight=rc.bottom; canvasBitmap=CreateCompatibleBitmap(screen,std::max<int>(1,canvasWidth),std::max<int>(1,canvasHeight)); originalBitmap=(HBITMAP)SelectObject(canvas,canvasBitmap);
    }
    HDC dc=canvas; SetBkMode(dc,TRANSPARENT); SelectObject(dc,uiFont); fill(dc,rc,RGB(239,241,235));
    RECT bar{0,0,rc.right,px(44)}; fill(dc,bar,RGB(250,251,248));
    RECT border{0,px(43),rc.right,px(44)}; fill(dc,border,RGB(225,229,219));
    RECT sideButton{px(5),px(5),px(39),px(39)}; if(sidebarVisible) rounded(dc,sideButton,RGB(231,237,225)); icon(dc,5,sideButton,RGB(81,99,77));
    int tabs=visibleTabs();
    for(int n=tabOffset;n<(int)workspaces.size()&&n<tabOffset+tabs;n++) {
        RECT tab=tabRect(n); bool active=n==activeWorkspace;
        if(active) { rounded(dc,tab,RGB(231,237,225)); RECT underline{tab.left+px(12),tab.bottom-px(2),tab.right-px(12),tab.bottom}; fill(dc,underline,RGB(144,165,130)); }
        RECT name=tab; name.left+=px(12); name.right-=px(27); SelectObject(dc,smallFont);
        std::wstring title=workspaceName(n); label(dc,title.c_str(),name,active?RGB(48,71,42):RGB(111,124,104),DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        RECT close{tab.right-px(24),tab.top+px(5),tab.right-px(3),tab.bottom-px(5)}; icon(dc,7,close,RGB(134,148,124));
    }
    RECT pair{rc.right-px(78),px(5),rc.right-px(44),px(39)},plus{rc.right-px(39),px(5),rc.right-px(5),px(39)};
    icon(dc,0,pair,RGB(81,99,77)); icon(dc,6,plus,RGB(81,99,77));
    if(sidebarVisible) {
        RECT side{0,px(44),px(58),rc.bottom}; fill(dc,side,RGB(248,250,244));
        RECT edge{px(57),px(44),px(58),rc.bottom}; fill(dc,edge,RGB(226,231,219));
        const wchar_t* names[]={L"Leer",L"Subrayar",L"Lápiz",L"Deshacer",L"Doble"}; int kinds[]={9,1,2,8,0};
        for(int n=0;n<5;n++) {
            int y=px(57+n*58); RECT box{px(9),y,px(49),y+px(34)};
            if(n<3&&tool==n) rounded(dc,box,RGB(227,236,218)); icon(dc,kinds[n],box,RGB(81,101,73));
            RECT caption{0,y+px(35),px(57),y+px(51)}; SelectObject(dc,smallFont); label(dc,names[n],caption,RGB(120,132,111));
        }
    }
    if(auto* w=workspace()) {
        for(int n=0;n<w->count;n++) paintPane(dc,n);
        if(w->count==2) { RECT left=paneArea(0),divider{left.right+px(5),px(58),left.right+px(6),rc.bottom-px(16)}; fill(dc,divider,RGB(222,228,214)); }
    } else {
        RECT center{px(30),rc.bottom/2-px(50),rc.right-px(30),rc.bottom/2+px(8)}; SelectObject(dc,headingFont);
        label(dc,L"Un espacio para estudiar.",center,RGB(48,67,44)); center.top=center.bottom; center.bottom+=px(50); SelectObject(dc,uiFont);
        label(dc,L"Arrastra tus PDFs o pulsa Ctrl + O",center,RGB(114,127,106));
    }
    if(!toast.empty()) {
        RECT r{px(sidebarVisible?74:18),rc.bottom-px(48),rc.right-px(18),rc.bottom-px(14)};
        rounded(dc,r,RGB(249,251,245)); SelectObject(dc,smallFont); label(dc,toast.c_str(),r,RGB(88,106,77),DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    }
    BitBlt(screen,0,0,rc.right,rc.bottom,dc,0,0,SRCCOPY); if(!supplied) EndPaint(window,&ps);
}
void fonts() {
    if(uiFont) DeleteObject(uiFont); if(headingFont) DeleteObject(headingFont); if(smallFont) DeleteObject(smallFont);
    auto font=[](int size,int weight) {return CreateFontW(-px(size),0,0,0,weight,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");};
    uiFont=font(14,FW_NORMAL); smallFont=font(12,FW_NORMAL); headingFont=font(28,FW_NORMAL);
}
void toggleFullscreen() {
    fullscreen=!fullscreen;
    if(fullscreen) {
        GetWindowPlacement(window,&placement); MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&mi);
        SetWindowLongPtrW(window,GWL_STYLE,WS_POPUP|WS_VISIBLE);
        SetWindowPos(window,nullptr,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED|SWP_NOZORDER);
    } else { SetWindowLongPtrW(window,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE); SetWindowPlacement(window,&placement); SetWindowPos(window,nullptr,0,0,0,0,SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER); }
}
LRESULT CALLBACK windowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
#ifdef BRISA_TEST
        case WM_APP+12: if(focusedPane()) beginExport(documents[focusedPane()->doc].path+L".exported.pdf"); return 0;
        case WM_APP+13: return exportBusy;
        case WM_APP+7: {
            HDC dc=GetDC(hwnd); paint(dc); BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth=canvasWidth; info.bmiHeader.biHeight=-canvasHeight; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
            std::vector<unsigned char> buffer((size_t)canvasWidth*canvasHeight*4); SelectObject(canvas,originalBitmap);
            GetDIBits(dc,canvasBitmap,0,canvasHeight,buffer.data(),&info,DIB_RGB_COLORS); SelectObject(canvas,canvasBitmap); ReleaseDC(hwnd,dc);
            BITMAPFILEHEADER head{}; head.bfType=0x4d42; head.bfOffBits=sizeof(head)+sizeof(BITMAPINFOHEADER); head.bfSize=head.bfOffBits+buffer.size();
            FILE* f=fopen("work/capture.bmp","wb"); if(f) { fwrite(&head,sizeof(head),1,f); fwrite(&info.bmiHeader,sizeof(BITMAPINFOHEADER),1,f); fwrite(buffer.data(),buffer.size(),1,f); fclose(f); } return 0;
        }
        case WM_APP+8: { BYTE keys[256]{}; if(wp&1) keys[VK_CONTROL]=0x80; if(wp&2) keys[VK_SHIFT]=0x80; SetKeyboardState(keys); return 0; }
        case WM_APP+9: { auto* w=workspace(); if(w) for(int n=0;n<w->count;n++) if(w->panes[n].busy) return 1; return 0; }
        case WM_APP+10: {
            auto* w=workspace(); if(wp==0) return workspaces.size(); if(wp==1) return w?w->count:0; if(wp==2) return w?w->active:0;
            if(wp==3||wp==4) return w?w->panes[wp-3].page:-1;
            if(wp==5) return focusedPane()?documents[focusedPane()->doc].highlights.size():0;
            if(wp==6) return focusedPane()&&focusedPane()->image?focusedPane()->image->glyphs.size():0;
            if(wp==7) return activeWorkspace;
            if(wp==8) return focusedPane()?documents[focusedPane()->doc].ink.size():0;
            return 0;
        }
        case WM_APP+11: {
            auto* w=workspace(); if(!w) return 0; int n=w->active; auto& p=w->panes[n]; if(!p.image) return 0; RECT r=pageRect(n);
            FILE* f=fopen("work/glyphs.tsv","wb"); if(f) {
                for(int j=0;j<(int)p.image->glyphs.size();j++) { auto& g=p.image->glyphs[j]; if(g.code<32||g.r<=g.l||g.b<=g.t) continue;
                    fprintf(f,"%d\t%u\t%.2f\t%.2f\t%.2f\t%.2f\n",j,g.code,r.left+g.l*p.image->w,r.top+g.t*p.image->h,r.left+g.r*p.image->w,r.top+g.b*p.image->h); }
                fclose(f);
            } return 0;
        }
#endif
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_PRINTCLIENT: paint((HDC)wp); return 0;
        case WM_CLOSE: if(exportBusy) { notify(L"Espera a que termine el guardado del PDF."); return 0; } finishInteraction(); if(saveAll(true)) DestroyWindow(hwnd); return 0;
        case WM_KILLFOCUS: finishInteraction(false); return 0;
        case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize={px(760),px(500)}; return 0;
        case WM_DPICHANGED: {
            finishInteraction(false); dpiScale=HIWORD(wp)/96.0; fonts(); RECT* r=(RECT*)lp;
            SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE); return 0;
        }
        case WM_SIZE: if(wp!=SIZE_MINIMIZED) { finishInteraction(false); ensureVisible(); SetTimer(hwnd,1,120,nullptr); repaint(); } return 0;
        case WM_TIMER: if(wp==1) { KillTimer(hwnd,1); requestRender(); } else if(wp==2) { KillTimer(hwnd,2); toast.clear(); repaint(); } return 0;
        case READY: {
            Result* result=(Result*)lp;
            if(result->generation!=InterlockedCompareExchange(&generation,0,0)||!workspace()) { dispose(result); return 0; }
            auto& w=*workspace();
            for(int n=0;n<w.count;n++) if(result->images[n]) { auto& p=w.panes[n]; p.image.reset(result->images[n]); result->images[n]=nullptr; p.page=p.image->page; p.count=p.image->count; p.busy=false; }
            dispose(result); updateTitle(); repaint(); return 0;
        }
        case EXPORT_READY: {
            std::unique_ptr<ExportResult> result((ExportResult*)lp); exportBusy=false;
            notify(result->error.empty()?L"PDF anotado guardado.":result->error.c_str()); return 0;
        }
        case WM_DROPFILES: {
            finishInteraction(); UINT count=DragQueryFileW((HDROP)wp,0xffffffff,nullptr,0);
            for(UINT n=0;n<count;n++) { wchar_t path[32768]; if(DragQueryFileW((HDROP)wp,n,path,32768)) openPath(path); }
            DragFinish((HDROP)wp); return 0;
        }
        case WM_SYSCOMMAND: if((wp&0xfff0)==SC_KEYMENU&&lp==0) { menuVisible=!menuVisible; SetMenu(hwnd,menuVisible?menus:nullptr); DrawMenuBar(hwnd); return 0; } break;
        case WM_COMMAND:
            if(LOWORD(wp)>=WORKSPACE_MENU&&LOWORD(wp)<WORKSPACE_MENU+128) { switchWorkspace(LOWORD(wp)-WORKSPACE_MENU); return 0; }
            if(LOWORD(wp)>=3000&&LOWORD(wp)<3064) { createPair(LOWORD(wp)-3000); return 0; }
            switch(LOWORD(wp)) {
                case OPEN: openDialog(); break; case EXPORT_PDF: exportDialog(); break; case EXIT: SendMessageW(hwnd,WM_CLOSE,0,0); break;
                case FIT: fitPage(); break; case PLUS: zoomBy(1.2); break; case MINUS: zoomBy(1/1.2); break;
                case FULL: toggleFullscreen(); break; case HAND: setTool(0); break; case MARKER: setTool(1); break; case PEN: setTool(2); break;
                case UNDO: undo(); break; case SAVE: saveAll(); break; case SIDEBAR: toggleSidebar(); break; case SPLIT: pairMenu(); break;
            } return 0;
        case WM_LBUTTONDOWN: {
            int x=GET_X_LPARAM(lp),y=GET_Y_LPARAM(lp); RECT rc=client();
            if(y<px(44)) {
                if(x<px(44)) toggleSidebar(); else if(x>rc.right-px(42)) openDialog(); else if(x>rc.right-px(82)) pairMenu();
                else for(int n=tabOffset;n<(int)workspaces.size()&&n<tabOffset+visibleTabs();n++) { RECT r=tabRect(n); if(PtInRect(&r,{x,y})) { if(x>r.right-px(25)) closeWorkspace(n); else switchWorkspace(n); break; } }
                return 0;
            }
            if(sidebarVisible&&x<px(58)) {
                int row=(y-px(57))/px(58); if(y>=px(57)&&row>=0&&row<5) { if(row<3) setTool(row); else if(row==3) undo(); else pairMenu(); } return 0;
            }
            int n=paneAt(x,y); if(n<0) return 0; setActivePane(n);
            RECT prev=headerButton(n,false),next=headerButton(n,true);
            if(PtInRect(&prev,{x,y})) { go(focusedPane()->page-1,n); return 0; }
            if(PtInRect(&next,{x,y})) { go(focusedPane()->page+1,n); return 0; }
            RECT area=contentArea(n); if(!PtInRect(&area,{x,y})) return 0;
            Pane& p=workspace()->panes[n]; if(p.busy||!p.image||!p.image->error.empty()) return 0;
            interactionPane=n;
            if(tool==1) {
                selectionStart=selectionEnd=glyphAt(n,x,y,true);
                if(selectionStart>=0) { selecting=true; SetCapture(hwnd); repaint(); }
                else if(glyphAt(n,x,y,false)<0) notify(L"Esta página no tiene texto seleccionable. Puedes usar el lápiz.");
            } else if(tool==2) { if(addInk(n,x,y,true)) { drawing=true; SetCapture(hwnd); } }
            else { dragging=true; dragX=x; dragY=y; SetCapture(hwnd); }
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x=GET_X_LPARAM(lp),y=GET_Y_LPARAM(lp);
            if(selecting) { int index=glyphAt(interactionPane,x,y,false); if(index>=0) selectionEnd=index; repaint(); }
            else if(drawing) { if(!addInk(interactionPane,x,y,false)) finishInteraction(); }
            else if(dragging&&workspace()) { Pane& p=workspace()->panes[interactionPane]; p.panX+=x-dragX; p.panY+=y-dragY; dragX=x; dragY=y; repaint(); }
            return 0;
        }
        case WM_LBUTTONUP:
            if(selecting) { int n=glyphAt(interactionPane,GET_X_LPARAM(lp),GET_Y_LPARAM(lp),false); if(n>=0) selectionEnd=n; }
            if(drawing) addInk(interactionPane,GET_X_LPARAM(lp),GET_Y_LPARAM(lp),false);
            finishInteraction(); return 0;
        case WM_CAPTURECHANGED: if(dragging||drawing||selecting) finishInteraction(false); return 0;
        case WM_SETCURSOR:
            if(LOWORD(lp)==HTCLIENT) {
                POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd,&pt); int n=paneAt(pt.x,pt.y); bool content=false;
                if(n>=0) { RECT r=contentArea(n); content=PtInRect(&r,pt); }
                SetCursor(content&&tool==1?LoadCursorW(nullptr,IDC_IBEAM):content&&tool==2?penCursor:dragging?LoadCursorW(nullptr,IDC_SIZEALL):LoadCursorW(nullptr,IDC_ARROW)); return TRUE;
            } break;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ScreenToClient(hwnd,&pt); int delta=GET_WHEEL_DELTA_WPARAM(wp);
            if(pt.y<px(44)) { tabOffset=std::clamp(tabOffset+(delta<0?1:-1),0,std::max<int>(0,(int)workspaces.size()-visibleTabs())); repaint(); return 0; }
            int n=paneAt(pt.x,pt.y); if(n<0) return 0; setActivePane(n); Pane& p=*focusedPane();
            if(GET_KEYSTATE_WPARAM(wp)&MK_CONTROL) zoomBy(delta>0?1.2:1/1.2,n);
            else if(!p.fit) { p.panY+=delta; repaint(); }
            else if(abs(delta)>=WHEEL_DELTA) go(p.page+(delta<0?1:-1),n);
            return 0;
        }
        case WM_KEYDOWN: {
            bool ctrl=GetKeyState(VK_CONTROL)&0x8000,shift=GetKeyState(VK_SHIFT)&0x8000;
            Pane* p=focusedPane();
            if(ctrl&&wp>='1'&&wp<='9') switchWorkspace((int)wp-'1');
            else if(ctrl&&wp==VK_TAB&&!workspaces.empty()) switchWorkspace((activeWorkspace+(int)workspaces.size()+(shift?-1:1))%(int)workspaces.size());
            else if(ctrl&&wp=='O') openDialog(); else if(ctrl&&shift&&wp=='S') exportDialog(); else if(ctrl&&wp=='S') { finishInteraction(); saveAll(); } else if(ctrl&&wp=='Z') undo();
            else if(ctrl&&wp=='W') closeWorkspace(activeWorkspace); else if(ctrl&&wp=='B') toggleSidebar(); else if(ctrl&&wp=='D') pairMenu();
            else if(wp=='H') setTool(1); else if(wp=='P') setTool(2); else if(wp=='V') setTool(0);
            else if(wp==VK_F10) { menuVisible=!menuVisible; SetMenu(hwnd,menuVisible?menus:nullptr); DrawMenuBar(hwnd); }
            else if(wp==VK_TAB&&workspace()&&workspace()->count==2) setActivePane(1-workspace()->active);
            else if(p&&(wp==VK_NEXT||wp==VK_RIGHT||wp==VK_SPACE)) go(p->page+1);
            else if(p&&(wp==VK_PRIOR||wp==VK_LEFT)) go(p->page-1);
            else if(p&&wp==VK_HOME) go(0); else if(p&&wp==VK_END) go(p->count-1);
            else if(wp==VK_ADD||wp==VK_OEM_PLUS) zoomBy(1.2); else if(wp==VK_SUBTRACT||wp==VK_OEM_MINUS) zoomBy(1/1.2);
            else if(wp=='0') fitPage(); else if(wp==VK_F11) toggleFullscreen();
            else if(wp==VK_ESCAPE) { finishInteraction(false); if(fullscreen) toggleFullscreen(); else if(menuVisible) {menuVisible=false;SetMenu(hwnd,nullptr);DrawMenuBar(hwnd);} else setTool(0); }
            else if(p&&(wp==VK_UP||wp==VK_DOWN)) { p->panY+=wp==VK_UP?px(80):-px(80); repaint(); }
            return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
int selfTest(const wchar_t* path) {
    FPDF_InitLibrary(); Engine engine; Item item; item.path=path; item.width=900; item.height=1100;
    auto image=std::unique_ptr<Image>(render(engine,item,0,false));
    bool ok=image->error.empty()&&!image->pixels.empty();
    std::wstring report=L"ok="+std::to_wstring(ok)+L"\npages="+std::to_wstring(image->count)+L"\nglyphs="+std::to_wstring(image->glyphs.size())+L"\nerror="+image->error+L"\n";
    std::wstring target=item.path+L".brisa-test.txt";
    HANDLE file=CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE) { int len=WideCharToMultiByte(CP_UTF8,0,report.c_str(),-1,nullptr,0,nullptr,nullptr); std::vector<char> bytes(len); WideCharToMultiByte(CP_UTF8,0,report.c_str(),-1,bytes.data(),len,nullptr,nullptr); DWORD done; WriteFile(file,bytes.data(),len-1,&done,nullptr); CloseHandle(file); }
    engine.close(); FPDF_DestroyLibrary(); return ok?0:1;
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    int argc=0; wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
#ifdef BRISA_TEST
    if(argc==4&&!wcscmp(argv[1],L"--export-test")) {
        FPDF_InitLibrary(); Document document; document.path=argv[2]; loadDocument(document);
        std::unique_ptr<ExportResult> result(exportPdf({document,argv[3]}));
        int code=result->error.empty()?0:1; FPDF_DestroyLibrary(); LocalFree(argv); return code;
    }
#endif
    if(argc>=3&&!wcscmp(argv[1],L"--self-test")) { int code=selfTest(argv[2]); LocalFree(argv); return code; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput startup; Gdiplus::GdiplusStartup(&gdiplusToken,&startup,nullptr);
    dpiScale=GetDpiForSystem()/96.0; fonts(); InitializeCriticalSection(&mutex); wakeEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    BYTE andMask[128],xorMask[128]{}; memset(andMask,255,sizeof(andMask));
    for(int y=0;y<32;y++) for(int x=0;x<32;x++) {int d=(x-16)*(x-16)+(y-16)*(y-16);if(d>=5&&d<=13)andMask[y*4+x/8]&=~(0x80>>(x%8));}
    penCursor=CreateCursor(instance,16,16,32,32,andMask,xorMask);
    WNDCLASSW cls{}; cls.lpfnWndProc=windowProc; cls.hInstance=instance; cls.lpszClassName=L"BrisaPdfWindow"; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hIcon=LoadIconW(nullptr,IDI_APPLICATION); RegisterClassW(&cls);
    menus=CreateMenu(); HMENU file=CreatePopupMenu(),view=CreatePopupMenu(),mark=CreatePopupMenu(); workspaceMenu=CreatePopupMenu();
    AppendMenuW(file,MF_STRING,OPEN,L"Abrir PDFs…\tCtrl+O"); AppendMenuW(file,MF_STRING,SAVE,L"Guardar marcas\tCtrl+S"); AppendMenuW(file,MF_STRING,EXPORT_PDF,L"Guardar PDF anotado…\tCtrl+Shift+S"); AppendMenuW(file,MF_STRING,EXIT,L"Salir");
    AppendMenuW(view,MF_STRING,FIT,L"Ajustar página\t0"); AppendMenuW(view,MF_STRING,PLUS,L"Acercar\t+"); AppendMenuW(view,MF_STRING,MINUS,L"Alejar\t−");
    AppendMenuW(view,MF_STRING,SIDEBAR,L"Herramientas\tCtrl+B"); AppendMenuW(view,MF_STRING,SPLIT,L"Dos PDFs lado a lado\tCtrl+D"); AppendMenuW(view,MF_STRING,FULL,L"Pantalla completa\tF11");
    AppendMenuW(mark,MF_STRING|MF_CHECKED,HAND,L"Leer / mover\tV"); AppendMenuW(mark,MF_STRING,MARKER,L"Subrayar texto\tH"); AppendMenuW(mark,MF_STRING,PEN,L"Lápiz\tP"); AppendMenuW(mark,MF_STRING,UNDO,L"Deshacer\tCtrl+Z");
    AppendMenuW(menus,MF_POPUP,(UINT_PTR)file,L"Archivo"); AppendMenuW(menus,MF_POPUP,(UINT_PTR)view,L"Vista"); AppendMenuW(menus,MF_POPUP,(UINT_PTR)mark,L"Marcar"); AppendMenuW(menus,MF_POPUP,(UINT_PTR)workspaceMenu,L"Workspaces");
    window=CreateWindowExW(WS_EX_ACCEPTFILES,cls.lpszClassName,L"Brisa",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,px(1240),px(880),nullptr,nullptr,instance,nullptr);
    if(!window||!wakeEvent) return 1; worker=CreateThread(nullptr,0,workerMain,nullptr,0,nullptr); if(!worker) return 1;
    for(int n=1;n<argc;n++) openPath(argv[n]); LocalFree(argv);
    ShowWindow(window,show); UpdateWindow(window);
    MSG msg; while(GetMessageW(&msg,nullptr,0,0)>0) {TranslateMessage(&msg);DispatchMessageW(&msg);}
    InterlockedExchange(&stopping,1); SetEvent(wakeEvent); WaitForSingleObject(worker,INFINITE);
    while(PeekMessageW(&msg,nullptr,READY,READY,PM_REMOVE)) dispose((Result*)msg.lParam);
    while(PeekMessageW(&msg,nullptr,EXPORT_READY,EXPORT_READY,PM_REMOVE)) delete (ExportResult*)msg.lParam;
    CloseHandle(worker);CloseHandle(wakeEvent);DeleteCriticalSection(&mutex);
    if(canvas) {SelectObject(canvas,originalBitmap);DeleteObject(canvasBitmap);DeleteDC(canvas);}
    DeleteObject(uiFont);DeleteObject(smallFont);DeleteObject(headingFont);DestroyCursor(penCursor);DestroyMenu(menus); Gdiplus::GdiplusShutdown(gdiplusToken); return 0;
}
