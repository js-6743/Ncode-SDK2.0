#include <windows.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "../sampleApp(mupdf)/mupdf/fitz.h"
#include "../sampleApp(mupdf)/mupdf/pdf.h"

// A4 size in points (1/72 inch)
static const int A4_WIDTH_PT = 595;   // 210mm
static const int A4_HEIGHT_PT = 842;  // 297mm
static const int TARGET_DPI = 600;

static fz_context *g_ctx = NULL;
static pdf_document *g_doc_dest = NULL;

static void add_image_res(pdf_obj *resources, const char *name, fz_image *image)
{
    pdf_obj *subres, *ref;

    subres = pdf_dict_get(g_ctx, resources, PDF_NAME_XObject);
    if (!subres)
    {
        subres = pdf_new_dict(g_ctx, g_doc_dest, 10);
        pdf_dict_put_drop(g_ctx, resources, PDF_NAME_XObject, subres);
    }

    ref = pdf_add_image(g_ctx, g_doc_dest, image, 0);
    pdf_dict_puts(g_ctx, subres, name, ref);
    pdf_drop_obj(g_ctx, ref);

    fz_drop_image(g_ctx, image);
}

static void make_contents(fz_buffer* contents, const char *name, int w, int h)
{
    char line[256];

    fz_append_string(g_ctx, contents, "q\n");
    std::snprintf(line, sizeof(line), "%d 0 0 %d 0 0 cm", w, h);
    fz_append_string(g_ctx, contents, line);
    fz_append_string(g_ctx, contents, "\n");
    std::snprintf(line, sizeof(line), "/%s Do", name);
    fz_append_string(g_ctx, contents, line);
    fz_append_string(g_ctx, contents, "\nQ\n");
}

static void create_page(const char *name, int w, int h, fz_image *image)
{
    int rotate = 0;
    fz_rect mediabox = { 0, 0, (float)A4_WIDTH_PT, (float)A4_HEIGHT_PT };
    pdf_obj *resources;
    pdf_obj *page;
    fz_buffer *contents;

    resources = pdf_new_dict(g_ctx, g_doc_dest, 2);
    add_image_res(resources, name, image);

    contents = fz_new_buffer(g_ctx, 1024);
    make_contents(contents, name, w, h);

    page = pdf_add_page(g_ctx, g_doc_dest, &mediabox, rotate, resources, contents);
    pdf_insert_page(g_ctx, g_doc_dest, -1, page);

    pdf_drop_obj(g_ctx, page);
    fz_drop_buffer(g_ctx, contents);
    pdf_drop_obj(g_ctx, resources);
}

static std::vector<std::string> list_pdf_files(const std::string &dir)
{
    std::vector<std::string> files;
    std::string pattern = dir + "\\*.pdf";
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE)
        return files;

    do {
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            files.emplace_back(dir + "\\" + ffd.cFileName);
        }
    } while (FindNextFileA(hFind, &ffd));
    FindClose(hFind);

    std::sort(files.begin(), files.end());
    return files;
}

static std::string path_dirname(const std::string &p)
{
    char buf[MAX_PATH];
    lstrcpynA(buf, p.c_str(), MAX_PATH);
    PathRemoveFileSpecA(buf);
    return std::string(buf);
}

static std::string path_basename_noext(const std::string &p)
{
    char fname[_MAX_FNAME];
    char ext[_MAX_EXT];
    _splitpath_s(p.c_str(), nullptr, 0, nullptr, 0, fname, _MAX_FNAME, ext, _MAX_EXT);
    return std::string(fname);
}

static std::string path_join(const std::string &a, const std::string &b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

int main(int argc, char **argv)
{
    printf("Ncoded PDF generator (A4, MuPDF overlay)\n\n");
    if (argc < 2)
    {
        printf("Usage: ncode-paper-app-2.exe <background.pdf>\n");
        return 2;
    }

    std::string bg_path = argv[1];
    char full_bg[MAX_PATH];
    if (GetFullPathNameA(bg_path.c_str(), MAX_PATH, full_bg, NULL) == 0)
    {
        printf("Cannot resolve input path: %s\n", bg_path.c_str());
        return 2;
    }
    bg_path = full_bg;

    // Pattern folder: <this exe dir>\\Ncode paper
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string exe_dir = path_dirname(exe_path);
    std::string pattern_dir = path_join(exe_dir, "Ncode paper");

    std::vector<std::string> pattern_files = list_pdf_files(pattern_dir);
    if (pattern_files.empty())
    {
        printf("No pattern PDFs found in: %s\n", pattern_dir.c_str());
        return 3;
    }

    pdf_write_options opts = { 0 };
    g_ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
    if (!g_ctx)
    {
        printf("Cannot initialize MuPDF context\n");
        return 1;
    }

    fz_try(g_ctx)
    {
        pdf_parse_write_options(g_ctx, &opts, "compress");
        fz_register_document_handlers(g_ctx);

        g_doc_dest = pdf_create_document(g_ctx);

        printf("Opening background: %s\n", bg_path.c_str());
        fz_document* doc_src = fz_open_document(g_ctx, bg_path.c_str());

        // Open all pattern PDFs and cache page counts
        struct PattDoc { std::string path; fz_document* doc; int pages; };
        std::vector<PattDoc> patt_docs;
        patt_docs.reserve(pattern_files.size());
        int total_patt_pages = 0;
        for (auto &pf : pattern_files)
        {
            PattDoc pd; pd.path = pf; pd.doc = fz_open_document(g_ctx, pf.c_str());
            pd.pages = pd.doc->count_pages(g_ctx, pd.doc);
            patt_docs.push_back(pd);
            total_patt_pages += pd.pages;
        }

        int bg_pages = doc_src->count_pages(g_ctx, doc_src);
        if (total_patt_pages < bg_pages)
        {
            printf("Insufficient pattern pages: have %d, need %d\n", total_patt_pages, bg_pages);
            fz_throw(g_ctx, FZ_ERROR_GENERIC, "not enough pattern pages");
        }

        // Target pixel size for A4 at TARGET_DPI
        const float scale_dpi = TARGET_DPI / 72.0f;
        const int target_w = (int)(A4_WIDTH_PT * scale_dpi + 0.5f);
        const int target_h = (int)(A4_HEIGHT_PT * scale_dpi + 0.5f);

        printf("Processing %d page(s); A4 @ %d dpi -> %dx%d pixels\n", bg_pages, TARGET_DPI, target_w, target_h);

        // Iterate background pages and overlay corresponding pattern page
        int patt_doc_index = 0;
        int patt_page_offset = 0;
        for (int i = 0; i < bg_pages; ++i)
        {
            printf("- Page %d: render background and pattern\n", i + 1);

            // Resolve which pattern doc/page corresponds to global page index i
            int remaining = i;
            patt_doc_index = 0;
            while (patt_doc_index < (int)patt_docs.size())
            {
                if (remaining < patt_docs[patt_doc_index].pages)
                {
                    patt_page_offset = remaining;
                    break;
                }
                remaining -= patt_docs[patt_doc_index].pages;
                ++patt_doc_index;
            }

            if (patt_doc_index >= (int)patt_docs.size())
                fz_throw(g_ctx, FZ_ERROR_GENERIC, "pattern page index overflow");

            // Load pages
            fz_page *bg_page = fz_load_page(g_ctx, doc_src, i);
            fz_rect bg_rect; fz_bound_page(g_ctx, bg_page, &bg_rect);

            fz_page *pt_page = fz_load_page(g_ctx, patt_docs[patt_doc_index].doc, patt_page_offset);
            fz_rect pt_rect; fz_bound_page(g_ctx, pt_page, &pt_rect);

            // Compute non-uniform scale to fit A4 pixel size
            fz_matrix m_bg = fz_identity;
            float sx_bg = target_w / (bg_rect.x1 - bg_rect.x0);
            float sy_bg = target_h / (bg_rect.y1 - bg_rect.y0);
            fz_scale(&m_bg, sx_bg, sy_bg);

            fz_matrix m_pt = fz_identity;
            float sx_pt = target_w / (pt_rect.x1 - pt_rect.x0);
            float sy_pt = target_h / (pt_rect.y1 - pt_rect.y0);
            fz_scale(&m_pt, sx_pt, sy_pt);

            // Render background to RGB
            fz_pixmap *pixSrc = fz_new_pixmap_from_page_contents(g_ctx, bg_page, &m_bg, fz_device_rgb(g_ctx), 0);

            // Render pattern to GRAY (single channel) for robust dot detection
            fz_pixmap *pixNcode = fz_new_pixmap_from_page_contents(g_ctx, pt_page, &m_pt, fz_device_gray(g_ctx), 0);

            if (pixSrc->w != target_w || pixSrc->h != target_h || pixNcode->w != target_w || pixNcode->h != target_h)
                fz_throw(g_ctx, FZ_ERROR_GENERIC, "rendered size mismatch");

            // Destination CMYK pixmap
            fz_pixmap *pixRemovedK = fz_new_pixmap(g_ctx, fz_device_cmyk(g_ctx), target_w, target_h, 0);

            // Composite: K=255 for dots; background CMY = 255 - RGB, K=0
            const int th = 16; // threshold for black dot in GRAY
            for (int y = 0; y < target_h; ++y)
            {
                for (int x = 0; x < target_w; ++x)
                {
                    unsigned char *p_src = &pixSrc->samples[(y * pixSrc->w + x) * pixSrc->n];
                    unsigned char *p_pt  = &pixNcode->samples[(y * pixNcode->w + x) * pixNcode->n];
                    unsigned char *p_dst = &pixRemovedK->samples[(y * pixRemovedK->w + x) * pixRemovedK->n];

                    if (p_pt[0] <= th)
                    {
                        p_dst[0] = 0;   // C
                        p_dst[1] = 0;   // M
                        p_dst[2] = 0;   // Y
                        p_dst[3] = 255; // K
                    }
                    else
                    {
                        p_dst[0] = 255 - p_src[0]; // C
                        p_dst[1] = 255 - p_src[1]; // M
                        p_dst[2] = 255 - p_src[2]; // Y
                        p_dst[3] = 0;              // K
                    }
                }
            }

            // Convert to image and insert into destination PDF with A4 mediabox
            fz_image *imageNcoded = fz_new_image_from_pixmap(g_ctx, pixRemovedK, NULL);
            char id[32]; std::snprintf(id, sizeof(id), "I%d", i);
            create_page(id, A4_WIDTH_PT, A4_HEIGHT_PT, imageNcoded);

            // Release
            fz_drop_page(g_ctx, bg_page);
            fz_drop_page(g_ctx, pt_page);
            fz_drop_pixmap(g_ctx, pixSrc);
            fz_drop_pixmap(g_ctx, pixNcode);
            fz_drop_pixmap(g_ctx, pixRemovedK);
            fz_drop_image(g_ctx, imageNcoded);
        }

        // Save output next to input with _ncode suffix
        std::string out_dir = path_dirname(bg_path);
        std::string base = path_basename_noext(bg_path);
        std::string out_name = base + "_ncode.pdf";
        std::string out_path = path_join(out_dir, out_name);
        printf("Saving: %s\n", out_path.c_str());
        pdf_save_document(g_ctx, g_doc_dest, out_path.c_str(), &opts);

        // Cleanup pattern docs
        for (auto &pd : patt_docs)
        {
            fz_drop_document(g_ctx, pd.doc);
        }
        fz_drop_document(g_ctx, doc_src);
    }
    fz_always(g_ctx)
    {
        if (g_doc_dest) { pdf_drop_document(g_ctx, g_doc_dest); g_doc_dest = NULL; }
        if (g_ctx) { fz_drop_context(g_ctx); g_ctx = NULL; }
    }
    fz_catch(g_ctx)
    {
        printf("Failed due to MuPDF error.\n");
        return 1;
    }

    printf("Done.\n");
    return 0;
}

