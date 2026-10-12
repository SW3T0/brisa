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

// PDFium is owned exclusively by the worker. UI never calls its API.
constexpr UINT READY = WM_APP + 1;
constexpr double PIXEL_LIMIT = 12000000; // 48 MB per page raster.
struct Request { LONG generation; wchar_t path[32768]; int page, width, height; double zoom; bool fit; };
struct Image { LONG generation; int page, count, w, h; double scale; void* pixels; wchar_t error[160]; };
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
HWND window;
CRITICAL_SECTION mutex;
HANDLE wakeEvent, worker;
volatile LONG generation = 0, stopping = 0;
Request pending{};
Image* displayed = nullptr;
wchar_t currentPath[32768]{};
int pageIndex = 0, pageCount = 0, panX = 0, panY = 0;
int dragX = 0, dragY = 0;
double zoom = 1.0;
bool fit = true, busy = false, dragging = false, fullscreen = false;
WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
HFONT uiFont, headingFont;
HDC canvas = nullptr;
HBITMAP canvasBitmap = nullptr, originalBitmap = nullptr;
int canvasWidth = 0, canvasHeight = 0;
int toolbar = 0;
double dpiScale = 1;
HMENU menus;
bool menusVisible = false, inkDirty = false, drawingInk = false;
int inkTool = 0; // 0 hand, 1 highlighter, 2 pen
ULONG_PTR gdiplusToken;
struct InkPoint { int page, start, tool; float x, y; };
InkPoint* ink = nullptr;
unsigned inkCount = 0, inkCapacity = 0;
constexpr unsigned INK_LIMIT = 200000;
struct InkHeader { char magic[8]; unsigned count; DWORD sizeLow,sizeHigh; FILETIME modified; };
enum { OPEN=100, EXIT, FIT, PLUS, MINUS, FULL, HAND, MARKER, PEN, UNDO, SAVE };
bool saveInk() {
    if(!currentPath[0] || !inkDirty) return true;
    wchar_t path[32768], temp[32768];
    if(wcslen(currentPath)>32730) return false;
    swprintf(path,32768,L"%ls.brisa-ink",currentPath); swprintf(temp,32768,L"%ls.tmp",path);
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if(!GetFileAttributesExW(currentPath,GetFileExInfoStandard,&attr)) return false;
    InkHeader h{{'B','R','I','S','A','0','1',0},inkCount,attr.nFileSizeLow,attr.nFileSizeHigh,attr.ftLastWriteTime};
    HANDLE f=CreateFileW(temp,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return false;
    DWORD done=0; bool ok=WriteFile(f,&h,sizeof(h),&done,nullptr) && done==sizeof(h);
    DWORD bytes=inkCount*sizeof(InkPoint);
    ok=ok && WriteFile(f,ink,bytes,&done,nullptr) && done==bytes && FlushFileBuffers(f);
    CloseHandle(f);
    ok=ok && MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if(!ok) DeleteFileW(temp); else inkDirty=false;
    return ok;
}
void loadInk() {
    inkCount=0; inkDirty=false; drawingInk=false;
    wchar_t path[32768]; if(wcslen(currentPath)>32730) return;
    swprintf(path,32768,L"%ls.brisa-ink",currentPath);
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return;
    InkHeader h{}; DWORD done; WIN32_FILE_ATTRIBUTE_DATA attr{};
    bool ok=ReadFile(f,&h,sizeof(h),&done,nullptr) && done==sizeof(h) && !memcmp(h.magic,"BRISA01",8) && h.count<=INK_LIMIT;
    ok=ok && GetFileAttributesExW(currentPath,GetFileExInfoStandard,&attr) && h.sizeLow==attr.nFileSizeLow && h.sizeHigh==attr.nFileSizeHigh && CompareFileTime(&h.modified,&attr.ftLastWriteTime)==0;
    if(ok && h.count) {
        InkPoint* buffer=(InkPoint*)malloc(h.count*sizeof(InkPoint));
        ok=buffer && ReadFile(f,buffer,h.count*sizeof(InkPoint),&done,nullptr) && done==h.count*sizeof(InkPoint);
        for(unsigned n=0;ok && n<h.count;n++) ok=buffer[n].page>=0 && buffer[n].page<10000000 && (buffer[n].tool==1 || buffer[n].tool==2) && (buffer[n].start==0 || buffer[n].start==1) && isfinite(buffer[n].x) && isfinite(buffer[n].y) && buffer[n].x>=0 && buffer[n].x<=1 && buffer[n].y>=0 && buffer[n].y<=1;
        if(ok) { free(ink); ink=buffer; inkCount=inkCapacity=h.count; } else free(buffer);
    }
    CloseHandle(f);
}
bool protectInk() {
    if(!inkDirty || saveInk()) return true;
    return MessageBoxW(window,L"No se pudieron guardar las marcas junto al PDF. ¿Continuar y descartarlas?",L"Brisa",MB_YESNO|MB_ICONWARNING)==IDYES;
}
bool addInk(int x,int y,bool start) {
    if(!displayed || !displayed->pixels || displayed->error[0] || busy || displayed->page!=pageIndex || inkCount>=INK_LIMIT) return false;
    RECT rc; GetClientRect(window,&rc);
    int left=(rc.right-displayed->w)/2+panX, top=toolbar+(rc.bottom-toolbar-displayed->h)/2+panY;
    float nx=(x-left)/(float)displayed->w, ny=(y-top)/(float)displayed->h;
    if(nx<0 || nx>1 || ny<0 || ny>1) return false;
    if(inkCount==inkCapacity) {
        unsigned capacity=inkCapacity?inkCapacity*2:1024; if(capacity>INK_LIMIT) capacity=INK_LIMIT;
        InkPoint* next=(InkPoint*)realloc(ink,capacity*sizeof(InkPoint)); if(!next) return false;
        ink=next; inkCapacity=capacity;
    }
    ink[inkCount++]={pageIndex,start?1:0,inkTool,nx,ny}; inkDirty=true; InvalidateRect(window,nullptr,FALSE); return true;
}
void undoInk() {
    for(unsigned n=inkCount;n>0;n--) if(ink[n-1].page==pageIndex && ink[n-1].start) {
        unsigned end=n; while(end<inkCount && !ink[end].start) end++;
        memmove(ink+n-1,ink+end,(inkCount-end)*sizeof(InkPoint)); inkCount-=end-n+1;
        inkDirty=true; saveInk(); InvalidateRect(window,nullptr,FALSE); return;
    }
}
void updateTitle();
void setTool(int tool) {
    inkTool=tool;
    CheckMenuRadioItem(GetSubMenu(menus,2),HAND,PEN,HAND+tool,MF_BYCOMMAND);
    updateTitle();
}
void toggleMenus() { menusVisible=!menusVisible; SetMenu(window,menusVisible?menus:nullptr); DrawMenuBar(window); }
void updateTitle() {
    wchar_t title[600]; const wchar_t* name=wcsrchr(currentPath,L'\\'); name=name?name+1:currentPath;
    const wchar_t* tool=inkTool==1?L" · Subrayador":inkTool==2?L" · Lápiz":L"";
    if(pageCount) swprintf(title,600,L"Brisa — %.450ls · %d/%d%ls",name,pageIndex+1,pageCount,tool);
    else swprintf(title,600,L"Brisa — %.450ls%ls",name,tool);
    SetWindowTextW(window,title);
}

int px(int x) { return (int)round(x * dpiScale); }
void dispose(Image* i) { if (i) { free(i->pixels); delete i; } }
bool stale(LONG g) { return InterlockedCompareExchange(&stopping, 0, 0) || InterlockedCompareExchange(&generation, 0, 0) != g; }
int pauseRender(IFSDK_PAUSE* p) { return stale(*(LONG*)p->user); }

Image* render(Engine& e, Request r, bool cancellable) {
    Image* out = new Image{}; out->generation = r.generation;
    if ((!e.doc || wcscmp(e.path, r.path)) && !e.open(r.path, out->error)) return out;
    out->count = FPDF_GetPageCount(e.doc);
    if (out->count < 1) { wcscpy(out->error, L"El PDF no contiene páginas."); return out; }
    out->page = r.page < 0 ? 0 : r.page >= out->count ? out->count - 1 : r.page;
    FPDF_PAGE p = FPDF_LoadPage(e.doc, out->page);
    if (!p) { wcscpy(out->error, L"No se puede cargar esta página."); return out; }
    double w = FPDF_GetPageWidth(p), h = FPDF_GetPageHeight(p);
    if (!isfinite(w) || !isfinite(h) || w <= 0 || h <= 0) {
        FPDF_ClosePage(p); wcscpy(out->error, L"La página tiene un tamaño inválido."); return out;
    }
    double scale = r.fit ? fmin(r.width / w, r.height / h) : r.zoom;
    scale = fmax(.02, scale);
    scale = fmin(scale, sqrt(PIXEL_LIMIT / (w * h)));
    // Independent bounds also protect extreme aspect ratios.
    scale = fmin(scale, fmin(16000.0 / w, 16000.0 / h));
    out->w = (int)fmax(1, floor(w * scale)); out->h = (int)fmax(1, floor(h * scale)); out->scale = scale;
    out->pixels = malloc((size_t)out->w * out->h * 4);
    FPDF_BITMAP b = out->pixels ? FPDFBitmap_CreateEx(out->w, out->h, FPDFBitmap_BGRx, out->pixels, out->w * 4) : nullptr;
    if (!b) { wcscpy(out->error, L"No hay memoria suficiente para renderizar."); }
    else {
        FPDFBitmap_FillRect(b, 0, 0, out->w, out->h, 0xffffffff);
        int status = FPDF_RENDER_DONE;
        if (cancellable) {
            IFSDK_PAUSE pause{1, pauseRender, &r.generation};
            status = FPDF_RenderPageBitmap_Start(b, p, 0, 0, out->w, out->h, 0, FPDF_ANNOT, &pause);
            while (status == FPDF_RENDER_TOBECONTINUED && !stale(r.generation)) status = FPDF_RenderPage_Continue(p, &pause);
            FPDF_RenderPage_Close(p);
        } else FPDF_RenderPageBitmap(b, p, 0, 0, out->w, out->h, 0, FPDF_ANNOT);
        if (status == FPDF_RENDER_FAILED) wcscpy(out->error, L"No se puede renderizar esta página.");
        FPDFBitmap_Destroy(b);
    }
    FPDF_ClosePage(p);
    return out;
}
DWORD WINAPI workerMain(void*) {
    FPDF_InitLibrary(); Engine engine;
    while (true) {
        WaitForSingleObject(wakeEvent, INFINITE);
        if (InterlockedCompareExchange(&stopping, 0, 0)) break;
        EnterCriticalSection(&mutex); Request r = pending; LeaveCriticalSection(&mutex);
        if (!r.path[0]) continue;
        Image* result = render(engine, r, true);
        if (stale(r.generation) || !PostMessageW(window, READY, 0, (LPARAM)result)) dispose(result);
    }
    engine.close(); FPDF_DestroyLibrary(); return 0;
}
void requestRender() {
    if (!currentPath[0]) return;
    RECT rc; GetClientRect(window, &rc);
    Request r{}; r.generation = InterlockedIncrement(&generation);
    wcscpy(r.path, currentPath); r.page = pageIndex;
    r.width = rc.right - px(48); r.height = rc.bottom - toolbar - px(48);
    if (r.width < 1) r.width = 1; if (r.height < 1) r.height = 1;
    r.zoom = zoom; r.fit = fit;
    EnterCriticalSection(&mutex); pending = r; LeaveCriticalSection(&mutex);
    busy = true; SetEvent(wakeEvent); InvalidateRect(window, nullptr, FALSE);
}
void openPath(const wchar_t* p) {
    if (wcslen(p) >= 32768 || !protectInk()) return;
    wcscpy(currentPath, p); pageIndex = pageCount = 0; panX = panY = 0; fit = true;
    loadInk(); dispose(displayed); displayed = nullptr; updateTitle(); requestRender();
}
void openDialog() {
    wchar_t path[32768]{}; OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner=window; ofn.lpstrFilter=L"Documentos PDF\0*.pdf\0Todos los archivos\0*.*\0";
    ofn.lpstrFile=path; ofn.nMaxFile=32768; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(GetOpenFileNameW(&ofn)) openPath(path);
}
void go(int next) {
    if (pageCount < 1) return;
    next = next < 0 ? 0 : next >= pageCount ? pageCount - 1 : next;
    if (next == pageIndex) return;
    pageIndex = next; panX = panY = 0; requestRender();
}
void changeZoom(double factor) {
    if (!currentPath[0]) return;
    zoom = fit && displayed ? displayed->scale : zoom;
    zoom = fmin(8, fmax(.1, zoom * factor)); fit = false; panX = panY = 0; requestRender();
}
void toggleFullscreen() {
    fullscreen = !fullscreen;
    if (fullscreen) {
        GetWindowPlacement(window, &placement); MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window, nullptr, mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOZORDER);
    } else {
        SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(window, &placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }
}
void fill(HDC dc, RECT r, COLORREF color) { HBRUSH b = CreateSolidBrush(color); FillRect(dc, &r, b); DeleteObject(b); }
void text(HDC dc, const wchar_t* s, RECT r, COLORREF color, UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE) {
    SetTextColor(dc, color); DrawTextW(dc, s, -1, &r, format);
}
void clampPan(int width, int height) {
    if (!displayed) return;
    int mx = (displayed->w > width - px(48)) ? (displayed->w - width + px(48)) / 2 : 0;
    int my = (displayed->h > height - px(48)) ? (displayed->h - height + px(48)) / 2 : 0;
    panX = panX < -mx ? -mx : panX > mx ? mx : panX;
    panY = panY < -my ? -my : panY > my ? my : panY;
}
void paint(HDC supplied = nullptr) {
    PAINTSTRUCT ps{}; HDC screen = supplied ? supplied : BeginPaint(window, &ps); RECT rc; GetClientRect(window, &rc);
    if(!canvas) canvas=CreateCompatibleDC(screen);
    if(canvasWidth!=rc.right || canvasHeight!=rc.bottom) {
        if(canvasBitmap) { SelectObject(canvas,originalBitmap); DeleteObject(canvasBitmap); }
        canvasWidth=rc.right; canvasHeight=rc.bottom;
        canvasBitmap=CreateCompatibleBitmap(screen,canvasWidth>0?canvasWidth:1,canvasHeight>0?canvasHeight:1);
        originalBitmap=(HBITMAP)SelectObject(canvas,canvasBitmap);
    }
    HDC dc=canvas;
    SetBkMode(dc, TRANSPARENT); SelectObject(dc, uiFont);
    fill(dc, rc, RGB(239,239,235));
    SaveDC(dc); IntersectClipRect(dc,0,toolbar,rc.right,rc.bottom);
    if (displayed && displayed->pixels && !displayed->error[0]) {
        clampPan(rc.right,rc.bottom-toolbar);
        int x = (rc.right - displayed->w)/2 + panX;
        int y = toolbar + (rc.bottom-toolbar-displayed->h)/2 + panY;
        RECT shadow{x+px(3),y+px(4),x+displayed->w+px(3),y+displayed->h+px(4)};
        fill(dc,shadow,RGB(215,218,208));
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = displayed->w; bi.bmiHeader.biHeight = -displayed->h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc,x,y,displayed->w,displayed->h,0,0,0,displayed->h,displayed->pixels,&bi,DIB_RGB_COLORS);
        Gdiplus::Graphics graphics(dc);
        graphics.SetClip(Gdiplus::Rect(x,y,displayed->w,displayed->h));
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // Width follows PDF points; zoom never changes where marks sit.
        for(unsigned n=0;n<inkCount;) {
            const InkPoint& point=ink[n];
            unsigned end=n+1; while(end<inkCount && !ink[end].start && ink[end].page==point.page) end++;
            if(point.page!=displayed->page) { n=end; continue; }
            float width=(float)(displayed->scale*(point.tool==1?12:1.6));
            Gdiplus::Pen pen(point.tool==1?Gdiplus::Color(68,247,201,49):Gdiplus::Color(230,48,74,61),width);
            pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            Gdiplus::GraphicsPath path;
            float ax=x+point.x*displayed->w, ay=y+point.y*displayed->h;
            if(end==n+1) path.AddLine(ax,ay,ax+.01f,ay+.01f);
            for(unsigned j=n+1;j<end;j++) path.AddLine(x+ink[j-1].x*displayed->w,y+ink[j-1].y*displayed->h,x+ink[j].x*displayed->w,y+ink[j].y*displayed->h);
            graphics.DrawPath(&pen,&path); n=end;
        }
    } else {
        RECT center{px(35),(rc.bottom+toolbar)/2-px(68),rc.right-px(35),(rc.bottom+toolbar)/2};
        SelectObject(dc,headingFont);
        text(dc, displayed && displayed->error[0] ? L"No se pudo abrir el PDF" : busy ? L"Abriendo…" : L"Un espacio para estudiar.",center,RGB(47,64,49));
        SelectObject(dc,uiFont); center.top = center.bottom; center.bottom += px(55);
        text(dc,displayed && displayed->error[0] ? displayed->error : L"Arrastra un PDF aquí o pulsa Ctrl + O\nAlt · menús     H · subrayar     V · mover",center,RGB(104,113,100),DT_CENTER|DT_WORDBREAK);
    }
    if (busy && displayed && displayed->pixels) {
        RECT status{rc.right-px(142),rc.bottom-px(36),rc.right-px(16),rc.bottom-px(12)};
        fill(dc,status,RGB(250,250,247)); text(dc,L"Cargando…",status,RGB(89,98,86));
    }
    RestoreDC(dc,-1); BitBlt(screen,0,0,rc.right,rc.bottom,dc,0,0,SRCCOPY); if(!supplied) EndPaint(window,&ps);
}
void fonts() {
    if (uiFont) DeleteObject(uiFont); if (headingFont) DeleteObject(headingFont);
    uiFont = CreateFontW(-px(14),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    headingFont = CreateFontW(-px(28),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    toolbar = 0;
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
#ifdef BRISA_TEST
        case WM_APP+9: return busy ? 1 : 0;
        case WM_APP+8: {
            BYTE keys[256]{}; if(wp&1) keys[VK_CONTROL]=0x80; if(wp&2) keys[VK_SHIFT]=0x80;
            SetKeyboardState(keys); return 0;
        }
        case WM_APP+7: {
            HDC dc=GetDC(hwnd); paint(dc);
            BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth=canvasWidth; info.bmiHeader.biHeight=-canvasHeight;
            info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
            size_t size=(size_t)canvasWidth*canvasHeight*4; void* buffer=malloc(size);
            SelectObject(canvas,originalBitmap);
            GetDIBits(dc,canvasBitmap,0,canvasHeight,buffer,&info,DIB_RGB_COLORS);
            SelectObject(canvas,canvasBitmap); ReleaseDC(hwnd,dc);
            BITMAPFILEHEADER head{}; head.bfType=0x4d42; head.bfOffBits=sizeof(head)+sizeof(BITMAPINFOHEADER); head.bfSize=head.bfOffBits+size;
            FILE* f=fopen("work/capture.bmp","wb");
            if(f) { fwrite(&head,sizeof(head),1,f); fwrite(&info.bmiHeader,sizeof(BITMAPINFOHEADER),1,f); fwrite(buffer,size,1,f); fclose(f); }
            free(buffer); return 0;
        }
#endif
        case WM_CLOSE: if(protectInk()) DestroyWindow(hwnd); return 0;
        case WM_SYSCOMMAND: if((wp&0xfff0)==SC_KEYMENU && lp==0) { toggleMenus(); return 0; } break;
        case WM_COMMAND:
            switch(LOWORD(wp)) {
                case OPEN: openDialog(); break; case EXIT: SendMessageW(hwnd,WM_CLOSE,0,0); break;
                case FIT: fit=true; panX=panY=0; requestRender(); break;
                case PLUS: changeZoom(1.2); break; case MINUS: changeZoom(1/1.2); break;
                case FULL: toggleFullscreen(); break; case HAND: setTool(0); break;
                case MARKER: setTool(1); break; case PEN: setTool(2); break;
                case UNDO: undoInk(); break;
                case SAVE: if(!saveInk()) MessageBoxW(hwnd,L"No se pudieron guardar las marcas. Comprueba los permisos de la carpeta.",L"Brisa",MB_OK|MB_ICONERROR); break;
            } return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_PRINTCLIENT: paint((HDC)wp); return 0;
        case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = {px(640),px(420)}; return 0;
        case WM_DPICHANGED: {
            dpiScale = HIWORD(wp)/96.0; fonts(); RECT* r = (RECT*)lp;
            SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE); return 0;
        }
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) { KillTimer(hwnd,1); SetTimer(hwnd,1,120,nullptr); InvalidateRect(hwnd,nullptr,FALSE); } return 0;
        case WM_TIMER: KillTimer(hwnd,1); if (fit) requestRender(); return 0;
        case READY: {
            Image* i = (Image*)lp;
            if (i->generation != InterlockedCompareExchange(&generation,0,0)) { dispose(i); return 0; }
            dispose(displayed); displayed = i; busy = false; pageCount = i->count; pageIndex = i->page;
            updateTitle();
            InvalidateRect(hwnd,nullptr,FALSE); return 0;
        }
        case WM_DROPFILES: {
            wchar_t p[32768]; if(DragQueryFileW((HDROP)wp,0,p,32768)) openPath(p); DragFinish((HDROP)wp); return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (inkTool && addInk(x,y,true)) { drawingInk=true; SetCapture(hwnd); }
            else if (!inkTool && y > toolbar && displayed && displayed->pixels) { dragging=true; dragX=x; dragY=y; SetCapture(hwnd); }
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (drawingInk) { if(!addInk(x,y,false)) { drawingInk=false; ReleaseCapture(); saveInk(); } }
            else if (dragging) { panX += x-dragX; panY += y-dragY; dragX=x; dragY=y; InvalidateRect(hwnd,nullptr,FALSE); }
            return 0;
        }
        case WM_LBUTTONUP: if(drawingInk) { addInk(GET_X_LPARAM(lp),GET_Y_LPARAM(lp),false); drawingInk=false; saveInk(); } dragging=false; ReleaseCapture(); return 0;
        case WM_CAPTURECHANGED: if(drawingInk) { drawingInk=false; saveInk(); } dragging=false; return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp)==HTCLIENT) { SetCursor(LoadCursorW(nullptr,inkTool?IDC_CROSS:dragging?IDC_SIZEALL:IDC_ARROW)); return TRUE; } break;
        case WM_MOUSEWHEEL: {
            int delta=GET_WHEEL_DELTA_WPARAM(wp);
            if (GET_KEYSTATE_WPARAM(wp)&MK_CONTROL) changeZoom(delta>0?1.2:1/1.2);
            else if (displayed && !fit) { panY += delta; InvalidateRect(hwnd,nullptr,FALSE); }
            else if (abs(delta)>=WHEEL_DELTA) go(pageIndex+(delta<0?1:-1));
            return 0;
        }
        case WM_KEYDOWN:
            if ((GetKeyState(VK_CONTROL)&0x8000) && wp=='O') openDialog();
            else if ((GetKeyState(VK_CONTROL)&0x8000) && wp=='S') SendMessageW(hwnd,WM_COMMAND,SAVE,0);
            else if ((GetKeyState(VK_CONTROL)&0x8000) && wp=='Z') undoInk();
            else if(wp=='H') setTool(1); else if(wp=='P') setTool(2); else if(wp=='V') setTool(0);
            else if(wp==VK_F10) toggleMenus();
            else if (wp==VK_NEXT || wp==VK_RIGHT || wp==VK_SPACE) go(pageIndex+1);
            else if (wp==VK_PRIOR || wp==VK_LEFT) go(pageIndex-1);
            else if (wp==VK_HOME) go(0); else if (wp==VK_END) go(pageCount-1);
            else if (wp==VK_ADD || wp==VK_OEM_PLUS) changeZoom(1.2);
            else if (wp==VK_SUBTRACT || wp==VK_OEM_MINUS) changeZoom(1/1.2);
            else if (wp=='0') { fit=true; panX=panY=0; requestRender(); }
            else if (wp==VK_F11 || (wp==VK_ESCAPE && fullscreen)) toggleFullscreen();
            else if (wp==VK_ESCAPE) { setTool(0); if(menusVisible) toggleMenus(); }
            else if (wp==VK_UP || wp==VK_DOWN) { panY += wp==VK_UP?px(80):-px(80); InvalidateRect(hwnd,nullptr,FALSE); }
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

int selfTest(const wchar_t* path) {
    FPDF_InitLibrary(); Engine e; Request r{}; wcscpy(r.path,path); r.width=900; r.height=1100; r.fit=true;
    LARGE_INTEGER freq, before, after; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&before);
    Image* a=render(e,r,false); QueryPerformanceCounter(&after);
    bool ok=a->pixels && !a->error[0] && a->count>=1;
    unsigned long long hash=1469598103934665603ULL;
    if(ok) for(size_t n=0;n<(size_t)a->w*a->h*4;n++) { hash^=((unsigned char*)a->pixels)[n]; hash*=1099511628211ULL; }
    PROCESS_MEMORY_COUNTERS_EX mem{}; GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&mem,sizeof(mem));
    wchar_t report[1024]; swprintf(report,1024,L"ok=%d\npages=%d\nraster=%dx%d\nrender_ms=%.2f\nworking_set_bytes=%llu\nprivate_bytes=%llu\nhash=%llu\nerror=%ls\n",ok,a->count,a->w,a->h,
        (after.QuadPart-before.QuadPart)*1000.0/freq.QuadPart,(unsigned long long)mem.WorkingSetSize,(unsigned long long)mem.PrivateUsage,hash,a->error);
    // In GUI subsystem builds, write a report beside the test input.
    wchar_t target[32768]; swprintf(target,32768,L"%ls.brisa-test.txt",path);
    HANDLE f=CreateFileW(target,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f!=INVALID_HANDLE_VALUE) { char utf8[4096]; int len=WideCharToMultiByte(CP_UTF8,0,report,-1,utf8,sizeof(utf8),nullptr,nullptr); DWORD done; WriteFile(f,utf8,len-1,&done,nullptr); CloseHandle(f); }
    int count=a->count; dispose(a);
    // Exercise every fixture page and clamping before releasing document resources.
    if(ok) for(int n=0;n<count;n++) { r.page=n; Image* b=render(e,r,false); ok=ok && b->pixels && !b->error[0]; dispose(b); }
    e.close(); FPDF_DestroyLibrary(); return ok?0:1;
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE, PWSTR,int show) {
    int argc=0; wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc>=3 && !wcscmp(argv[1],L"--self-test")) { int result=selfTest(argv[2]); LocalFree(argv); return result; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput startup; Gdiplus::GdiplusStartup(&gdiplusToken,&startup,nullptr);
    dpiScale=GetDpiForSystem()/96.0; fonts();
    InitializeCriticalSection(&mutex); wakeEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    WNDCLASSW cls{}; cls.lpfnWndProc=windowProc; cls.hInstance=instance; cls.lpszClassName=L"BrisaPdfWindow";
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    RegisterClassW(&cls);
    menus=CreateMenu(); HMENU fileMenu=CreatePopupMenu(), viewMenu=CreatePopupMenu(), markMenu=CreatePopupMenu();
    AppendMenuW(fileMenu,MF_STRING,OPEN,L"Abrir…\tCtrl+O"); AppendMenuW(fileMenu,MF_STRING,SAVE,L"Guardar marcas\tCtrl+S");
    AppendMenuW(fileMenu,MF_SEPARATOR,0,nullptr); AppendMenuW(fileMenu,MF_STRING,EXIT,L"Salir");
    AppendMenuW(viewMenu,MF_STRING,FIT,L"Ajustar página\t0"); AppendMenuW(viewMenu,MF_STRING,PLUS,L"Acercar\t+");
    AppendMenuW(viewMenu,MF_STRING,MINUS,L"Alejar\t−"); AppendMenuW(viewMenu,MF_STRING,FULL,L"Pantalla completa\tF11");
    AppendMenuW(markMenu,MF_STRING|MF_CHECKED,HAND,L"Mover\tV"); AppendMenuW(markMenu,MF_STRING,MARKER,L"Subrayador\tH");
    AppendMenuW(markMenu,MF_STRING,PEN,L"Lápiz\tP"); AppendMenuW(markMenu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(markMenu,MF_STRING,UNDO,L"Deshacer trazo\tCtrl+Z");
    AppendMenuW(menus,MF_POPUP,(UINT_PTR)fileMenu,L"Archivo"); AppendMenuW(menus,MF_POPUP,(UINT_PTR)viewMenu,L"Vista");
    AppendMenuW(menus,MF_POPUP,(UINT_PTR)markMenu,L"Marcar");
    window=CreateWindowExW(WS_EX_ACCEPTFILES,cls.lpszClassName,L"Brisa",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,px(1050),px(820),nullptr,nullptr,instance,nullptr);
    if(!window || !wakeEvent) return 1;
    worker=CreateThread(nullptr,0,workerMain,nullptr,0,nullptr);
    if(!worker) return 1;
    if(argc>1) openPath(argv[1]); LocalFree(argv);
    ShowWindow(window,show); UpdateWindow(window);
    MSG msg; while(GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    InterlockedExchange(&stopping,1); SetEvent(wakeEvent); WaitForSingleObject(worker,INFINITE);
    // Dispose results posted immediately before shutdown.
    while(PeekMessageW(&msg,nullptr,READY,READY,PM_REMOVE)) dispose((Image*)msg.lParam);
    dispose(displayed); CloseHandle(worker); CloseHandle(wakeEvent); DeleteCriticalSection(&mutex);
    DeleteObject(uiFont); DeleteObject(headingFont);
    if(canvas) { SelectObject(canvas,originalBitmap); DeleteObject(canvasBitmap); DeleteDC(canvas); }
    DestroyMenu(menus); free(ink);
    Gdiplus::GdiplusShutdown(gdiplusToken); return 0;
}
