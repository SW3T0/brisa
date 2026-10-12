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

struct Glyph {
    float left;
    float top;
    float right;
    float bottom;
    unsigned codepoint;
    bool vertical = false;
};

struct RenderedPage {
    int page = 0;
    int count = 0;
    int width = 0;
    int height = 0;
    double scale = 1;
    std::vector<unsigned char> pixels;
    std::vector<Glyph> glyphs;
    std::wstring error;
};

struct RenderItem {
    std::wstring path;
    int page = 0;
    int width = 1;
    int height = 1;
    double zoom = 1;
    bool fit = true;
    bool shouldRender = true;
};

struct RenderRequest {
    LONG generation = 0;
    int panes = 0;
    RenderItem items[2];
};

struct RenderResult {
    LONG generation;
    RenderedPage* images[2]{};
};

struct PdfEngine {
    HANDLE file = INVALID_HANDLE_VALUE;
    FPDF_DOCUMENT document = nullptr;
    FPDF_FILEACCESS access{};
    wchar_t path[32768]{};

    void close() {
        if (document) {
            FPDF_CloseDocument(document);
        }
        document = nullptr;
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        file = INVALID_HANDLE_VALUE;
        path[0] = 0;
    }

    static int read(void* contextOrPath, unsigned long offset, unsigned char* data,
                    unsigned long size) {
        LARGE_INTEGER position;
        position.QuadPart = offset;
        DWORD bytesRead = 0;
        return SetFilePointerEx((HANDLE)contextOrPath, position, nullptr, FILE_BEGIN) &&
               ReadFile((HANDLE)contextOrPath, data, size, &bytesRead, nullptr) &&
               bytesRead == size;
    }

    bool open(const wchar_t* sourcePath, wchar_t* error) {
        close();
        file = CreateFileW(sourcePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER length{};
        if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &length)) {
            wcscpy(error, L"No se puede abrir este archivo.");
            close();
            return false;
        }
        if (length.QuadPart <= 0 || length.QuadPart > 0xffffffffLL) {
            wcscpy(error, L"Archivo vacío o mayor de 4 GB.");
            close();
            return false;
        }
        access = {(unsigned long)length.QuadPart, read, file};
        document = FPDF_LoadCustomDocument(&access, nullptr);
        if (!document) {
            wcscpy(error, FPDF_GetLastError() == FPDF_ERR_PASSWORD
                              ? L"Este PDF requiere contraseña. El MVP no admite contraseñas."
                              : L"No se puede leer el PDF. Puede estar dañado.");
            close();
            return false;
        }
        wcscpy(path, sourcePath);
        return true;
    }
};

// UI owns workspaces and annotations; worker owns all PDFium handles.
struct InkPoint {
    int page;
    int startsStroke;
    int tool;
    float x;
    float y;
};

struct Highlight {
    int page;
    int group;
    float left;
    float top;
    float right;
    float bottom;
};

struct InkHeader {
    char magic[8];
    unsigned count;
    DWORD sizeLow;
    DWORD sizeHigh;
    FILETIME modified;
};

struct Document {
    std::wstring path;
    std::vector<InkPoint> ink;
    std::vector<Highlight> highlights;
    bool dirty = false;
    int nextGroup = 1;
    int lastTool = 1;
};

struct ExportJob {
    Document document;
    std::wstring destination;
};

struct ExportResult {
    std::wstring destination;
    std::wstring error;
};

std::deque<ExportJob> exportQueue;
bool exportBusy = false;

struct Pane {
    int documentIndex = -1;
    int page = 0;
    int count = 0;
    int panX = 0;
    int panY = 0;
    double zoom = 1;
    bool fit = true;
    bool busy = false;
    std::unique_ptr<RenderedPage> image;
};

struct Workspace {
    Pane panes[2];
    int count = 1;
    int active = 0;
};

HWND window;
HMENU menus, workspaceMenu;
HFONT uiFont, headingFont, smallFont;
HCURSOR penCursor;
ULONG_PTR gdiplusToken;
HDC canvas = nullptr;
HBITMAP canvasBitmap = nullptr, originalBitmap = nullptr;
int canvasWidth = 0, canvasHeight = 0;
double dpiScale = 1;
bool menuVisible = false, sidebarVisible = false, fullscreen = false;
WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
std::vector<Document> documents;
std::vector<std::unique_ptr<Workspace>> workspaces;
int activeWorkspace = -1, tool = 0, tabOffset = 0;
bool dragging = false, drawing = false, selecting = false;
int dragX = 0, dragY = 0, interactionPane = -1, selectionStart = -1, selectionEnd = -1;
std::wstring toast;
CRITICAL_SECTION renderMutex;
HANDLE wakeEvent, renderWorker;
RenderRequest pendingRenderRequest;
volatile LONG generation = 0, stopping = 0;
constexpr unsigned POINT_LIMIT = 200000, HIGHLIGHT_LIMIT = 100000, WORKSPACE_MENU = 1000;

enum { OPEN = 100, EXIT, FIT, PLUS, MINUS, FULL, HAND, MARKER, PEN, UNDO, SAVE, SIDEBAR, SPLIT };

constexpr unsigned EXPORT_PDF = 113;

int scaledPixels(int value) {
    return (int)round(value * dpiScale);
}

Workspace* currentWorkspace() {
    return activeWorkspace >= 0 ? workspaces[activeWorkspace].get() : nullptr;
}

Pane* focusedPane() {
    auto* workspace = currentWorkspace();
    return workspace ? &workspace->panes[workspace->active] : nullptr;
}

const wchar_t* documentFilename(int documentIndex) {
    if (documentIndex < 0) {
        return L"";
    }
    const wchar_t* path = documents[documentIndex].path.c_str();
    const wchar_t* separator = wcsrchr(path, L'\\');
    return separator ? separator + 1 : path;
}

void repaint() {
    InvalidateRect(window, nullptr, FALSE);
}

void showNotification(const wchar_t* text) {
    toast = text;
    SetTimer(window, 2, 3600, nullptr);
    repaint();
}

RECT clientRect() {
    RECT r{};
    GetClientRect(window, &r);
    return r;
}

RECT paneArea(int paneIndex) {
    RECT area = clientRect();
    area.top = scaledPixels(44);
    if (sidebarVisible) {
        area.left = scaledPixels(58);
    }
    auto* workspace = currentWorkspace();
    if (workspace && workspace->count == 2) {
        int middle = area.left + (area.right - area.left) / 2;
        if (paneIndex == 0) {
            area.right = middle - scaledPixels(6);
        } else {
            area.left = middle + scaledPixels(6);
        }
    }
    return area;
}

RECT contentArea(int paneIndex) {
    RECT area = paneArea(paneIndex);
    area.top += scaledPixels(35);
    area.bottom -= scaledPixels(12);
    return area;
}

void clampPan(Pane& pane, RECT area) {
    if (!pane.image) {
        return;
    }
    int maxPanX =
        std::max<int>(0, (pane.image->width - (area.right - area.left) + scaledPixels(32)) / 2);
    int maxPanY =
        std::max<int>(0, (pane.image->height - (area.bottom - area.top) + scaledPixels(32)) / 2);
    pane.panX = std::clamp(pane.panX, -maxPanX, maxPanX);
    pane.panY = std::clamp(pane.panY, -maxPanY, maxPanY);
}

RECT pageRect(int paneIndex) {
    RECT area = contentArea(paneIndex);
    auto* workspace = currentWorkspace();
    if (!workspace || !workspace->panes[paneIndex].image) {
        return {};
    }
    Pane& pane = workspace->panes[paneIndex];
    clampPan(pane, area);
    int x = area.left + (area.right - area.left - pane.image->width) / 2 + pane.panX;
    int y = area.top + (area.bottom - area.top - pane.image->height) / 2 + pane.panY;
    return {x, y, x + pane.image->width, y + pane.image->height};
}

int paneAt(int x, int y) {
    auto* workspace = currentWorkspace();
    if (!workspace) {
        return -1;
    }
    for (int paneIndex = 0; paneIndex < workspace->count; paneIndex++) {
        RECT area = paneArea(paneIndex);
        if (PtInRect(&area, {x, y})) {
            return paneIndex;
        }
    }
    return -1;
}

void fillRect(HDC dc, RECT rectangle, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rectangle, brush);
    DeleteObject(brush);
}

void drawLabel(HDC dc, const wchar_t* text, RECT rectangle, COLORREF color,
               UINT flags = DT_SINGLELINE | DT_VCENTER | DT_CENTER) {
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rectangle, flags | DT_NOPREFIX);
}

void updateTitle() {
    auto* workspace = currentWorkspace();
    std::wstring title = L"Brisa";
    if (workspace) {
        title += L" — ";
        title += documentFilename(workspace->panes[0].documentIndex);
        if (workspace->count == 2) {
            title += L" + ";
            title += documentFilename(workspace->panes[1].documentIndex);
        }
        title += L" · " + std::to_wstring(activeWorkspace + 1) + L"/" +
                 std::to_wstring(workspaces.size());
        Pane& pane = workspace->panes[workspace->active];
        title += L" · " + std::to_wstring(pane.page + 1) + L"/" + std::to_wstring(pane.count);
    }
    SetWindowTextW(window, title.c_str());
}

bool saveDocument(int documentIndex) {
    Document& document = documents[documentIndex];
    if (!document.dirty) {
        return true;
    }
    std::wstring path = document.path + L".brisa-ink", temporaryPath = path + L".tmp";
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(document.path.c_str(), GetFileExInfoStandard, &attributes)) {
        return false;
    }
    InkHeader header{{'B', 'R', 'I', 'S', 'A', '0', '2', 0},
                     (unsigned)document.ink.size(),
                     attributes.nFileSizeLow,
                     attributes.nFileSizeHigh,
                     attributes.ftLastWriteTime};
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    auto write = [&](const void* p, DWORD size) {
        DWORD done = 0;
        return WriteFile(file, p, size, &done, nullptr) && done == size;
    };
    unsigned count = (unsigned)document.highlights.size();
    bool ok = write(&header, sizeof(header)) &&
              write(document.ink.data(), header.count * sizeof(InkPoint)) &&
              write(&count, sizeof(count)) &&
              write(document.highlights.data(), count * sizeof(Highlight)) &&
              FlushFileBuffers(file);
    CloseHandle(file);
    ok = ok && MoveFileExW(temporaryPath.c_str(), path.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (ok) {
        document.dirty = false;
    } else {
        DeleteFileW(temporaryPath.c_str());
    }
    return ok;
}

void loadDocument(Document& document) {
    HANDLE file =
        CreateFileW((document.path + L".brisa-ink").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    auto read = [&](void* point, DWORD size) {
        DWORD got = 0;
        return ReadFile(file, point, size, &got, nullptr) && got == size;
    };
    InkHeader header{};
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    bool ok = read(&header, sizeof(header)) &&
              (!memcmp(header.magic, "BRISA01", 8) || !memcmp(header.magic, "BRISA02", 8)) &&
              header.count <= POINT_LIMIT;
    ok = ok && GetFileAttributesExW(document.path.c_str(), GetFileExInfoStandard, &attributes) &&
         header.sizeLow == attributes.nFileSizeLow && header.sizeHigh == attributes.nFileSizeHigh &&
         CompareFileTime(&header.modified, &attributes.ftLastWriteTime) == 0;
    if (ok) {
        document.ink.resize(header.count);
        ok = read(document.ink.data(), header.count * sizeof(InkPoint));
        for (auto& point : document.ink) {
            ok = ok && point.page >= 0 && (point.startsStroke == 0 || point.startsStroke == 1) &&
                 (point.tool == 1 || point.tool == 2) && isfinite(point.x) && isfinite(point.y) &&
                 point.x >= 0 && point.x <= 1 && point.y >= 0 && point.y <= 1;
        }
        if (ok && !memcmp(header.magic, "BRISA02", 8)) {
            unsigned count = 0;
            ok = read(&count, sizeof(count)) && count <= HIGHLIGHT_LIMIT;
            if (ok) {
                document.highlights.resize(count);
                ok = read(document.highlights.data(), count * sizeof(Highlight));
            }
            for (auto& highlight : document.highlights) {
                ok = ok && highlight.page >= 0 && highlight.group > 0 &&
                     highlight.group < 100000000 && isfinite(highlight.left) &&
                     isfinite(highlight.top) && isfinite(highlight.right) &&
                     isfinite(highlight.bottom) && highlight.left >= 0 && highlight.top >= 0 &&
                     highlight.right <= 1 && highlight.bottom <= 1 &&
                     highlight.left < highlight.right && highlight.top < highlight.bottom;
                document.nextGroup = std::max(document.nextGroup, highlight.group + 1);
            }
        }
    }
    CloseHandle(file);
    if (!ok) {
        document.ink.clear();
        document.highlights.clear();
        document.nextGroup = 1;
    }
    if (document.highlights.empty() && !document.ink.empty()) {
        document.lastTool = 2;
    }
}

bool saveAll(bool closing = false) {
    bool ok = true;
    for (int n = 0; n < (int)documents.size(); n++) {
        if (!saveDocument(n)) {
            ok = false;
        }
    }
    if (!ok && closing) {
        return MessageBoxW(window, L"No se pudieron guardar algunas marcas. ¿Salir y descartarlas?",
                           L"Brisa", MB_YESNO | MB_ICONWARNING) == IDYES;
    }
    if (!ok) {
        showNotification(
            L"No se pudieron guardar las marcas. Comprueba los permisos de la carpeta.");
    }
    return ok;
}

bool isRenderStale(LONG requestedGeneration) {
    return InterlockedCompareExchange(&stopping, 0, 0) ||
           InterlockedCompareExchange(&generation, 0, 0) != requestedGeneration;
}

int pauseRender(IFSDK_PAUSE* pause) {
    return isRenderStale(*(LONG*)pause->user);
}

RenderedPage* renderPage(PdfEngine& engine, const RenderItem& item, LONG generation,
                         bool cancellable = true) {
    auto image = std::make_unique<RenderedPage>();
    wchar_t error[160]{};
    if ((!engine.document || wcscmp(engine.path, item.path.c_str())) &&
        !engine.open(item.path.c_str(), error)) {
        image->error = error;
        return image.release();
    }
    image->count = FPDF_GetPageCount(engine.document);
    if (image->count < 1) {
        image->error = L"El PDF no contiene páginas.";
        return image.release();
    }
    image->page = std::clamp(item.page, 0, image->count - 1);
    FPDF_PAGE page = FPDF_LoadPage(engine.document, image->page);
    if (!page) {
        image->error = L"No se puede cargar esta página.";
        return image.release();
    }
    double pageWidth = FPDF_GetPageWidth(page), pageHeight = FPDF_GetPageHeight(page);
    if (!isfinite(pageWidth) || !isfinite(pageHeight) || pageWidth <= 0 || pageHeight <= 0) {
        image->error = L"Tamaño de página inválido.";
        FPDF_ClosePage(page);
        return image.release();
    }
    double scale =
        item.fit ? std::min(item.width / pageWidth, item.height / pageHeight) : item.zoom;
    scale =
        std::min(std::max(.02, scale), std::min(sqrt(PIXEL_LIMIT / (pageWidth * pageHeight)),
                                                std::min(16000 / pageWidth, 16000 / pageHeight)));
    image->width = std::max<int>(1, (int)floor(pageWidth * scale));
    image->height = std::max<int>(1, (int)floor(pageHeight * scale));
    image->scale = scale;
    image->pixels.resize((size_t)image->width * image->height * 4);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(image->width, image->height, FPDFBitmap_BGRx,
                                             image->pixels.data(), image->width * 4);
    if (!bitmap) {
        image->error = L"No hay memoria suficiente para renderizar.";
    } else {
        FPDFBitmap_FillRect(bitmap, 0, 0, image->width, image->height, 0xffffffff);
        if (cancellable) {
            IFSDK_PAUSE pause{1, pauseRender, &generation};
            int status = FPDF_RenderPageBitmap_Start(bitmap, page, 0, 0, image->width,
                                                     image->height, 0, FPDF_ANNOT, &pause);
            while (status == FPDF_RENDER_TOBECONTINUED && !isRenderStale(generation)) {
                status = FPDF_RenderPage_Continue(page, &pause);
            }
            FPDF_RenderPage_Close(page);
            if (status == FPDF_RENDER_FAILED) {
                image->error = L"No se puede renderizar esta página.";
            }
        } else {
            FPDF_RenderPageBitmap(bitmap, page, 0, 0, image->width, image->height, 0, FPDF_ANNOT);
        }
        FPDFBitmap_Destroy(bitmap);
    }
    if (!cancellable || !isRenderStale(generation)) {
        FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
        if (text) {
            int count = std::clamp(FPDFText_CountChars(text), 0, 200000);
            image->glyphs.resize(count);
            for (int n = 0; n < count; n++) {
                if (cancellable && n % 256 == 0 && isRenderStale(generation)) {
                    break;
                }
                auto& glyph = image->glyphs[n];
                glyph.codepoint = FPDFText_GetUnicode(text, n);
                double l, item, b, t;
                if (glyph.codepoint < 32 || !FPDFText_GetCharBox(text, n, &l, &item, &b, &t) ||
                    item <= l || t <= b) {
                    continue;
                }
                int xs[4], ys[4];
                double xx[4] = {l, item, item, l}, yy[4] = {t, t, b, b};
                for (int k = 0; k < 4; k++) {
                    FPDF_PageToDevice(page, 0, 0, image->width, image->height, 0, xx[k], yy[k],
                                      &xs[k], &ys[k]);
                }
                bool pageVertical = abs(ys[1] - ys[0]) > abs(xs[1] - xs[0]);
                glyph.vertical = pageVertical != (fabs(cos(FPDFText_GetCharAngle(text, n))) < .5);
                glyph.left =
                    std::clamp(*std::min_element(xs, xs + 4) / (float)image->width, 0.f, 1.f);
                glyph.right =
                    std::clamp(*std::max_element(xs, xs + 4) / (float)image->width, 0.f, 1.f);
                glyph.top =
                    std::clamp(*std::min_element(ys, ys + 4) / (float)image->height, 0.f, 1.f);
                glyph.bottom =
                    std::clamp(*std::max_element(ys, ys + 4) / (float)image->height, 0.f, 1.f);
            }
            FPDFText_ClosePage(text);
        }
    }
    FPDF_ClosePage(page);
    return image.release();
}

void disposeRenderResult(RenderResult* result) {
    if (result) {
        delete result->images[0];
        delete result->images[1];
        delete result;
    }
}

// Export opens its own document on the same worker thread. Rendering caches and
// the original file stay untouched, even if export is cancelled or fails.
bool isSameFile(const std::wstring& first, const std::wstring& second) {
    if (!_wcsicmp(first.c_str(), second.c_str())) {
        return true;
    }
    HANDLE a = CreateFileW(first.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE b =
        CreateFileW(second.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    BY_HANDLE_FILE_INFORMATION x{}, y{};
    bool same = a != INVALID_HANDLE_VALUE && b != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandle(a, &x) && GetFileInformationByHandle(b, &y) &&
                x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
                x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
    if (a != INVALID_HANDLE_VALUE) {
        CloseHandle(a);
    }
    if (b != INVALID_HANDLE_VALUE) {
        CloseHandle(b);
    }
    return same;
}

FS_POINTF pdfPoint(FPDF_PAGE page, float x, float y) {
    double pdfX = 0, pdfY = 0;
    FPDF_DeviceToPage(page, 0, 0, 1000000, 1000000, 0, (int)round(x * 1000000),
                      (int)round(y * 1000000), &pdfX, &pdfY);
    return {(float)pdfX, (float)pdfY};
}

FS_RECTF pointBounds(const std::vector<FS_POINTF>& points, float padding = 0) {
    FS_RECTF rectangle{points[0].x, points[0].y, points[0].x, points[0].y};
    for (auto point : points) {
        rectangle.left = std::min(rectangle.left, point.x);
        rectangle.right = std::max(rectangle.right, point.x);
        rectangle.top = std::max(rectangle.top, point.y);
        rectangle.bottom = std::min(rectangle.bottom, point.y);
    }
    rectangle.left -= padding;
    rectangle.right += padding;
    rectangle.top += padding;
    rectangle.bottom -= padding;
    return rectangle;
}

bool embedHighlights(FPDF_PAGE page, int pageIndex, const Document& document) {
    bool ok = true;
    for (size_t highlightIndex = 0; highlightIndex < document.highlights.size() && ok;) {
        const auto& highlight = document.highlights[highlightIndex];
        if (highlight.page != pageIndex) {
            highlightIndex++;
            continue;
        }
        int group = highlight.group;
        FPDF_ANNOTATION annotation = FPDFPage_CreateAnnot(page, FPDF_ANNOT_HIGHLIGHT);
        if (!annotation) {
            ok = false;
            break;
        }
        std::vector<FS_POINTF> corners;
        ok = FPDFAnnot_SetColor(annotation, FPDFANNOT_COLORTYPE_Color, 250, 201, 38, 90) &&
             FPDFAnnot_SetFlags(annotation, FPDF_ANNOT_FLAG_PRINT);
        do {
            const auto& rectangle = document.highlights[highlightIndex];
            auto topLeft = pdfPoint(page, rectangle.left, rectangle.top),
                 topRight = pdfPoint(page, rectangle.right, rectangle.top),
                 bottomLeft = pdfPoint(page, rectangle.left, rectangle.bottom),
                 bottomRight = pdfPoint(page, rectangle.right, rectangle.bottom);
            FS_QUADPOINTSF quad{topLeft.x,    topLeft.y,    topRight.x,    topRight.y,
                                bottomLeft.x, bottomLeft.y, bottomRight.x, bottomRight.y};
            ok = ok && FPDFAnnot_AppendAttachmentPoints(annotation, &quad);
            corners.insert(corners.end(), {topLeft, topRight, bottomLeft, bottomRight});
            highlightIndex++;
        } while (highlightIndex < document.highlights.size() &&
                 document.highlights[highlightIndex].page == pageIndex &&
                 document.highlights[highlightIndex].group == group);
        auto rect = pointBounds(corners);
        ok = ok && FPDFAnnot_SetRect(annotation, &rect);
        FPDFPage_CloseAnnot(annotation);
    }
    return ok;
}

bool embedInkStrokes(FPDF_PAGE page, int pageIndex, const Document& document) {
    bool ok = true;
    for (size_t strokeStart = 0; strokeStart < document.ink.size() && ok;) {
        const auto& firstPoint = document.ink[strokeStart];
        size_t strokeEnd = strokeStart + 1;
        while (strokeEnd < document.ink.size() && !document.ink[strokeEnd].startsStroke &&
               document.ink[strokeEnd].page == firstPoint.page) {
            strokeEnd++;
        }
        if (firstPoint.page != pageIndex) {
            strokeStart = strokeEnd;
            continue;
        }
        std::vector<FS_POINTF> points;
        points.reserve(strokeEnd - strokeStart + 1);
        for (size_t pointIndex = strokeStart; pointIndex < strokeEnd; pointIndex++) {
            points.push_back(
                pdfPoint(page, document.ink[pointIndex].x, document.ink[pointIndex].y));
        }
        if (points.size() == 1) {
            points.push_back({points[0].x + .01f, points[0].y});
        }
        float width = firstPoint.tool == 1 ? 12.f : 1.6f;
        unsigned alpha = firstPoint.tool == 1 ? 68 : 230;
        unsigned red = firstPoint.tool == 1 ? 247 : 48, green = firstPoint.tool == 1 ? 201 : 74,
                 blue = firstPoint.tool == 1 ? 49 : 61;
        FPDF_ANNOTATION annotation = FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK);
        if (!annotation) {
            ok = false;
            break;
        }
        auto rect = pointBounds(points, width);
        ok = FPDFAnnot_SetRect(annotation, &rect) && FPDFAnnot_SetBorder(annotation, 0, 0, width) &&
             FPDFAnnot_SetColor(annotation, FPDFANNOT_COLORTYPE_Color, red, green, blue, alpha) &&
             FPDFAnnot_SetFlags(annotation, FPDF_ANNOT_FLAG_PRINT) &&
             FPDFAnnot_AddInkStroke(annotation, points.data(), points.size()) >= 0;
        // Explicit appearance path also renders in readers that do not
        // synthesize an InkList appearance on opening the file.
        FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(points[0].x, points[0].y);
        if (!path) {
            ok = false;
        } else {
            bool appearance = FPDFPageObj_SetStrokeColor(path, red, green, blue, alpha) &&
                              FPDFPageObj_SetStrokeWidth(path, width) &&
                              FPDFPath_SetDrawMode(path, FPDF_FILLMODE_NONE, 1);
            for (size_t pointIndex = 1; pointIndex < points.size(); pointIndex++) {
                appearance =
                    appearance && FPDFPath_LineTo(path, points[pointIndex].x, points[pointIndex].y);
            }
            if (!ok || !appearance || !FPDFAnnot_AppendObject(annotation, path)) {
                FPDFPageObj_Destroy(path);
                ok = false;
            }
        }
        FPDFPage_CloseAnnot(annotation);
        strokeStart = strokeEnd;
    }
    return ok;
}

bool embedMarks(FPDF_DOCUMENT pdf, const Document& document) {
    int pageCount = FPDF_GetPageCount(pdf);
    // Visit only marked pages, rather than loading a large document in full.
    std::vector<int> markedPages;
    for (auto highlight : document.highlights) {
        markedPages.push_back(highlight.page);
    }
    for (auto point : document.ink) {
        if (point.startsStroke) {
            markedPages.push_back(point.page);
        }
    }
    std::sort(markedPages.begin(), markedPages.end());
    markedPages.erase(std::unique(markedPages.begin(), markedPages.end()), markedPages.end());
    for (int pageIndex : markedPages) {
        if (pageIndex < 0 || pageIndex >= pageCount) {
            return false;
        }
        FPDF_PAGE page = FPDF_LoadPage(pdf, pageIndex);
        if (!page) {
            return false;
        }
        bool ok = embedHighlights(page, pageIndex, document) &&
                  embedInkStrokes(page, pageIndex, document);
        if (ok) {
            // PDFium's annotation render pass promotes new dictionaries to
            // indirect objects and generates missing appearances. A 1px
            // target prepares interoperable annotations without a page image.
            FPDF_BITMAP bitmap = FPDFBitmap_Create(1, 1, 0);
            if (!bitmap) {
                ok = false;
            } else {
                FPDF_RenderPageBitmap(bitmap, page, 0, 0, 1, 1, 0, FPDF_ANNOT);
                FPDFBitmap_Destroy(bitmap);
            }
        }
        FPDF_ClosePage(page);
        if (!ok) {
            return false;
        }
    }
    return true;
}

struct PdfWriter {
    FPDF_FILEWRITE api{1, write};
    HANDLE file = INVALID_HANDLE_VALUE;

    static int write(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
        auto* writer = reinterpret_cast<PdfWriter*>(self);
        DWORD done = 0;
        return WriteFile(writer->file, data, size, &done, nullptr) && done == size;
    }
};

ExportResult* exportPdf(const ExportJob& job) {
    auto result = std::make_unique<ExportResult>();
    result->destination = job.destination;
    if (isSameFile(job.document.path, job.destination)) {
        result->error = L"Elige otro archivo para conservar el PDF original.";
        return result.release();
    }
    PdfEngine engine;
    wchar_t error[160]{};
    if (!engine.open(job.document.path.c_str(), error)) {
        result->error = error;
        return result.release();
    }
    if (!embedMarks(engine.document, job.document)) {
        result->error = L"No se pudieron incorporar las anotaciones al PDF.";
        engine.close();
        return result.release();
    }
    // Create a unique temporary file in the destination directory, then rename
    // it atomically: a failed export never truncates an existing destination.
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int n = 0; n < 20 && file == INVALID_HANDLE_VALUE; n++) {
        temporary = job.destination + L".brisa-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(n) + L".tmp";
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) {
        result->error = L"No se puede escribir en la carpeta de destino.";
    } else {
        PdfWriter writer;
        writer.file = file;
        bool ok = FPDF_SaveAsCopy(engine.document, &writer.api, FPDF_NO_INCREMENTAL) &&
                  FlushFileBuffers(file);
        CloseHandle(file);
        engine.close();
        if (ok) {
            ok = MoveFileExW(temporary.c_str(), job.destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        if (!ok) {
            DeleteFileW(temporary.c_str());
            result->error = L"No se pudo guardar el PDF. Comprueba los permisos o si está abierto "
                            L"en otro programa.";
        }
    }
    engine.close();
    return result.release();
}

DWORD WINAPI workerMain(void*) {
    FPDF_InitLibrary();
    PdfEngine engines[2];
    while (true) {
        WaitForSingleObject(wakeEvent, INFINITE);
        if (InterlockedCompareExchange(&stopping, 0, 0)) {
            break;
        }
        EnterCriticalSection(&renderMutex);
        RenderRequest request = pendingRenderRequest;
        std::unique_ptr<ExportJob> job;
        if (!exportQueue.empty()) {
            job = std::make_unique<ExportJob>(std::move(exportQueue.front()));
            exportQueue.pop_front();
        }
        LeaveCriticalSection(&renderMutex);
        if (job) {
            auto* outcome = exportPdf(*job);
            if (!PostMessageW(window, EXPORT_READY, 0, (LPARAM)outcome)) {
                delete outcome;
            }
        }
        auto result = new RenderResult{};
        result->generation = request.generation;
        for (int paneIndex = 0; paneIndex < request.panes && !isRenderStale(request.generation);
             paneIndex++) {
            if (request.items[paneIndex].shouldRender) {
                result->images[paneIndex] =
                    renderPage(engines[paneIndex], request.items[paneIndex], request.generation);
            }
        }
        if (request.panes < 1) {
            engines[0].close();
        }
        if (request.panes < 2) {
            engines[1].close();
        }
        if (isRenderStale(request.generation) || !PostMessageW(window, READY, 0, (LPARAM)result)) {
            disposeRenderResult(result);
        }
    }
    engines[0].close();
    engines[1].close();
    FPDF_DestroyLibrary();
    return 0;
}

void requestRender(int requestedPane = -1) {
    auto* workspace = currentWorkspace();
    RenderRequest request;
    request.generation = InterlockedIncrement(&generation);
    request.panes = workspace ? workspace->count : 0;
    for (int paneIndex = 0; paneIndex < request.panes; paneIndex++) {
        Pane& pane = workspace->panes[paneIndex];
        RECT area = contentArea(paneIndex);
        RenderItem& item = request.items[paneIndex];
        item.path = documents[pane.documentIndex].path;
        item.page = pane.page;
        item.fit = pane.fit;
        item.zoom = pane.zoom;
        item.width = std::max<int>(1, area.right - area.left - scaledPixels(32));
        item.height = std::max<int>(1, area.bottom - area.top - scaledPixels(32));
        item.shouldRender =
            requestedPane < 0 || requestedPane == paneIndex || pane.busy || !pane.image;
        if (item.shouldRender) {
            pane.busy = true;
        }
    }
    EnterCriticalSection(&renderMutex);
    pendingRenderRequest = std::move(request);
    LeaveCriticalSection(&renderMutex);
    SetEvent(wakeEvent);
    repaint();
}

void finishInteraction(bool commit = true);

std::wstring workspaceName(int index) {
    auto& w = *workspaces[index];
    std::wstring s = documentFilename(w.panes[0].documentIndex);
    if (w.count == 2) {
        s += L" + ";
        s += documentFilename(w.panes[1].documentIndex);
    }
    return s;
}

int tabWidth() {
    int available = clientRect().right - scaledPixels(124);
    return std::clamp(available / std::max<int>(1, (int)workspaces.size()), scaledPixels(142),
                      scaledPixels(240));
}

int visibleTabs() {
    return std::max<int>(1, (clientRect().right - scaledPixels(124)) / tabWidth());
}

RECT tabRect(int slot) {
    int x = scaledPixels(44) + (slot - tabOffset) * tabWidth();
    return {x, scaledPixels(5), x + tabWidth() - scaledPixels(4), scaledPixels(40)};
}

void ensureVisible() {
    if (activeWorkspace < tabOffset) {
        tabOffset = std::max<int>(0, activeWorkspace);
    }
    if (activeWorkspace >= tabOffset + visibleTabs()) {
        tabOffset = activeWorkspace - visibleTabs() + 1;
    }
    tabOffset = std::clamp(tabOffset, 0, std::max<int>(0, (int)workspaces.size() - visibleTabs()));
}

void refreshWorkspaceMenu() {
    while (GetMenuItemCount(workspaceMenu) > 0) {
        DeleteMenu(workspaceMenu, 0, MF_BYPOSITION);
    }
    for (int n = 0; n < (int)workspaces.size(); n++) {
        std::wstring name = workspaceName(n), escaped;
        for (wchar_t c : name) {
            escaped += c;
            if (c == L'&') {
                escaped += L'&';
            }
        }
        if (n < 9) {
            escaped += L"\tCtrl+" + std::to_wstring(n + 1);
        }
        AppendMenuW(workspaceMenu, MF_STRING | (n == activeWorkspace ? MF_CHECKED : 0),
                    WORKSPACE_MENU + n, escaped.c_str());
    }
}

void switchWorkspace(int slot) {
    if (slot < 0 || slot >= (int)workspaces.size() || slot == activeWorkspace) {
        return;
    }
    finishInteraction();
    saveAll();
    if (auto* old = currentWorkspace()) {
        for (int n = 0; n < old->count; n++) {
            old->panes[n].image.reset();
            old->panes[n].busy = false;
        }
    }
    activeWorkspace = slot;
    ensureVisible();
    refreshWorkspaceMenu();
    updateTitle();
    requestRender();
}

Pane copyState(const Pane& p) {
    Pane next;
    next.documentIndex = p.documentIndex;
    next.page = p.page;
    next.count = p.count;
    next.panX = p.panX;
    next.panY = p.panY;
    next.zoom = p.zoom;
    next.fit = p.fit;
    return next;
}

void openPath(const wchar_t* path) {
    wchar_t absolute[32768];
    DWORD len = GetFullPathNameW(path, 32768, absolute, nullptr);
    if (!len || len >= 32768) {
        return;
    }
    int doc = -1;
    for (int n = 0; n < (int)documents.size(); n++) {
        if (!_wcsicmp(documents[n].path.c_str(), absolute)) {
            doc = n;
            break;
        }
    }
    if (doc < 0) {
        if (documents.size() >= 64) {
            showNotification(L"Límite de 64 documentos por sesión.");
            return;
        }
        Document d;
        d.path = absolute;
        loadDocument(d);
        documents.push_back(std::move(d));
        doc = (int)documents.size() - 1;
    }
    for (int n = 0; n < (int)workspaces.size(); n++) {
        if (workspaces[n]->count == 1 && workspaces[n]->panes[0].documentIndex == doc) {
            switchWorkspace(n);
            return;
        }
    }
    if (workspaces.size() >= 128) {
        showNotification(L"Límite de 128 workspaces por sesión.");
        return;
    }
    auto w = std::make_unique<Workspace>();
    w->panes[0].documentIndex = doc;
    workspaces.push_back(std::move(w));
    switchWorkspace((int)workspaces.size() - 1);
}

void openDialog() {
    wchar_t paths[32768]{};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = window;
    ofn.lpstrFilter = L"Documentos PDF\0*.pdf\0Todos los archivos\0*.*\0";
    ofn.lpstrFile = paths;
    ofn.nMaxFile = 32768;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT |
                OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }
    const wchar_t* name = paths + wcslen(paths) + 1;
    if (!*name) {
        openPath(paths);
    } else {
        for (; *name; name += wcslen(name) + 1) {
            std::wstring full = std::wstring(paths) + L"\\" + name;
            openPath(full.c_str());
        }
    }
}

void beginExport(const std::wstring& destination) {
    if (exportBusy) {
        showNotification(L"Espera a que termine el guardado del PDF.");
        return;
    }
    finishInteraction();
    auto* pane = focusedPane();
    if (!pane) {
        return;
    }
    for (auto& document : documents) {
        if (isSameFile(document.path, destination)) {
            showNotification(L"Elige una copia nueva para conservar el PDF original.");
            return;
        }
    }
    ExportJob job{documents[pane->documentIndex], destination};
    EnterCriticalSection(&renderMutex);
    exportQueue.push_back(std::move(job));
    LeaveCriticalSection(&renderMutex);
    exportBusy = true;
    SetEvent(wakeEvent);
    showNotification(L"Guardando PDF anotado…");
}

void exportDialog() {
    auto* pane = focusedPane();
    if (!pane) {
        return;
    }
    if (exportBusy) {
        showNotification(L"Espera a que termine el guardado del PDF.");
        return;
    }
    std::wstring name = documents[pane->documentIndex].path;
    auto dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        name.resize(dot);
    }
    name += L"-anotado.pdf";
    wchar_t path[32768]{};
    wcsncpy(path, name.c_str(), 32767);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = window;
    ofn.lpstrFilter = L"Documento PDF\0*.pdf\0";
    ofn.lpstrDefExt = L"pdf";
    ofn.lpstrFile = path;
    ofn.nMaxFile = 32768;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        beginExport(path);
    }
}

void createPair(int other) {
    auto* current = currentWorkspace();
    if (!current || other < 0 || other >= (int)documents.size()) {
        return;
    }
    int first = current->panes[current->active].documentIndex;
    if (first == other) {
        return;
    }
    for (int n = 0; n < (int)workspaces.size(); n++) {
        if (workspaces[n]->count == 2 && workspaces[n]->panes[0].documentIndex == first &&
            workspaces[n]->panes[1].documentIndex == other) {
            switchWorkspace(n);
            return;
        }
    }
    if (workspaces.size() >= 128) {
        showNotification(L"Límite de 128 workspaces por sesión.");
        return;
    }
    auto w = std::make_unique<Workspace>();
    w->count = 2;
    w->panes[0] = copyState(current->panes[current->active]);
    w->panes[1].documentIndex = other;
    for (auto& solo : workspaces) {
        if (solo->count == 1 && solo->panes[0].documentIndex == other) {
            w->panes[1] = copyState(solo->panes[0]);
            break;
        }
    }
    for (auto& pane : w->panes) {
        pane.fit = true;
        pane.panX = pane.panY = 0;
    }
    workspaces.push_back(std::move(w));
    switchWorkspace((int)workspaces.size() - 1);
}

void pairMenu() {
    if (!currentWorkspace() || documents.size() < 2) {
        showNotification(L"Abre un segundo PDF con Ctrl+O para crear una vista doble.");
        return;
    }
    HMENU popup = CreatePopupMenu();
    int first = focusedPane()->documentIndex;
    AppendMenuW(popup, MF_STRING | MF_DISABLED, 0, L"Mostrar al lado de este PDF");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    for (int n = 0; n < (int)documents.size(); n++) {
        if (n != first) {
            std::wstring escaped;
            for (const wchar_t* p = documentFilename(n); *p; p++) {
                escaped += *p;
                if (*p == L'&') {
                    escaped += *p;
                }
            }
            AppendMenuW(popup, MF_STRING, 3000 + n, escaped.c_str());
        }
    }
    POINT pt{clientRect().right - scaledPixels(78), scaledPixels(44)};
    ClientToScreen(window, &pt);
    UINT choice =
        TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTALIGN, pt.x, pt.y, 0, window, nullptr);
    DestroyMenu(popup);
    if (choice >= 3000) {
        createPair(choice - 3000);
    }
}

void closeWorkspace(int slot) {
    if (slot < 0 || slot >= (int)workspaces.size()) {
        return;
    }
    finishInteraction();
    saveAll();
    bool current = slot == activeWorkspace;
    workspaces.erase(workspaces.begin() + slot);
    if (current) {
        activeWorkspace = -1;
        InterlockedIncrement(&generation);
        if (!workspaces.empty()) {
            switchWorkspace(std::min(slot, (int)workspaces.size() - 1));
        } else {
            ensureVisible();
            updateTitle();
            refreshWorkspaceMenu();
            requestRender();
            repaint();
        }
    } else {
        if (slot < activeWorkspace) {
            activeWorkspace--;
        }
        ensureVisible();
        updateTitle();
        refreshWorkspaceMenu();
        repaint();
    }
}

void setActivePane(int n) {
    auto* w = currentWorkspace();
    if (!w || n < 0 || n >= w->count) {
        return;
    }
    w->active = n;
    updateTitle();
    repaint();
}

void goToPage(int nextPage, int paneIndex = -1) {
    auto* workspace = currentWorkspace();
    if (!workspace) {
        return;
    }
    if (paneIndex < 0) {
        paneIndex = workspace->active;
    }
    Pane& targetPane = workspace->panes[paneIndex];
    if (targetPane.count < 1) {
        return;
    }
    nextPage = std::clamp(nextPage, 0, targetPane.count - 1);
    if (nextPage == targetPane.page) {
        return;
    }
    finishInteraction();
    targetPane.page = nextPage;
    targetPane.panX = targetPane.panY = 0;
    requestRender(paneIndex);
    updateTitle();
}

void zoomBy(double factor, int paneIndex = -1) {
    auto* workspace = currentWorkspace();
    if (!workspace) {
        return;
    }
    if (paneIndex < 0) {
        paneIndex = workspace->active;
    }
    Pane& targetPane = workspace->panes[paneIndex];
    finishInteraction();
    targetPane.zoom =
        targetPane.fit && targetPane.image ? targetPane.image->scale : targetPane.zoom;
    targetPane.zoom = std::clamp(targetPane.zoom * factor, .1, 8.0);
    targetPane.fit = false;
    targetPane.panX = targetPane.panY = 0;
    requestRender(paneIndex);
}

void fitPage() {
    if (auto* p = focusedPane()) {
        finishInteraction();
        p->fit = true;
        p->panX = p->panY = 0;
        requestRender(currentWorkspace()->active);
    }
}

void toggleSidebar() {
    finishInteraction();
    sidebarVisible = !sidebarVisible;
    requestRender();
    repaint();
}

void setTool(int next) {
    finishInteraction();
    tool = next;
    CheckMenuRadioItem(GetSubMenu(menus, 2), HAND, PEN, HAND + tool, MF_BYCOMMAND);
    SendMessageW(window, WM_SETCURSOR, (WPARAM)window, HTCLIENT);
    repaint();
}

int glyphAt(int paneIndex, int x, int y, bool strict) {
    Pane& pane = currentWorkspace()->panes[paneIndex];
    if (!pane.image || pane.busy || !pane.image->error.empty()) {
        return -1;
    }
    RECT rect = pageRect(paneIndex);
    float normalizedX = (x - rect.left) / (float)pane.image->width,
          normalizedY = (y - rect.top) / (float)pane.image->height;
    double bestDistance = std::numeric_limits<double>::max();
    int closestGlyph = -1;
    for (int glyphIndex = 0; glyphIndex < (int)pane.image->glyphs.size(); glyphIndex++) {
        const Glyph& glyph = pane.image->glyphs[glyphIndex];
        if (glyph.codepoint < 32 || glyph.left >= glyph.right || glyph.top >= glyph.bottom) {
            continue;
        }
        double dx = std::max({glyph.left - normalizedX, 0.f, normalizedX - glyph.right}) *
                    pane.image->width,
               dy = std::max({glyph.top - normalizedY, 0.f, normalizedY - glyph.bottom}) *
                    pane.image->height;
        double distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            closestGlyph = glyphIndex;
        }
    }
    return strict && bestDistance > scaledPixels(8) * scaledPixels(8) ? -1 : closestGlyph;
}

std::vector<Highlight> selectionRectangles(int paneIndex) {
    std::vector<Highlight> rectangles;
    auto* workspace = currentWorkspace();
    if (!workspace || selectionStart < 0 || selectionEnd < 0) {
        return rectangles;
    }
    Pane& pane = workspace->panes[paneIndex];
    if (!pane.image) {
        return rectangles;
    }
    int first = std::min(selectionStart, selectionEnd),
        last = std::max(selectionStart, selectionEnd);
    bool previousVertical = false;
    for (int glyphIndex = first; glyphIndex <= last && glyphIndex < (int)pane.image->glyphs.size();
         glyphIndex++) {
        const Glyph& glyph = pane.image->glyphs[glyphIndex];
        if (glyph.codepoint < 32 || glyph.right <= glyph.left || glyph.bottom <= glyph.top) {
            continue;
        }
        Highlight rectangle{pane.page, 0, glyph.left, glyph.top, glyph.right, glyph.bottom};
        if (!rectangles.empty()) {
            Highlight& previous = rectangles.back();
            float height = std::max(previous.bottom - previous.top, glyph.bottom - glyph.top);
            bool sameLine = fabs((previous.top + previous.bottom - glyph.top - glyph.bottom) / 2) <
                            height * .55;
            bool nearby = glyph.left <= previous.right + height * 2 &&
                          glyph.right >= previous.left - height * 2;
            if (glyph.vertical) {
                float width = std::max(previous.right - previous.left, glyph.right - glyph.left);
                sameLine = fabs((previous.left + previous.right - glyph.left - glyph.right) / 2) <
                           width * .55;
                nearby = glyph.top <= previous.bottom + width * 2 &&
                         glyph.bottom >= previous.top - width * 2;
            }
            if (previousVertical == glyph.vertical && sameLine && nearby) {
                previous.left = std::min(previous.left, glyph.left);
                previous.top = std::min(previous.top, glyph.top);
                previous.right = std::max(previous.right, glyph.right);
                previous.bottom = std::max(previous.bottom, glyph.bottom);
                continue;
            }
        }
        rectangles.push_back(rectangle);
        previousVertical = glyph.vertical;
    }
    return rectangles;
}

bool addInk(int paneIndex, int x, int y, bool startsStroke) {
    auto* workspace = currentWorkspace();
    if (!workspace) {
        return false;
    }
    Pane& pane = workspace->panes[paneIndex];
    if (!pane.image || pane.busy || pane.image->pixels.empty() || !pane.image->error.empty()) {
        return false;
    }
    RECT page = pageRect(paneIndex);
    if (!PtInRect(&page, {x, y})) {
        return false;
    }
    Document& document = documents[pane.documentIndex];
    if (document.ink.size() >= POINT_LIMIT) {
        return false;
    }
    document.ink.push_back({pane.page, startsStroke ? 1 : 0, 2,
                            (x - page.left) / (float)pane.image->width,
                            (y - page.top) / (float)pane.image->height});
    document.dirty = true;
    document.lastTool = 2;
    repaint();
    return true;
}

void finishInteraction(bool commit) {
    if (selecting && commit && interactionPane >= 0 && currentWorkspace()) {
        Pane& pane = currentWorkspace()->panes[interactionPane];
        Document& document = documents[pane.documentIndex];
        auto rectangles = selectionRectangles(interactionPane);
        if (!rectangles.empty() &&
            rectangles.size() + document.highlights.size() <= HIGHLIGHT_LIMIT) {
            int group = document.nextGroup++;
            for (auto& rectangle : rectangles) {
                rectangle.group = group;
                document.highlights.push_back(rectangle);
            }
            document.dirty = true;
            document.lastTool = 1;
        }
    }
    bool changed = drawing || selecting;
    dragging = drawing = selecting = false;
    interactionPane = -1;
    selectionStart = selectionEnd = -1;
    if (GetCapture() == window) {
        ReleaseCapture();
    }
    if (changed) {
        saveAll();
    }
    repaint();
}

void undo() {
    Pane* pane = focusedPane();
    if (!pane) {
        return;
    }
    Document& document = documents[pane->documentIndex];
    bool changed = false;
    // Tool-aware undo avoids deleting a highlight when undoing a pen stroke.
    if (tool == 1 || (tool == 0 && document.lastTool == 1)) {
        for (int markIndex = (int)document.highlights.size() - 1; markIndex >= 0; markIndex--) {
            if (document.highlights[markIndex].page == pane->page) {
                int group = document.highlights[markIndex].group;
                document.highlights.erase(std::remove_if(document.highlights.begin(),
                                                         document.highlights.end(),
                                                         [&](const Highlight& highlight) {
                                                             return highlight.page == pane->page &&
                                                                    highlight.group == group;
                                                         }),
                                          document.highlights.end());
                changed = true;
                break;
            }
        }
    } else {
        for (int markIndex = (int)document.ink.size() - 1; markIndex >= 0; markIndex--) {
            if (document.ink[markIndex].page == pane->page &&
                document.ink[markIndex].startsStroke) {
                int strokeEnd = markIndex + 1;
                while (strokeEnd < (int)document.ink.size() &&
                       !document.ink[strokeEnd].startsStroke) {
                    strokeEnd++;
                }
                document.ink.erase(document.ink.begin() + markIndex,
                                   document.ink.begin() + strokeEnd);
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        document.dirty = true;
        saveAll();
        repaint();
    }
}

enum class IconKind {
    SplitView,
    Highlight,
    Pen,
    PreviousPage,
    NextPage,
    Sidebar,
    AddDocument,
    CloseWorkspace,
    Undo,
    Read
};

void drawIcon(HDC dc, IconKind kind, RECT box, COLORREF color) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)),
                     (float)(1.45 * dpiScale));
    float x = (box.left + box.right) / 2.f, y = (box.top + box.bottom) / 2.f,
          s = (float)(8 * dpiScale);
    auto line = [&](float a, float b, float c, float d) {
        g.DrawLine(&pen, x + a * s, y + b * s, x + c * s, y + d * s);
    };
    switch (kind) {
    case IconKind::SplitView:
        g.DrawRectangle(&pen, x - s, y - s, 2 * s, 2 * s);
        line(0, -1, 0, 1);
        break;
    case IconKind::Highlight:
        line(-.7, .6, .6, -.7);
        line(-.4, .8, .8, -.4);
        line(.6, -.7, .8, -.4);
        line(-.7, .6, -.4, .8);
        line(-.9, 1, .4, 1);
        break;
    case IconKind::Pen:
        line(-.7, .7, .6, -.6);
        line(-.4, .9, .8, -.4);
        line(.6, -.6, .8, -.4);
        line(-.7, .7, -.8, 1);
        line(-.8, 1, -.4, .9);
        break;
    case IconKind::PreviousPage:
        line(.55, -.55, -.1, 0);
        line(-.1, 0, .55, .55);
        break;
    case IconKind::NextPage:
        line(-.55, -.55, .1, 0);
        line(.1, 0, -.55, .55);
        break;
    case IconKind::Sidebar:
        g.DrawRectangle(&pen, x - s, y - s, 2 * s, 2 * s);
        line(-.35, -1, -.35, 1);
        break;
    case IconKind::AddDocument:
        line(-.7, 0, .7, 0);
        line(0, -.7, 0, .7);
        break;
    case IconKind::CloseWorkspace:
        line(-.4, -.4, .4, .4);
        line(-.4, .4, .4, -.4);
        break;
    case IconKind::Undo:
        line(-.6, -.3, -.6, .3);
        line(-.6, -.3, .2, -.3);
        line(-.6, -.3, -.15, -.7);
        g.DrawArc(&pen, x - .6f * s, y - .3f * s, 1.5f * s, 1.2f * s, 270, 170);
        break;
    case IconKind::Read:
        line(-.6, -.9, -.6, .7);
        line(-.6, -.9, .5, .25);
        line(-.6, .7, -.1, .1);
        line(-.1, .1, .5, .25);
        break;
    }
}

void fillRoundedRect(HDC dc, RECT r, COLORREF c, int radius = 7) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ old = SelectObject(dc, b), pen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, scaledPixels(radius), scaledPixels(radius));
    SelectObject(dc, old);
    SelectObject(dc, pen);
    DeleteObject(b);
}

RECT headerButton(int pane, bool next) {
    RECT r = paneArea(pane);
    int right = r.right - scaledPixels(next ? 7 : 37);
    return {right - scaledPixels(28), r.top + scaledPixels(3), right, r.top + scaledPixels(31)};
}

void paintAnnotations(HDC dc, const Pane& pane, RECT page, int paneIndex) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetClip(Gdiplus::Rect(page.left, page.top, pane.image->width, pane.image->height));
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Document& document = documents[pane.documentIndex];
    auto rectangle = [&](const Highlight& highlight, bool preview) {
        Gdiplus::SolidBrush brush(preview ? Gdiplus::Color(72, 74, 125, 183)
                                          : Gdiplus::Color(90, 250, 201, 38));
        float x = page.left + highlight.left * pane.image->width,
              y = page.top + highlight.top * pane.image->height;
        graphics.FillRectangle(
            &brush, x - 1, y - 1,
            std::max(1.f, (highlight.right - highlight.left) * pane.image->width + 2),
            std::max(1.f, (highlight.bottom - highlight.top) * pane.image->height + 2));
    };
    for (auto& highlight : document.highlights) {
        if (highlight.page == pane.page) {
            rectangle(highlight, false);
        }
    }
    if (selecting && interactionPane == paneIndex) {
        for (auto& highlight : selectionRectangles(paneIndex)) {
            rectangle(highlight, true);
        }
    }
    for (unsigned strokeStart = 0; strokeStart < document.ink.size();) {
        const auto& point = document.ink[strokeStart];
        unsigned strokeEnd = strokeStart + 1;
        while (strokeEnd < document.ink.size() && !document.ink[strokeEnd].startsStroke &&
               document.ink[strokeEnd].page == point.page) {
            strokeEnd++;
        }
        if (point.page != pane.page) {
            strokeStart = strokeEnd;
            continue;
        }
        Gdiplus::Pen pen(point.tool == 1 ? Gdiplus::Color(68, 247, 201, 49)
                                         : Gdiplus::Color(230, 48, 74, 61),
                         (float)(pane.image->scale * (point.tool == 1 ? 12 : 1.6)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::GraphicsPath path;
        float x = page.left + point.x * pane.image->width,
              y = page.top + point.y * pane.image->height;
        if (strokeEnd == strokeStart + 1) {
            path.AddLine(x, y, x + .01f, y + .01f);
        }
        for (unsigned pointIndex = strokeStart + 1; pointIndex < strokeEnd; pointIndex++) {
            path.AddLine(page.left + document.ink[pointIndex - 1].x * pane.image->width,
                         page.top + document.ink[pointIndex - 1].y * pane.image->height,
                         page.left + document.ink[pointIndex].x * pane.image->width,
                         page.top + document.ink[pointIndex].y * pane.image->height);
        }
        graphics.DrawPath(&pen, &path);
        strokeStart = strokeEnd;
    }
}

void paintPane(HDC dc, int paneIndex) {
    auto& workspace = *currentWorkspace();
    Pane& pane = workspace.panes[paneIndex];
    RECT area = paneArea(paneIndex), content = contentArea(paneIndex);
    RECT header{area.left + scaledPixels(14), area.top, area.right - scaledPixels(7),
                area.top + scaledPixels(35)};
    if (workspace.count == 2 && workspace.active == paneIndex) {
        RECT active{area.left + scaledPixels(14), area.top + scaledPixels(33),
                    area.right - scaledPixels(14), area.top + scaledPixels(35)};
        fillRect(dc, active, RGB(174, 192, 166));
    }
    RECT name = header;
    name.right -= scaledPixels(165);
    SelectObject(dc, smallFont);
    drawLabel(dc, documentFilename(pane.documentIndex), name, RGB(85, 97, 84),
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    wchar_t count[64];
    swprintf(count, 64, L"%d / %d", pane.page + 1, pane.count);
    RECT number = header;
    number.left = header.right - scaledPixels(165);
    number.right = header.right - scaledPixels(69);
    drawLabel(dc, pane.count ? count : L"…", number, RGB(112, 122, 108));
    drawIcon(dc, IconKind::PreviousPage, headerButton(paneIndex, false),
             pane.page > 0 ? RGB(79, 96, 76) : RGB(184, 191, 178));
    drawIcon(dc, IconKind::NextPage, headerButton(paneIndex, true),
             pane.count && pane.page + 1 < pane.count ? RGB(79, 96, 76) : RGB(184, 191, 178));
    SaveDC(dc);
    IntersectClipRect(dc, content.left, content.top, content.right, content.bottom);
    if (pane.image && !pane.image->pixels.empty() && pane.image->error.empty()) {
        RECT page = pageRect(paneIndex), shadow = page;
        OffsetRect(&shadow, scaledPixels(2), scaledPixels(3));
        fillRect(dc, shadow, RGB(216, 220, 210));
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = pane.image->width;
        bitmapInfo.bmiHeader.biHeight = -pane.image->height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        SetDIBitsToDevice(dc, page.left, page.top, pane.image->width, pane.image->height, 0, 0, 0,
                          pane.image->height, pane.image->pixels.data(), &bitmapInfo,
                          DIB_RGB_COLORS);
        paintAnnotations(dc, pane, page, paneIndex);
    } else {
        RECT text = content;
        InflateRect(&text, -scaledPixels(22), -scaledPixels(22));
        std::wstring message =
            pane.image && !pane.image->error.empty() ? pane.image->error : L"Abriendo…";
        SelectObject(dc, uiFont);
        drawLabel(dc, message.c_str(), text, RGB(106, 118, 101),
                  DT_CENTER | DT_WORDBREAK | DT_VCENTER);
    }
    RestoreDC(dc, -1);
    if (pane.busy && pane.image) {
        RECT r{area.right - scaledPixels(112), area.bottom - scaledPixels(27),
               area.right - scaledPixels(12), area.bottom - scaledPixels(7)};
        fillRoundedRect(dc, r, RGB(249, 250, 246));
        drawLabel(dc, L"Cargando…", r, RGB(113, 123, 108));
    }
}

void paint(HDC supplied = nullptr) {
    PAINTSTRUCT ps{};
    HDC screen = supplied ? supplied : BeginPaint(window, &ps);
    RECT rc = clientRect();
    if (!canvas) {
        canvas = CreateCompatibleDC(screen);
    }
    if (canvasWidth != rc.right || canvasHeight != rc.bottom) {
        if (canvasBitmap) {
            SelectObject(canvas, originalBitmap);
            DeleteObject(canvasBitmap);
        }
        canvasWidth = rc.right;
        canvasHeight = rc.bottom;
        canvasBitmap = CreateCompatibleBitmap(screen, std::max<int>(1, canvasWidth),
                                              std::max<int>(1, canvasHeight));
        originalBitmap = (HBITMAP)SelectObject(canvas, canvasBitmap);
    }
    HDC dc = canvas;
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, uiFont);
    fillRect(dc, rc, RGB(239, 241, 235));
    RECT bar{0, 0, rc.right, scaledPixels(44)};
    fillRect(dc, bar, RGB(250, 251, 248));
    RECT border{0, scaledPixels(43), rc.right, scaledPixels(44)};
    fillRect(dc, border, RGB(225, 229, 219));
    RECT sideButton{scaledPixels(5), scaledPixels(5), scaledPixels(39), scaledPixels(39)};
    if (sidebarVisible) {
        fillRoundedRect(dc, sideButton, RGB(231, 237, 225));
    }
    drawIcon(dc, IconKind::Sidebar, sideButton, RGB(81, 99, 77));
    int tabs = visibleTabs();
    for (int n = tabOffset; n < (int)workspaces.size() && n < tabOffset + tabs; n++) {
        RECT tab = tabRect(n);
        bool active = n == activeWorkspace;
        if (active) {
            fillRoundedRect(dc, tab, RGB(231, 237, 225));
            RECT underline{tab.left + scaledPixels(12), tab.bottom - scaledPixels(2),
                           tab.right - scaledPixels(12), tab.bottom};
            fillRect(dc, underline, RGB(144, 165, 130));
        }
        RECT name = tab;
        name.left += scaledPixels(12);
        name.right -= scaledPixels(27);
        SelectObject(dc, smallFont);
        std::wstring title = workspaceName(n);
        drawLabel(dc, title.c_str(), name, active ? RGB(48, 71, 42) : RGB(111, 124, 104),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT close{tab.right - scaledPixels(24), tab.top + scaledPixels(5),
                   tab.right - scaledPixels(3), tab.bottom - scaledPixels(5)};
        drawIcon(dc, IconKind::CloseWorkspace, close, RGB(134, 148, 124));
    }
    RECT pair{rc.right - scaledPixels(78), scaledPixels(5), rc.right - scaledPixels(44),
              scaledPixels(39)},
        plus{rc.right - scaledPixels(39), scaledPixels(5), rc.right - scaledPixels(5),
             scaledPixels(39)};
    drawIcon(dc, IconKind::SplitView, pair, RGB(81, 99, 77));
    drawIcon(dc, IconKind::AddDocument, plus, RGB(81, 99, 77));
    if (sidebarVisible) {
        RECT side{0, scaledPixels(44), scaledPixels(58), rc.bottom};
        fillRect(dc, side, RGB(248, 250, 244));
        RECT edge{scaledPixels(57), scaledPixels(44), scaledPixels(58), rc.bottom};
        fillRect(dc, edge, RGB(226, 231, 219));
        const wchar_t* names[] = {L"Leer", L"Subrayar", L"Lápiz", L"Deshacer", L"Doble"};
        IconKind kinds[] = {IconKind::Read, IconKind::Highlight, IconKind::Pen, IconKind::Undo,
                            IconKind::SplitView};
        for (int n = 0; n < 5; n++) {
            int y = scaledPixels(57 + n * 58);
            RECT box{scaledPixels(9), y, scaledPixels(49), y + scaledPixels(34)};
            if (n < 3 && tool == n) {
                fillRoundedRect(dc, box, RGB(227, 236, 218));
            }
            drawIcon(dc, kinds[n], box, RGB(81, 101, 73));
            RECT caption{0, y + scaledPixels(35), scaledPixels(57), y + scaledPixels(51)};
            SelectObject(dc, smallFont);
            drawLabel(dc, names[n], caption, RGB(120, 132, 111));
        }
    }
    if (auto* w = currentWorkspace()) {
        for (int n = 0; n < w->count; n++) {
            paintPane(dc, n);
        }
        if (w->count == 2) {
            RECT left = paneArea(0),
                 divider{left.right + scaledPixels(5), scaledPixels(58),
                         left.right + scaledPixels(6), rc.bottom - scaledPixels(16)};
            fillRect(dc, divider, RGB(222, 228, 214));
        }
    } else {
        RECT center{scaledPixels(30), rc.bottom / 2 - scaledPixels(50), rc.right - scaledPixels(30),
                    rc.bottom / 2 + scaledPixels(8)};
        SelectObject(dc, headingFont);
        drawLabel(dc, L"Un espacio para estudiar.", center, RGB(48, 67, 44));
        center.top = center.bottom;
        center.bottom += scaledPixels(50);
        SelectObject(dc, uiFont);
        drawLabel(dc, L"Arrastra tus PDFs o pulsa Ctrl + O", center, RGB(114, 127, 106));
    }
    if (!toast.empty()) {
        RECT r{scaledPixels(sidebarVisible ? 74 : 18), rc.bottom - scaledPixels(48),
               rc.right - scaledPixels(18), rc.bottom - scaledPixels(14)};
        fillRoundedRect(dc, r, RGB(249, 251, 245));
        SelectObject(dc, smallFont);
        drawLabel(dc, toast.c_str(), r, RGB(88, 106, 77),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    BitBlt(screen, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    if (!supplied) {
        EndPaint(window, &ps);
    }
}

void createFonts() {
    if (uiFont) {
        DeleteObject(uiFont);
    }
    if (headingFont) {
        DeleteObject(headingFont);
    }
    if (smallFont) {
        DeleteObject(smallFont);
    }
    auto font = [](int size, int weight) {
        return CreateFontW(-scaledPixels(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
    };
    uiFont = font(14, FW_NORMAL);
    smallFont = font(12, FW_NORMAL);
    headingFont = font(28, FW_NORMAL);
}

void toggleFullscreen() {
    fullscreen = !fullscreen;
    if (fullscreen) {
        GetWindowPlacement(window, &placement);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window, nullptr, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER);
    } else {
        SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(window, &placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }
}

void handleCommand(HWND hwnd, unsigned command) {

    if (command >= WORKSPACE_MENU && command < WORKSPACE_MENU + 128) {
        switchWorkspace(command - WORKSPACE_MENU);
        return;
    }
    if (command >= 3000 && command < 3064) {
        createPair(command - 3000);
        return;
    }
    switch (command) {
    case OPEN:
        openDialog();
        break;
    case EXPORT_PDF:
        exportDialog();
        break;
    case EXIT:
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
        break;
    case FIT:
        fitPage();
        break;
    case PLUS:
        zoomBy(1.2);
        break;
    case MINUS:
        zoomBy(1 / 1.2);
        break;
    case FULL:
        toggleFullscreen();
        break;
    case HAND:
        setTool(0);
        break;
    case MARKER:
        setTool(1);
        break;
    case PEN:
        setTool(2);
        break;
    case UNDO:
        undo();
        break;
    case SAVE:
        saveAll();
        break;
    case SIDEBAR:
        toggleSidebar();
        break;
    case SPLIT:
        pairMenu();
        break;
    }
    return;
}

void handleKeyDown(HWND hwnd, WPARAM key) {

    bool controlPressed = GetKeyState(VK_CONTROL) & 0x8000,
         shiftPressed = GetKeyState(VK_SHIFT) & 0x8000;
    Pane* pane = focusedPane();
    if (controlPressed && key >= '1' && key <= '9') {
        switchWorkspace((int)key - '1');
    } else if (controlPressed && key == VK_TAB && !workspaces.empty()) {
        switchWorkspace((activeWorkspace + (int)workspaces.size() + (shiftPressed ? -1 : 1)) %
                        (int)workspaces.size());
    } else if (controlPressed && key == 'O') {
        openDialog();
    } else if (controlPressed && shiftPressed && key == 'S') {
        exportDialog();
    } else if (controlPressed && key == 'S') {
        finishInteraction();
        saveAll();
    } else if (controlPressed && key == 'Z') {
        undo();
    } else if (controlPressed && key == 'W') {
        closeWorkspace(activeWorkspace);
    } else if (controlPressed && key == 'B') {
        toggleSidebar();
    } else if (controlPressed && key == 'D') {
        pairMenu();
    } else if (key == 'H') {
        setTool(1);
    } else if (key == 'P') {
        setTool(2);
    } else if (key == 'V') {
        setTool(0);
    } else if (key == VK_F10) {
        menuVisible = !menuVisible;
        SetMenu(hwnd, menuVisible ? menus : nullptr);
        DrawMenuBar(hwnd);
    } else if (key == VK_TAB && currentWorkspace() && currentWorkspace()->count == 2) {
        setActivePane(1 - currentWorkspace()->active);
    } else if (pane && (key == VK_NEXT || key == VK_RIGHT || key == VK_SPACE)) {
        goToPage(pane->page + 1);
    } else if (pane && (key == VK_PRIOR || key == VK_LEFT)) {
        goToPage(pane->page - 1);
    } else if (pane && key == VK_HOME) {
        goToPage(0);
    } else if (pane && key == VK_END) {
        goToPage(pane->count - 1);
    } else if (key == VK_ADD || key == VK_OEM_PLUS) {
        zoomBy(1.2);
    } else if (key == VK_SUBTRACT || key == VK_OEM_MINUS) {
        zoomBy(1 / 1.2);
    } else if (key == '0') {
        fitPage();
    } else if (key == VK_F11) {
        toggleFullscreen();
    } else if (key == VK_ESCAPE) {
        finishInteraction(false);
        if (fullscreen) {
            toggleFullscreen();
        } else if (menuVisible) {
            menuVisible = false;
            SetMenu(hwnd, nullptr);
            DrawMenuBar(hwnd);
        } else {
            setTool(0);
        }
    } else if (pane && (key == VK_UP || key == VK_DOWN)) {
        pane->panY += key == VK_UP ? scaledPixels(80) : -scaledPixels(80);
        repaint();
    }
    return;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
#ifdef BRISA_TEST
    case WM_APP + 12:
        if (focusedPane()) {
            beginExport(documents[focusedPane()->documentIndex].path + L".exported.pdf");
        }
        return 0;
    case WM_APP + 13:
        return exportBusy;
    case WM_APP + 7: {
        HDC dc = GetDC(hwnd);
        paint(dc);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = canvasWidth;
        info.bmiHeader.biHeight = -canvasHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        std::vector<unsigned char> buffer((size_t)canvasWidth * canvasHeight * 4);
        SelectObject(canvas, originalBitmap);
        GetDIBits(dc, canvasBitmap, 0, canvasHeight, buffer.data(), &info, DIB_RGB_COLORS);
        SelectObject(canvas, canvasBitmap);
        ReleaseDC(hwnd, dc);
        BITMAPFILEHEADER head{};
        head.bfType = 0x4d42;
        head.bfOffBits = sizeof(head) + sizeof(BITMAPINFOHEADER);
        head.bfSize = head.bfOffBits + buffer.size();
        FILE* f = fopen("work/capture.bmp", "wb");
        if (f) {
            fwrite(&head, sizeof(head), 1, f);
            fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
            fwrite(buffer.data(), buffer.size(), 1, f);
            fclose(f);
        }
        return 0;
    }
    case WM_APP + 8: {
        BYTE keys[256]{};
        if (wp & 1) {
            keys[VK_CONTROL] = 0x80;
        }
        if (wp & 2) {
            keys[VK_SHIFT] = 0x80;
        }
        SetKeyboardState(keys);
        return 0;
    }
    case WM_APP + 9: {
        auto* w = currentWorkspace();
        if (w) {
            for (int n = 0; n < w->count; n++) {
                if (w->panes[n].busy) {
                    return 1;
                }
            }
        }
        return 0;
    }
    case WM_APP + 10: {
        auto* w = currentWorkspace();
        if (wp == 0) {
            return workspaces.size();
        }
        if (wp == 1) {
            return w ? w->count : 0;
        }
        if (wp == 2) {
            return w ? w->active : 0;
        }
        if (wp == 3 || wp == 4) {
            return w ? w->panes[wp - 3].page : -1;
        }
        if (wp == 5) {
            return focusedPane() ? documents[focusedPane()->documentIndex].highlights.size() : 0;
        }
        if (wp == 6) {
            return focusedPane() && focusedPane()->image ? focusedPane()->image->glyphs.size() : 0;
        }
        if (wp == 7) {
            return activeWorkspace;
        }
        if (wp == 8) {
            return focusedPane() ? documents[focusedPane()->documentIndex].ink.size() : 0;
        }
        return 0;
    }
    case WM_APP + 11: {
        auto* w = currentWorkspace();
        if (!w) {
            return 0;
        }
        int n = w->active;
        auto& p = w->panes[n];
        if (!p.image) {
            return 0;
        }
        RECT r = pageRect(n);
        FILE* f = fopen("work/glyphs.tsv", "wb");
        if (f) {
            for (int j = 0; j < (int)p.image->glyphs.size(); j++) {
                auto& g = p.image->glyphs[j];
                if (g.codepoint < 32 || g.right <= g.left || g.bottom <= g.top) {
                    continue;
                }
                fprintf(f, "%d\t%u\t%.2f\t%.2f\t%.2f\t%.2f\n", j, g.codepoint,
                        r.left + g.left * p.image->width, r.top + g.top * p.image->height,
                        r.left + g.right * p.image->width, r.top + g.bottom * p.image->height);
            }
            fclose(f);
        }
        return 0;
    }
#endif
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint();
        return 0;
    case WM_PRINTCLIENT:
        paint((HDC)wp);
        return 0;
    case WM_CLOSE:
        if (exportBusy) {
            showNotification(L"Espera a que termine el guardado del PDF.");
            return 0;
        }
        finishInteraction();
        if (saveAll(true)) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_KILLFOCUS:
        finishInteraction(false);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize = {scaledPixels(760), scaledPixels(500)};
        return 0;
    case WM_DPICHANGED: {
        finishInteraction(false);
        dpiScale = HIWORD(wp) / 96.0;
        createFonts();
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            finishInteraction(false);
            ensureVisible();
            SetTimer(hwnd, 1, 120, nullptr);
            repaint();
        }
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            requestRender();
        } else if (wp == 2) {
            KillTimer(hwnd, 2);
            toast.clear();
            repaint();
        }
        return 0;
    case READY: {
        RenderResult* result = (RenderResult*)lp;
        if (result->generation != InterlockedCompareExchange(&generation, 0, 0) ||
            !currentWorkspace()) {
            disposeRenderResult(result);
            return 0;
        }
        auto& w = *currentWorkspace();
        for (int n = 0; n < w.count; n++) {
            if (result->images[n]) {
                auto& p = w.panes[n];
                p.image.reset(result->images[n]);
                result->images[n] = nullptr;
                p.page = p.image->page;
                p.count = p.image->count;
                p.busy = false;
            }
        }
        disposeRenderResult(result);
        updateTitle();
        repaint();
        return 0;
    }
    case EXPORT_READY: {
        std::unique_ptr<ExportResult> result((ExportResult*)lp);
        exportBusy = false;
        showNotification(result->error.empty() ? L"PDF anotado guardado." : result->error.c_str());
        return 0;
    }
    case WM_DROPFILES: {
        finishInteraction();
        UINT count = DragQueryFileW((HDROP)wp, 0xffffffff, nullptr, 0);
        for (UINT n = 0; n < count; n++) {
            wchar_t path[32768];
            if (DragQueryFileW((HDROP)wp, n, path, 32768)) {
                openPath(path);
            }
        }
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU && lp == 0) {
            menuVisible = !menuVisible;
            SetMenu(hwnd, menuVisible ? menus : nullptr);
            DrawMenuBar(hwnd);
            return 0;
        }
        break;
    case WM_COMMAND:
        handleCommand(hwnd, LOWORD(wp));
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT rc = clientRect();
        if (y < scaledPixels(44)) {
            if (x < scaledPixels(44)) {
                toggleSidebar();
            } else if (x > rc.right - scaledPixels(42)) {
                openDialog();
            } else if (x > rc.right - scaledPixels(82)) {
                pairMenu();
            } else {
                for (int n = tabOffset; n < (int)workspaces.size() && n < tabOffset + visibleTabs();
                     n++) {
                    RECT r = tabRect(n);
                    if (PtInRect(&r, {x, y})) {
                        if (x > r.right - scaledPixels(25)) {
                            closeWorkspace(n);
                        } else {
                            switchWorkspace(n);
                        }
                        break;
                    }
                }
            }
            return 0;
        }
        if (sidebarVisible && x < scaledPixels(58)) {
            int row = (y - scaledPixels(57)) / scaledPixels(58);
            if (y >= scaledPixels(57) && row >= 0 && row < 5) {
                if (row < 3) {
                    setTool(row);
                } else if (row == 3) {
                    undo();
                } else {
                    pairMenu();
                }
            }
            return 0;
        }
        int n = paneAt(x, y);
        if (n < 0) {
            return 0;
        }
        setActivePane(n);
        RECT prev = headerButton(n, false), next = headerButton(n, true);
        if (PtInRect(&prev, {x, y})) {
            goToPage(focusedPane()->page - 1, n);
            return 0;
        }
        if (PtInRect(&next, {x, y})) {
            goToPage(focusedPane()->page + 1, n);
            return 0;
        }
        RECT area = contentArea(n);
        if (!PtInRect(&area, {x, y})) {
            return 0;
        }
        Pane& p = currentWorkspace()->panes[n];
        if (p.busy || !p.image || !p.image->error.empty()) {
            return 0;
        }
        interactionPane = n;
        if (tool == 1) {
            selectionStart = selectionEnd = glyphAt(n, x, y, true);
            if (selectionStart >= 0) {
                selecting = true;
                SetCapture(hwnd);
                repaint();
            } else if (glyphAt(n, x, y, false) < 0) {
                showNotification(
                    L"Esta página no tiene texto seleccionable. Puedes usar el lápiz.");
            }
        } else if (tool == 2) {
            if (addInk(n, x, y, true)) {
                drawing = true;
                SetCapture(hwnd);
            }
        } else {
            dragging = true;
            dragX = x;
            dragY = y;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (selecting) {
            int index = glyphAt(interactionPane, x, y, false);
            if (index >= 0) {
                selectionEnd = index;
            }
            repaint();
        } else if (drawing) {
            if (!addInk(interactionPane, x, y, false)) {
                finishInteraction();
            }
        } else if (dragging && currentWorkspace()) {
            Pane& p = currentWorkspace()->panes[interactionPane];
            p.panX += x - dragX;
            p.panY += y - dragY;
            dragX = x;
            dragY = y;
            repaint();
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (selecting) {
            int n = glyphAt(interactionPane, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false);
            if (n >= 0) {
                selectionEnd = n;
            }
        }
        if (drawing) {
            addInk(interactionPane, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false);
        }
        finishInteraction();
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging || drawing || selecting) {
            finishInteraction(false);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            int n = paneAt(pt.x, pt.y);
            bool content = false;
            if (n >= 0) {
                RECT r = contentArea(n);
                content = PtInRect(&r, pt);
            }
            SetCursor(content && tool == 1   ? LoadCursorW(nullptr, IDC_IBEAM)
                      : content && tool == 2 ? penCursor
                      : dragging             ? LoadCursorW(nullptr, IDC_SIZEALL)
                                             : LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (pt.y < scaledPixels(44)) {
            tabOffset = std::clamp(tabOffset + (delta < 0 ? 1 : -1), 0,
                                   std::max<int>(0, (int)workspaces.size() - visibleTabs()));
            repaint();
            return 0;
        }
        int n = paneAt(pt.x, pt.y);
        if (n < 0) {
            return 0;
        }
        setActivePane(n);
        Pane& p = *focusedPane();
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
            zoomBy(delta > 0 ? 1.2 : 1 / 1.2, n);
        } else if (!p.fit) {
            p.panY += delta;
            repaint();
        } else if (abs(delta) >= WHEEL_DELTA) {
            goToPage(p.page + (delta < 0 ? 1 : -1), n);
        }
        return 0;
    }
    case WM_KEYDOWN:
        handleKeyDown(hwnd, wp);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int selfTest(const wchar_t* path) {
    FPDF_InitLibrary();
    PdfEngine engine;
    RenderItem item;
    item.path = path;
    item.width = 900;
    item.height = 1100;
    auto image = std::unique_ptr<RenderedPage>(renderPage(engine, item, 0, false));
    bool ok = image->error.empty() && !image->pixels.empty();
    std::wstring report =
        L"ok=" + std::to_wstring(ok) + L"\npages=" + std::to_wstring(image->count) + L"\nglyphs=" +
        std::to_wstring(image->glyphs.size()) + L"\nerror=" + image->error + L"\n";
    std::wstring target = item.path + L".brisa-test.txt";
    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        int len = WideCharToMultiByte(CP_UTF8, 0, report.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::vector<char> bytes(len);
        WideCharToMultiByte(CP_UTF8, 0, report.c_str(), -1, bytes.data(), len, nullptr, nullptr);
        DWORD done;
        WriteFile(file, bytes.data(), len - 1, &done, nullptr);
        CloseHandle(file);
    }
    engine.close();
    FPDF_DestroyLibrary();
    return ok ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
#ifdef BRISA_TEST
    if (argc == 4 && !wcscmp(argv[1], L"--export-test")) {
        FPDF_InitLibrary();
        Document document;
        document.path = argv[2];
        loadDocument(document);
        std::unique_ptr<ExportResult> result(exportPdf({document, argv[3]}));
        int code = result->error.empty() ? 0 : 1;
        FPDF_DestroyLibrary();
        LocalFree(argv);
        return code;
    }
#endif
    if (argc >= 3 && !wcscmp(argv[1], L"--self-test")) {
        int code = selfTest(argv[2]);
        LocalFree(argv);
        return code;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput startup;
    Gdiplus::GdiplusStartup(&gdiplusToken, &startup, nullptr);
    dpiScale = GetDpiForSystem() / 96.0;
    createFonts();
    InitializeCriticalSection(&renderMutex);
    wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    BYTE andMask[128], xorMask[128]{};
    memset(andMask, 255, sizeof(andMask));
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int d = (x - 16) * (x - 16) + (y - 16) * (y - 16);
            if (d >= 5 && d <= 13) {
                andMask[y * 4 + x / 8] &= ~(0x80 >> (x % 8));
            }
        }
    }
    penCursor = CreateCursor(instance, 16, 16, 32, 32, andMask, xorMask);
    WNDCLASSW cls{};
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"BrisaPdfWindow";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&cls);
    menus = CreateMenu();
    HMENU file = CreatePopupMenu(), view = CreatePopupMenu(), mark = CreatePopupMenu();
    workspaceMenu = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, OPEN, L"Abrir PDFs…\tCtrl+O");
    AppendMenuW(file, MF_STRING, SAVE, L"Guardar marcas\tCtrl+S");
    AppendMenuW(file, MF_STRING, EXPORT_PDF, L"Guardar PDF anotado…\tCtrl+Shift+S");
    AppendMenuW(file, MF_STRING, EXIT, L"Salir");
    AppendMenuW(view, MF_STRING, FIT, L"Ajustar página\t0");
    AppendMenuW(view, MF_STRING, PLUS, L"Acercar\t+");
    AppendMenuW(view, MF_STRING, MINUS, L"Alejar\t−");
    AppendMenuW(view, MF_STRING, SIDEBAR, L"Herramientas\tCtrl+B");
    AppendMenuW(view, MF_STRING, SPLIT, L"Dos PDFs lado a lado\tCtrl+D");
    AppendMenuW(view, MF_STRING, FULL, L"Pantalla completa\tF11");
    AppendMenuW(mark, MF_STRING | MF_CHECKED, HAND, L"Leer / mover\tV");
    AppendMenuW(mark, MF_STRING, MARKER, L"Subrayar texto\tH");
    AppendMenuW(mark, MF_STRING, PEN, L"Lápiz\tP");
    AppendMenuW(mark, MF_STRING, UNDO, L"Deshacer\tCtrl+Z");
    AppendMenuW(menus, MF_POPUP, (UINT_PTR)file, L"Archivo");
    AppendMenuW(menus, MF_POPUP, (UINT_PTR)view, L"Vista");
    AppendMenuW(menus, MF_POPUP, (UINT_PTR)mark, L"Marcar");
    AppendMenuW(menus, MF_POPUP, (UINT_PTR)workspaceMenu, L"Workspaces");
    window = CreateWindowExW(WS_EX_ACCEPTFILES, cls.lpszClassName, L"Brisa", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, scaledPixels(1240), scaledPixels(880),
                             nullptr, nullptr, instance, nullptr);
    if (!window || !wakeEvent) {
        return 1;
    }
    renderWorker = CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr);
    if (!renderWorker) {
        return 1;
    }
    for (int n = 1; n < argc; n++) {
        openPath(argv[n]);
    }
    LocalFree(argv);
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    InterlockedExchange(&stopping, 1);
    SetEvent(wakeEvent);
    WaitForSingleObject(renderWorker, INFINITE);
    while (PeekMessageW(&msg, nullptr, READY, READY, PM_REMOVE)) {
        disposeRenderResult((RenderResult*)msg.lParam);
    }
    while (PeekMessageW(&msg, nullptr, EXPORT_READY, EXPORT_READY, PM_REMOVE)) {
        delete (ExportResult*)msg.lParam;
    }
    CloseHandle(renderWorker);
    CloseHandle(wakeEvent);
    DeleteCriticalSection(&renderMutex);
    if (canvas) {
        SelectObject(canvas, originalBitmap);
        DeleteObject(canvasBitmap);
        DeleteDC(canvas);
    }
    DeleteObject(uiFont);
    DeleteObject(smallFont);
    DeleteObject(headingFont);
    DestroyCursor(penCursor);
    DestroyMenu(menus);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
