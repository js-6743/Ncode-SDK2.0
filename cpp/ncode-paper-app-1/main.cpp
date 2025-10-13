// N-code composer (MuPDF 1.27+ compatible)
// Single-pattern overlay: paste ONE Ncode pattern page over the design (no tiling),
// keep its original physical size (points), and ignore the mid-gray watermark
// (only dark pixels become pure K). The printed dot size matches the original Ncode PDF.
//
// Pattern PDFs are read from ".\\Ncode paper\\*.pdf" (sorted by leading number; all pages).
// Page size = design page size (points). No A4 forcing.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>

#define NOMINMAX         // <— add this line
#include <windows.h>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

static fz_context* ctx = nullptr;
static pdf_document* doc_dest = nullptr;

// --------- util: exe folder and pattern enumeration ----------
static std::string exe_dir()
{
    char buf[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) s.resize(p);
    return s;
}

static int leading_int(const char* s)
{
    int i = 0, v = 0, ok = 0;
    while (s[i] && (unsigned char)s[i] <= ' ') ++i;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ok = 1; ++i; }
    return ok ? v : 1000000000;
}

struct FileItem { std::string path; int key; };

static std::vector<FileItem> list_sorted_pdfs(const std::string& folder)
{
    std::vector<FileItem> out;
    std::string pat = folder + "\\*.pdf";
    WIN32_FIND_DATAA ffd{};
    HANDLE h = FindFirstFileA(pat.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            out.push_back({ folder + "\\" + ffd.cFileName, leading_int(ffd.cFileName) });
    } while (FindNextFileA(h, &ffd));
    FindClose(h);
    std::sort(out.begin(), out.end(),
        [](const FileItem& a, const FileItem& b) {
            if (a.key != b.key) return a.key < b.key;
            return _stricmp(a.path.c_str(), b.path.c_str()) < 0;
        });
    return out;
}

struct PatPage { std::string file; int index; };

static std::vector<PatPage> build_pattern_pages(const std::vector<FileItem>& files)
{
    std::vector<PatPage> out;
    for (auto& f : files) {
        fz_document* d = nullptr;
        try { d = fz_open_document(ctx, f.path.c_str()); }
        catch (...) { d = nullptr; }
        if (!d) continue;
        int n = 0; try { n = fz_count_pages(ctx, d); }
        catch (...) { n = 0; }
        fz_drop_document(ctx, d);
        for (int i = 0;i < n;++i) out.push_back({ f.path, i });
    }
    return out;
}

// --------- PDF helpers ----------
static void add_image_res(pdf_obj* res, const char* name, fz_image* image)
{
    pdf_obj* xobj = pdf_dict_gets(ctx, res, "XObject");
    if (!xobj) {
        xobj = pdf_new_dict(ctx, doc_dest, 10);
        pdf_dict_puts_drop(ctx, res, "XObject", xobj);
    }
    pdf_obj* ref = pdf_add_image(ctx, doc_dest, image);
    pdf_dict_puts(ctx, xobj, name, ref);
    pdf_drop_obj(ctx, ref);
    fz_drop_image(ctx, image);
}

static void draw_image_full(fz_buffer* contents, const char* name, float w_pt, float h_pt)
{
    char ln[128];
    fz_append_string(ctx, contents, "q\n");
    std::snprintf(ln, sizeof(ln), "%.4f 0 0 %.4f 0 0 cm\n", w_pt, h_pt);
    fz_append_string(ctx, contents, ln);
    std::snprintf(ln, sizeof(ln), "/%s Do\n", name);
    fz_append_string(ctx, contents, ln);
    fz_append_string(ctx, contents, "Q\n");
}

static bool create_page_fill(const char* xname, float w_pt, float h_pt, fz_image* img)
{
    fz_rect mediabox = fz_make_rect(0, 0, w_pt, h_pt);
    pdf_obj* res = pdf_new_dict(ctx, doc_dest, 8);
    pdf_obj* imRef = pdf_add_image(ctx, doc_dest, img);
    pdf_obj* xobj = pdf_dict_gets(ctx, res, "XObject");
    if (!xobj) { xobj = pdf_new_dict(ctx, doc_dest, 4); pdf_dict_puts_drop(ctx, res, "XObject", xobj); }
    pdf_dict_puts(ctx, xobj, xname, imRef);
    pdf_drop_obj(ctx, imRef);
    fz_drop_image(ctx, img);

    fz_buffer* contents = fz_new_buffer(ctx, 2048);
    draw_image_full(contents, xname, w_pt, h_pt);

    pdf_obj* page = pdf_add_page(ctx, doc_dest, mediabox, 0, res, contents);
    bool ok = (page != nullptr);
    if (ok) {
        pdf_insert_page(ctx, doc_dest, -1, page);
        pdf_drop_obj(ctx, page);
    }
    fz_drop_buffer(ctx, contents);
    pdf_drop_obj(ctx, res);
    return ok;
}

int main(int argc, char* argv[])
{
    if (argc != 2) { std::cerr << "Usage: " << argv[0] << " <input.pdf>\n"; return 1; }

    const std::string design_pdf = argv[1];
    std::string out_pdf = design_pdf;
    size_t p = out_pdf.rfind(".pdf");
    out_pdf = (p != std::string::npos) ? out_pdf.insert(p, "_ncode") : out_pdf + "_ncode.pdf";

    // Init MuPDF
    ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx) { std::cerr << "Cannot init MuPDF\n"; return 1; }
    fz_register_document_handlers(ctx);
    pdf_write_options saveopt{}; pdf_parse_write_options(ctx, &saveopt, "compress");
    doc_dest = pdf_create_document(ctx);

    // Pattern pages list
    std::string patFolder = exe_dir() + "\\Ncode paper";
    auto files = list_sorted_pdfs(patFolder);
    auto patPages = build_pattern_pages(files);
    if (patPages.empty()) { std::cerr << "No pattern PDFs in: " << patFolder << "\n"; goto finish; }

    // Pattern-locked DPI (dot-size driver); use the value that printed well for you
    const int   pattern_dpi = 600;               // your “good” setting
    const float scale = pattern_dpi / 72.0f;  // pixels per point

    // Single-pattern placement: align top-left (true) or center (false)
    const bool align_top_left = true;            // change to false to center the pattern page

    // Open design
    fz_document* doc_src = nullptr;
    try { doc_src = fz_open_document(ctx, design_pdf.c_str()); }
    catch (...) { doc_src = nullptr; }
    if (!doc_src) { std::cerr << "Cannot open design: " << design_pdf << "\n"; goto finish; }

    int pageCount = 0; try { pageCount = fz_count_pages(ctx, doc_src); }
    catch (...) { pageCount = 0; }
    if (!pageCount) { std::cerr << "No design pages\n"; goto finish; }

    int pages_added = 0;

    for (int i = 0; i < pageCount; ++i)
    {
        std::cout << "\nDesign page " << (i + 1) << "/" << pageCount << "\n";

        // Design page in points
        fz_page* dpage = nullptr;
        try { dpage = fz_load_page(ctx, doc_src, i); }
        catch (...) { dpage = nullptr; }
        if (!dpage) { std::cerr << "  ! load design page\n"; continue; }
        fz_rect drect = fz_bound_page(ctx, dpage);
        const float dw_pt = drect.x1, dh_pt = drect.y1;

        // Rasterize design -> RGB at pattern-locked scale
        fz_matrix ctm = fz_scale(scale, scale);
        fz_pixmap* pixRGB = nullptr;
        try { pixRGB = fz_new_pixmap_from_page(ctx, dpage, ctm, fz_device_rgb(ctx), 0); }
        catch (...) { pixRGB = nullptr; }
        fz_drop_page(ctx, dpage);
        if (!pixRGB) { std::cerr << "  ! raster design\n"; continue; }
        const int DW = pixRGB->w, DH = pixRGB->h;
        std::cout << "  design raster: " << DW << " x " << DH << " @ " << pattern_dpi << " dpi\n";

        // Convert design to CMY (K=0)
        fz_pixmap* pixCMYK = fz_new_pixmap(ctx, fz_device_cmyk(ctx), DW, DH, NULL, 0);
        for (int y = 0; y < DH; ++y) {
            for (int x = 0; x < DW; ++x) {
                unsigned char* ps = &pixRGB->samples[(y * DW + x) * pixRGB->n];
                unsigned char* pd = &pixCMYK->samples[(y * DW + x) * pixCMYK->n];
                pd[0] = 255 - ps[0];  // C
                pd[1] = 255 - ps[1];  // M
                pd[2] = 255 - ps[2];  // Y
                pd[3] = 0;            // K
            }
        }
        fz_drop_pixmap(ctx, pixRGB);

        // Select pattern page (wrap)
        const PatPage& pp = patPages[i % (int)patPages.size()];

        // Rasterize pattern page ONCE to gray at the same scale (no tiling)
        fz_document* pdoc = nullptr; fz_page* ppat = nullptr;
        fz_pixmap* pixPat = nullptr;
        try { pdoc = fz_open_document(ctx, pp.file.c_str()); }
        catch (...) { pdoc = nullptr; }
        if (pdoc) {
            try { ppat = fz_load_page(ctx, pdoc, pp.index); }
            catch (...) { ppat = nullptr; }
            if (ppat) {
                fz_rect prect = fz_bound_page(ctx, ppat);         // pattern page size in points
                const int PW = (int)(prect.x1 * scale + 0.5f);    // in pixels at the same scale
                const int PH = (int)(prect.y1 * scale + 0.5f);

                try { pixPat = fz_new_pixmap_from_page(ctx, ppat, ctm, fz_device_gray(ctx), 0); }
                catch (...) { pixPat = nullptr; }

                if (pixPat) {
                    std::cout << "  pattern raster: " << pixPat->w << " x " << pixPat->h
                        << " (expected ~" << PW << " x " << PH << ")\n";
                }
                fz_drop_page(ctx, ppat);
            }
            fz_drop_document(ctx, pdoc);
        }
        if (!pixPat) {
            std::cerr << "  ! raster pattern failed; skip page\n";
            fz_drop_pixmap(ctx, pixCMYK);
            continue;
        }

        // Compute where to place the single pattern pixmap (no tiling)
        int offX = 0, offY = 0;
        if (!align_top_left) { // center the pattern if you prefer
            offX = (DW - pixPat->w) / 2;
            offY = (DH - pixPat->h) / 2;
        }
        // Clip to page if offsets push us out of bounds
        int startX = std::max(0, offX);
        int startY = std::max(0, offY);
        int endX = std::min(DW, offX + pixPat->w);
        int endY = std::min(DH, offY + pixPat->h);

        // Compose: ONLY overlay where the pattern sits; pattern -> pure K, design CMY untouched elsewhere
        fz_pixmap* pixFinal = fz_new_pixmap(ctx, fz_device_cmyk(ctx), DW, DH, NULL, 0);

        // Start with design CMY
        std::memcpy(pixFinal->samples, pixCMYK->samples, (size_t)DW * DH * pixCMYK->n);

        const int TH = 128; // dot threshold
        for (int y = startY; y < endY; ++y) {
            int py = y - offY;
            for (int x = startX; x < endX; ++x) {
                int px = x - offX;
                unsigned char* pn = &pixPat->samples[(py * pixPat->w + px) * pixPat->n];
                // Only paint K if this pixel is dark (dot). Mid-gray watermark => ignored.
                if (pn[0] < TH) {
                    unsigned char* pf = &pixFinal->samples[(y * DW + x) * pixFinal->n];
                    pf[3] = 255; // K = 255
                    pf[0] = 0; pf[1] = 0; pf[2] = 0; // ensure CMY=0 under dot for clean K
                }
            }
        }

        fz_drop_pixmap(ctx, pixPat);
        fz_drop_pixmap(ctx, pixCMYK);

        // Write final page
        fz_image* pageImg = fz_new_image_from_pixmap(ctx, pixFinal, NULL);
        fz_drop_pixmap(ctx, pixFinal);

        char name[16]; std::snprintf(name, sizeof(name), "Im%d", i);
        if (create_page_fill(name, dw_pt, dh_pt, pageImg)) ++pages_added;
        else std::cerr << "  ! create_page failed\n";
    }

    if (!pages_added) { std::cerr << "No pages added; not saving.\n"; goto finish; }

    std::cout << "\nSaving: " << out_pdf << "\n";
    try { pdf_save_document(ctx, doc_dest, out_pdf.c_str(), &saveopt); }
    catch (...) { std::cerr << "  ! save failed\n"; }

finish:
    if (doc_dest) pdf_drop_document(ctx, doc_dest);
    fz_flush_warnings(ctx);
    if (ctx)      fz_drop_context(ctx);
    std::cout << "complete\n";
    return 0;
}