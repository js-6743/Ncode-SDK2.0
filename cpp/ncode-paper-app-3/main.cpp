#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>

#include "main.h"

namespace fs = std::filesystem;

// ---------------- MuPDF globals ----------------
static fz_context* ctx = nullptr;
static pdf_document* doc_dest = nullptr;

// ---------------- helpers ----------------------

static std::string get_exe_directory()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string exe_path(path);
    size_t pos = exe_path.find_last_of("\\/");
    return (pos != std::string::npos) ? exe_path.substr(0, pos) : "";
}

static std::string base_name_no_ext(const std::string& path)
{
    size_t s = path.find_last_of("\\/");
    size_t e = path.find_last_of('.');
    if (e == std::string::npos || e < s) e = path.size();
    return path.substr(s == std::string::npos ? 0 : s + 1,
        e - (s == std::string::npos ? 0 : s + 1));
}

static std::string find_pattern_pdf(const std::string& dir, const std::string& prefix)
{
    try {
        fs::path pdir(dir);
        if (!fs::exists(pdir) || !fs::is_directory(pdir)) {
            printf("Pattern dir not found: %s\n", dir.c_str());
            return {};
        }
        std::string best;
        for (const auto& e : fs::directory_iterator(pdir)) {
            if (!e.is_regular_file()) continue;
            auto name = e.path().filename().string();
            std::string lower = name;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            if (lower.size() >= 4 && lower.rfind(".pdf") == lower.size() - 4) {
                if (name.rfind(prefix, 0) == 0) {
                    if (best.empty() || name < fs::path(best).filename().string())
                        best = e.path().string();
                }
            }
        }
        return best;
    }
    catch (const std::exception& ex) {
        printf("find_pattern_pdf error: %s\n", ex.what());
        return {};
    }
}

// Add image to resources (copied from original sample)
static void add_image_res(pdf_obj* resources, const char* name, fz_image* image)
{
    pdf_obj* subres = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
    if (!subres) {
        subres = pdf_new_dict(ctx, doc_dest, 10);
        pdf_dict_put_drop(ctx, resources, PDF_NAME(XObject), subres);
    }

    pdf_obj* ref = pdf_add_image(ctx, doc_dest, image);
    pdf_dict_puts(ctx, subres, name, ref);
    pdf_drop_obj(ctx, ref);

    fz_drop_image(ctx, image);
}

// Make content stream (copied from original sample)
static void make_contents(fz_buffer* contents, const char* name, int w, int h)
{
    char line[4096];

    fz_append_string(ctx, contents, "q");
    fz_append_byte(ctx, contents, '\n');

    sprintf(line, "%d 0 0 %d 0 0 cm", w, h);
    fz_append_string(ctx, contents, line);
    fz_append_byte(ctx, contents, '\n');

    sprintf(line, "/%s Do", name);
    fz_append_string(ctx, contents, line);
    fz_append_byte(ctx, contents, '\n');

    fz_append_string(ctx, contents, "Q");
    fz_append_byte(ctx, contents, '\n');
}

// Create page (copied from original sample)
static void create_page(const char* name, int w, int h, fz_image* image)
{
    int rotate = 0;

    fz_rect mediabox = fz_make_rect(0, 0, (float)w, (float)h);
    pdf_obj* resources;
    pdf_obj* page;
    fz_buffer* contents;

    // make resource
    resources = pdf_new_dict(ctx, doc_dest, 2);
    add_image_res(resources, name, image);

    // make contents
    contents = fz_new_buffer(ctx, 1024);
    make_contents(contents, name, w, h);

    // make and insert page
    page = pdf_add_page(ctx, doc_dest, mediabox, rotate, resources, contents);
    pdf_insert_page(ctx, doc_dest, -1, page);

    // release
    pdf_drop_obj(ctx, page);
    fz_drop_buffer(ctx, contents);
    pdf_drop_obj(ctx, resources);
}

// ----------------------------- main -----------------------------

int main(int argc, char** argv)
{
    const char* graphics_pdf = (argc >= 2) ? argv[1] : "1_ROCF_C.pdf";
    
    // Default to looking for "Ncode paper" folder in the same directory as the exe
    std::string default_pattern_dir = get_exe_directory() + "\\Ncode paper";
    std::string pattern_dir = (argc >= 3) ? argv[2] : default_pattern_dir;

    printf("Input graphics: %s\n", graphics_pdf);
    printf("Pattern dir   : %s\n\n", pattern_dir.c_str());

    printf("1) Initializing context, handler, pdf document\n\n");
    pdf_write_options opts = { 0 };

    ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx) {
        printf("Cannot initialize context\n\n");
        return 1;
    }

    // Use compression to reduce file size
    pdf_parse_write_options(ctx, &opts, "compress");
    fz_register_document_handlers(ctx);
    doc_dest = pdf_create_document(ctx);

    printf("2) Loading source pdf file\n\n");
    fz_document* doc_src = nullptr;
    fz_try(ctx) {
        doc_src = fz_open_document(ctx, graphics_pdf);
    }
    fz_catch(ctx) {
        printf("Failed to open graphics PDF: %s\n", graphics_pdf);
        if (doc_dest) pdf_drop_document(ctx, doc_dest);
        fz_flush_warnings(ctx);
        if (ctx) fz_drop_context(ctx);
        return 1;
    }

    printf("3) Making Ncoded pages\n\n");
    int pageCount = fz_count_pages(ctx, doc_src);
    int dpi = 600;

    // A4 dimensions in points (210mm x 297mm)
    const float A4_WIDTH_PTS = 595.0f;
    const float A4_HEIGHT_PTS = 842.0f;

    for (int i = 0; i < pageCount; ++i)
    {
        printf("   3-1(page %d)) Loading source page and Ncode pattern\n", i + 1);

        fz_page* page = nullptr;
        fz_matrix ctm = fz_identity;

        fz_try(ctx) {
            page = fz_load_page(ctx, doc_src, i);
        }
        fz_catch(ctx) {
            printf("Failed to load page %d\n", i + 1);
            continue;
        }

        // Use A4 dimensions - render at exact pixel size
        float scale = dpi / 72.0f;
        int target_w = (int)(A4_WIDTH_PTS * scale);  // Should be 4958
        int target_h = (int)(A4_HEIGHT_PTS * scale);  // Should be 7017

        printf("   Target dimensions: %d x %d pixels\n", target_w, target_h);

        // Render background (RGB) and allocate CMYK destination
        fz_pixmap* pixSrc = nullptr;
        fz_pixmap* pixRemovedK = nullptr;

        fz_try(ctx) {
            // Create a custom matrix to render at EXACT pixel dimensions
            fz_rect page_rect = fz_bound_page(ctx, page);
            float scale_x = target_w / page_rect.x1;
            float scale_y = target_h / page_rect.y1;
            fz_matrix exact_ctm = fz_scale(scale_x, scale_y);

            // Render background at exact size
            pixSrc = fz_new_pixmap_from_page_contents(ctx, page, exact_ctm, fz_device_rgb(ctx), 0);

            printf("   Background rendered: %d x %d pixels\n", pixSrc->w, pixSrc->h);

            // Create CMYK pixmap with same dimensions as source
            pixRemovedK = fz_new_pixmap(ctx, fz_device_cmyk(ctx), pixSrc->w, pixSrc->h, NULL, 0);
        }
        fz_catch(ctx) {
            printf("Failed to render background on page %d\n", i + 1);
            if (page) fz_drop_page(ctx, page);
            if (pixSrc) fz_drop_pixmap(ctx, pixSrc);
            if (pixRemovedK) fz_drop_pixmap(ctx, pixRemovedK);
            continue;
        }

        // Find and load pattern PDF
        char prefix[16];
        _snprintf_s(prefix, _TRUNCATE, "%d_", i + 1);
        std::string pat_path = find_pattern_pdf(pattern_dir, prefix);

        if (pat_path.empty()) {
            printf("Pattern not found for page %d (looking for %s*.pdf)\n", i + 1, prefix);
            fz_drop_pixmap(ctx, pixRemovedK);
            fz_drop_pixmap(ctx, pixSrc);
            fz_drop_page(ctx, page);
            continue;
        }
        printf("   Pattern: %s\n", pat_path.c_str());

        // Render pattern as GRAY at A4 size
        fz_document* pat_doc = nullptr;
        fz_page* pat_pg = nullptr;
        fz_pixmap* pixPatG = nullptr;

        fz_try(ctx) {
            pat_doc = fz_open_document(ctx, pat_path.c_str());
            pat_pg = fz_load_page(ctx, pat_doc, 0);

            // Force pattern to render at SAME size as background
            fz_rect pat_rect = fz_bound_page(ctx, pat_pg);
            float pat_scale_x = pixSrc->w / pat_rect.x1;
            float pat_scale_y = pixSrc->h / pat_rect.y1;
            fz_matrix pat_ctm = fz_scale(pat_scale_x, pat_scale_y);

            pixPatG = fz_new_pixmap_from_page_contents(ctx, pat_pg, pat_ctm, fz_device_gray(ctx), 0);

            printf("   Pattern rendered: %d x %d pixels\n", pixPatG->w, pixPatG->h);
        }
        fz_catch(ctx) {
            printf("Failed to render pattern on page %d\n", i + 1);
            if (pixPatG) fz_drop_pixmap(ctx, pixPatG);
            if (pat_pg) fz_drop_page(ctx, pat_pg);
            if (pat_doc) fz_drop_document(ctx, pat_doc);
            fz_drop_pixmap(ctx, pixRemovedK);
            fz_drop_pixmap(ctx, pixSrc);
            fz_drop_page(ctx, page);
            continue;
        }

        printf("   3-2(page %d)) Combining background image(removed K) and Ncode pattern\n", i + 1);

        // Verify dimensions match
        if (pixSrc->w != pixPatG->w || pixSrc->h != pixPatG->h) {
            printf("   ERROR: Background (%dx%d) and pattern (%dx%d) sizes STILL DON'T MATCH!\n",
                pixSrc->w, pixSrc->h, pixPatG->w, pixPatG->h);
            printf("   Aborting this page to avoid corruption.\n");

            fz_drop_pixmap(ctx, pixPatG);
            fz_drop_page(ctx, pat_pg);
            fz_drop_document(ctx, pat_doc);
            fz_drop_pixmap(ctx, pixSrc);
            fz_drop_pixmap(ctx, pixRemovedK);
            fz_drop_page(ctx, page);
            continue;
        }

        printf("   Dimensions match perfectly: %d x %d pixels\n", pixSrc->w, pixSrc->h);

        // Compose pixels - same as original sample
        const unsigned char TH = 128;

        for (int y = 0; y < pixSrc->h; ++y) {
            for (int x = 0; x < pixSrc->w; ++x) {
                unsigned char* p_src = &pixSrc->samples[(y * pixSrc->w + x) * pixSrc->n];
                unsigned char* p_pat = &pixPatG->samples[(y * pixPatG->w + x) * pixPatG->n];
                unsigned char* p_dest = &pixRemovedK->samples[(y * pixSrc->w + x) * pixRemovedK->n];

                // draw Ncode dot
                if (p_pat[0] < TH) {
                    p_dest[0] = 0;       // C
                    p_dest[1] = 0;       // M
                    p_dest[2] = 0;       // Y
                    p_dest[3] = 255;     // K
                }
                // draw background with removed K
                else {
                    p_dest[0] = 255 - p_src[0];  // C
                    p_dest[1] = 255 - p_src[1];  // M
                    p_dest[2] = 255 - p_src[2];  // Y
                    p_dest[3] = 0;               // K = 0
                }
            }
        }

        // final Ncoded pixmap -> image
        fz_image* imageNcoded = fz_new_image_from_pixmap(ctx, pixRemovedK, NULL);

        printf("   3-3(page %d)) Inserting Ncoded image to destination pdf file\n", i + 1);
        char id[100];
        sprintf(id, "I%d", i);
        create_page(id, (int)A4_WIDTH_PTS, (int)A4_HEIGHT_PTS, imageNcoded);

        // release objects
        fz_drop_pixmap(ctx, pixPatG);
        fz_drop_page(ctx, pat_pg);
        fz_drop_document(ctx, pat_doc);
        fz_drop_pixmap(ctx, pixSrc);
        fz_drop_pixmap(ctx, pixRemovedK);
        fz_drop_page(ctx, page);

        printf("\n");
    }

    printf("4) Saving Ncoded pdf file\n\n");
    std::string out = base_name_no_ext(graphics_pdf) + "_ncode.pdf";
    pdf_save_document(ctx, doc_dest, out.c_str(), &opts);

    printf("Saved: %s\n", out.c_str());

    // Release objects
    if (doc_dest) pdf_drop_document(ctx, doc_dest);
    if (doc_src) fz_drop_document(ctx, doc_src);
    fz_flush_warnings(ctx);
    if (ctx) fz_drop_context(ctx);

    printf("complete\n");
    return 0;
}