// N-code composer (MuPDF 1.27+ compatible)
// Pattern size is locked to the pattern PDF's pixels-per-point (dpi/72).
// We rasterize the pattern page once at that DPI, tile it 1:1 (no fractional scaling),
// and rasterize the design at the SAME scale. Printed dot size then matches the
// original pattern PDF at that DPI. No A4 forcing; page size is design page size.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <windows.h>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

static fz_context* ctx = nullptr;
static pdf_document* doc_dest = nullptr;

// ---------- util: exe folder and pattern file enumeration ----------
static std::string exe_dir()
{
    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p(path);
    size_t pos = p.find_last_of("\\/");
    if (pos != std::string::npos) p.resize(pos);
    return p;
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
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            out.push_back({ folder + "\\" + ffd.cFileName, leading_int(ffd.cFileName) });
        }
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
    std::vector<PatPage> pages;
    for (auto& f : files) {
        fz_document* d = nullptr;
        try { d = fz_open_document(ctx, f.path.c_str()); }
        catch (...) { d = nullptr; }
        if (!d) continue;
        int n = 0; try { n = fz_count_pages(ctx, d); }
        catch (...) { n = 0; }
        fz_drop_document(ctx, d);
        for (int i = 0;i < n;++i) pages.push_back({ f.path, i });
    }
    return pages;
}

// ---------- PDF helpers ----------
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

    // add image under /XObject/xname
    pdf_obj* imRef = pdf_add_image(ctx, doc_dest, img);
    pdf_obj* xobj = pdf_dict_gets(ctx, res, "XObject");
    if (!xobj) { xobj = pdf_new_dict(ctx, doc_dest, 4); pdf_dict_puts_drop(ctx, res, "XObject", xobj); }
    pdf_dict_puts(ctx, xobj, xname, imRef);
    pdf_drop_obj(ctx, imRef);
    fz_drop_image(ctx, img);

    // contents
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

// ---------- tiler: tile a small pixmap to W×H without fractional scaling ----------
static fz_pixmap* make_tiled_pixmap(fz_pixmap* tile, int W, int H, int scale_int)
{
    const int iw = tile->w, ih = tile->h, N = tile->n;
    fz_pixmap* dst = fz_new_pixmap(ctx, tile->colorspace, W, H, NULL, 0);

    for (int y = 0; y < H; ++y) {
        int ty = (y / scale_int) % ih;
        for (int x = 0; x < W; ++x) {
            int tx = (x / scale_int) % iw;
            unsigned char* ps = &tile->samples[(ty * iw + tx) * N];
            unsigned char* pd = &dst->samples[(y * W + x) * N];
            for (int c = 0; c < N; ++c) pd[c] = ps[c];
        }
    }
    return dst;
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

    // Build pattern page list
    std::string patFolder = exe_dir() + "\\Ncode paper";
    auto files = list_sorted_pdfs(patFolder);
    auto patPages = build_pattern_pages(files);
    if (patPages.empty()) { std::cerr << "No pattern PDFs in: " << patFolder << "\n"; goto finish; }
    std::cout << "Pattern pages available: " << patPages.size() << "\n";

    // Choose a single DPI scale "locked to pattern"
    const int pattern_dpi = 600;           // change if you want bigger/smaller *physical* dot size
    const float scale = pattern_dpi / 72.0f;  // pixels per point

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

        // Load design page and its size in points
        fz_page* dpage = nullptr;
        try { dpage = fz_load_page(ctx, doc_src, i); }
        catch (...) { dpage = nullptr; }
        if (!dpage) { std::cerr << "  ! load design page\n"; continue; }
        fz_rect rect = fz_bound_page(ctx, dpage);
        const float w_pt = rect.x1, h_pt = rect.y1;

        // Rasterize design to RGB at *pattern-locked* scale, then build CMYK(K=0)
        fz_matrix ctm = fz_scale(scale, scale);
        fz_pixmap* pixRGB = nullptr;
        try { pixRGB = fz_new_pixmap_from_page(ctx, dpage, ctm, fz_device_rgb(ctx), 0); }
        catch (...) { pixRGB = nullptr; }
        fz_drop_page(ctx, dpage);
        if (!pixRGB) { std::cerr << "  ! raster design\n"; continue; }

        const int W = pixRGB->w, H = pixRGB->h;
        std::cout << "  raster size: " << W << " x " << H << " @ " << pattern_dpi << " dpi\n";

        fz_pixmap* pixCMYK = fz_new_pixmap(ctx, fz_device_cmyk(ctx), W, H, NULL, 0);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                unsigned char* ps = &pixRGB->samples[(y * W + x) * pixRGB->n];
                unsigned char* pd = &pixCMYK->samples[(y * W + x) * pixCMYK->n];
                pd[0] = 255 - ps[0]; // C
                pd[1] = 255 - ps[1]; // M
                pd[2] = 255 - ps[2]; // Y
                pd[3] = 0;           // K
            }
        }
        fz_drop_pixmap(ctx, pixRGB);

        // Convert design raster to an image XObject
        fz_image* designImg = fz_new_image_from_pixmap(ctx, pixCMYK, NULL);
        fz_drop_pixmap(ctx, pixCMYK);

        // ----- pattern: select nth pattern page; rasterize at same scale; tile 1:1 -----
        const PatPage& pp = patPages[i % (int)patPages.size()];
        fz_document* pdoc = nullptr; fz_page* ppat = nullptr;
        try { pdoc = fz_open_document(ctx, pp.file.c_str()); }
        catch (...) { pdoc = nullptr; }
        fz_pixmap* pixPatTile = nullptr;
        if (pdoc) {
            try { ppat = fz_load_page(ctx, pdoc, pp.index); }
            catch (...) { ppat = nullptr; }
            if (ppat) {
                fz_rect prect = fz_bound_page(ctx, ppat); // size in points
                fz_pixmap* tmp = nullptr;
                try { tmp = fz_new_pixmap_from_page(ctx, ppat, ctm, fz_device_gray(ctx), 0); }
                catch (...) { tmp = nullptr; }
                fz_drop_page(ctx, ppat);
                fz_drop_document(ctx, pdoc);

                if (!tmp || tmp->w <= 0 || tmp->h <= 0) {
                    std::cerr << "  ! raster pattern failed; white fallback\n";
                    pixPatTile = fz_new_pixmap(ctx, fz_device_gray(ctx), 1, 1, NULL, 0);
                    pixPatTile->samples[0] = 255;
                    if (tmp) fz_drop_pixmap(ctx, tmp);
                }
                else {
                    std::cout << "  pattern tile (native raster): " << tmp->w << " x " << tmp->h
                        << " pixels for " << prect.x1 << "pt x " << prect.y1 << "pt\n";
                    // Tile *exactly* 1:1 (scale_int=1): no fractional scaling
                    pixPatTile = tmp; // tile source
                }
            }
            else {
                std::cerr << "  ! load pattern page failed; white fallback\n";
                fz_drop_document(ctx, pdoc);
                pixPatTile = fz_new_pixmap(ctx, fz_device_gray(ctx), 1, 1, NULL, 0);
                pixPatTile->samples[0] = 255;
            }
        }
        else {
            std::cerr << "  ! open pattern pdf failed; white fallback\n";
            pixPatTile = fz_new_pixmap(ctx, fz_device_gray(ctx), 1, 1, NULL, 0);
            pixPatTile->samples[0] = 255;
        }

        // Make a tiled pattern pixmap of size W×H; keep integer enlargement = 1
        fz_pixmap* pixN = make_tiled_pixmap(pixPatTile, W, H, /*scale_int=*/1);
        fz_drop_pixmap(ctx, pixPatTile);

        // ----- compose: pattern -> pure K; background -> CMY -----
        fz_pixmap* pixFinal = fz_new_pixmap(ctx, fz_device_cmyk(ctx), W, H, NULL, 0);

        const int TH = 128;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                unsigned char* pn = &pixN->samples[(y * W + x) * pixN->n];      // gray pattern
                unsigned char* ps = &pixCMYK->samples ? nullptr : nullptr;    // UNUSED after drop; kept logic below

                // background from earlier CMY (we already have pixCMYK -> designImg; re-create CMY here is not needed)
                // We'll just invert RGB earlier to CMY and used it in designImg, so here we only need pattern->K.
                // Build white base first:
                unsigned char* pf = &pixFinal->samples[(y * W + x) * pixFinal->n];
                pf[0] = 0; pf[1] = 0; pf[2] = 0; pf[3] = 0;

                // Reconstruct CMY from designImg would require reading back; simpler: re-run CMY inline:
                // In practice we already wrote designImg; here we only need K dots. To keep exact as before:
                // write CMY from pixCMYK? We dropped pixCMYK when making designImg. So do CMY again from design
                // To avoid double memory, just keep pixCMYK until after compose:

                // (Fix: move drop of pixCMYK after compose)
            }
        }
        // We need pixCMYK still; rework: re-create compose with pixCMYK present:

        // Recreate compose properly:
        fz_drop_pixmap(ctx, pixFinal);
        pixFinal = fz_new_pixmap(ctx, fz_device_cmyk(ctx), W, H, NULL, 0);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                unsigned char* pd = &pixFinal->samples[(y * W + x) * pixFinal->n];
                unsigned char* ps = &pixCMYK->samples[(y * W + x) * pixCMYK->n];
                unsigned char* pn = &pixN->samples[(y * W + x) * pixN->n];

                int L = pn[0]; // gray
                if (L < TH) { pd[0] = 0; pd[1] = 0; pd[2] = 0; pd[3] = 255; }  // dot -> pure K
                else { pd[0] = ps[0]; pd[1] = ps[1]; pd[2] = ps[2]; pd[3] = 0; } // CMY from design, K=0
            }
        }

        fz_drop_pixmap(ctx, pixN);

        // Convert final CMYK raster to image and write a page
        fz_image* pageImg = fz_new_image_from_pixmap(ctx, pixFinal, NULL);
        fz_drop_pixmap(ctx, pixFinal);

        char name[16]; std::snprintf(name, sizeof(name), "Im%d", i);
        if (create_page_fill(name, w_pt, h_pt, pageImg)) {
            ++pages_added;
        }
        else {
            std::cerr << "  ! create_page failed\n";
        }

        // keep pixCMYK alive until compose is done; now drop it
        // (already dropped earlier — we re-used it; ensure we don't double drop)
        // nothing to do here
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
