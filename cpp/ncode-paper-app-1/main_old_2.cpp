// N-code composer (MuPDF 1.27+), original strategy + Ncode tiling (no fractional scaling)
// - Page size (points) comes from the source page
// - Raster size (pixels) = page points * (dpi / 72)
// - Background & destination rasters share the same pixel size
// - Ncode pattern is tiled at its native pixels (or integer-enlarged) ? no clipped squares
// - Final image is drawn to fill the page using *points*

#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>
#include <algorithm>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

static fz_context* ctx = nullptr;
static pdf_document* doc_dest = nullptr;

// ---- Add image as XObject under /Resources/XObject ----
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

// ---- draw final image to fill page in *points* ----
static void make_contents(fz_buffer* contents, const char* xname, float w_pt, float h_pt)
{
    char line[128];
    fz_append_string(ctx, contents, "q\n");
    std::snprintf(line, sizeof(line), "%.3f 0 0 %.3f 0 0 cm\n", w_pt, h_pt); // scale to page size (points)
    fz_append_string(ctx, contents, line);
    std::snprintf(line, sizeof(line), "/%s Do\n", xname);
    fz_append_string(ctx, contents, line);
    fz_append_string(ctx, contents, "Q\n");
}

// ---- create a page whose mediabox is in *points* ----
static bool create_page(const char* xname, float w_pt, float h_pt, fz_image* image)
{
    fz_rect mediabox = fz_make_rect(0, 0, w_pt, h_pt);
    pdf_obj* res = pdf_new_dict(ctx, doc_dest, 4);
    add_image_res(res, xname, image);

    fz_buffer* contents = fz_new_buffer(ctx, 2048);
    make_contents(contents, xname, w_pt, h_pt);

    pdf_obj* page = pdf_add_page(ctx, doc_dest, mediabox, 0, res, contents);
    if (!page) {
        std::cerr << "  ! pdf_add_page returned null\n";
        fz_drop_buffer(ctx, contents);
        pdf_drop_obj(ctx, res);
        return false;
    }
    pdf_insert_page(ctx, doc_dest, -1, page);

    pdf_drop_obj(ctx, page);
    fz_drop_buffer(ctx, contents);
    pdf_drop_obj(ctx, res);
    return true;
}

// ---- tile helper: repeat a small pixmap to fill W×H; scale_int is 1,2,3,... (integer enlarge) ----
static fz_pixmap* make_tiled_pixmap(fz_pixmap* tile, int W, int H, int scale_int)
{
    const int iw = tile->w, ih = tile->h, N = tile->n;
    fz_pixmap* dst = fz_new_pixmap(ctx, tile->colorspace, W, H, NULL, 0);

    for (int y = 0; y < H; ++y) {
        int ty = (y / scale_int) % ih; // integer enlarge, then repeat
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

    const std::string input_filename = argv[1];
    std::string       output_filename = input_filename;
    size_t pos = output_filename.rfind(".pdf");
    output_filename = (pos != std::string::npos)
        ? output_filename.insert(pos, "_ncode")
        : output_filename + "_ncode.pdf";

    // ---- init MuPDF ----
    ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx) { std::cerr << "Cannot init MuPDF context\n"; return 1; }
    fz_register_document_handlers(ctx);
    pdf_write_options opts{}; pdf_parse_write_options(ctx, &opts, "compress");
    doc_dest = pdf_create_document(ctx);

    // ---- open source ----
    fz_document* doc_src = nullptr;
    fz_try(ctx) { doc_src = fz_open_document(ctx, input_filename.c_str()); }
    fz_catch(ctx) { std::cerr << "Cannot open: " << input_filename << "\n"; goto finish; }

    int pageCount = 0;
    fz_try(ctx) { pageCount = fz_count_pages(ctx, doc_src); }
    fz_catch(ctx) { std::cerr << "Cannot count pages\n"; goto finish; }

    // ---- DPI (change here if needed) ----
    const int   dpi = 1200;          // original used 600
    const float scale = dpi / 72.0f;   // px-per-point

    int pages_added = 0;

    for (int i = 0; i < pageCount; ++i)
    {
        std::cout << "Page " << (i + 1) << "/" << pageCount << "\n";

        // 1) page size in points
        fz_page* page = nullptr;
        fz_try(ctx) { page = fz_load_page(ctx, doc_src, i); }
        fz_catch(ctx) { std::cerr << "  ! load_page failed\n"; continue; }

        fz_rect rect = fz_bound_page(ctx, page);
        const float w_pt = rect.x1;
        const float h_pt = rect.y1;

        // 2) rasterize background at dpi (RGB)
        fz_matrix ctm = fz_scale(scale, scale);
        fz_pixmap* pixSrc = nullptr;
        fz_try(ctx) { pixSrc = fz_new_pixmap_from_page(ctx, page, ctm, fz_device_rgb(ctx), 0); }
        fz_catch(ctx) { std::cerr << "  ! raster bg failed\n"; fz_drop_page(ctx, page); continue; }
        fz_drop_page(ctx, page);

        const int W = pixSrc->w, H = pixSrc->h;
        std::cout << "  raster size: " << W << " x " << H << "\n";

        // 3) destination CMYK pixmap (same pixel size)
        fz_pixmap* pixDst = fz_new_pixmap(ctx, fz_device_cmyk(ctx), W, H, NULL, 0);

        // 4) load Ncode at native pixels, tile to W×H (no fractional scaling)
        fz_image* imgN = nullptr;
        fz_pixmap* ncodeTile = nullptr;
        fz_pixmap* pixN = nullptr;

        fz_try(ctx) {
            imgN = fz_new_image_from_file(ctx, "Ncode_0.png");

            // rasterize PNG at its native size first
            int iw = 0, ih = 0;
            fz_pixmap* tmp = fz_get_pixmap_from_image(ctx, imgN, NULL, NULL, &iw, &ih);
            if (!tmp) {
                // fallback: white
                ncodeTile = fz_new_pixmap(ctx, fz_device_gray(ctx), 1, 1, NULL, 0);
                ncodeTile->samples[0] = 255;
            }
            else {
                ncodeTile = tmp; // native pixels of Ncode
            }

            // INTEGER enlargement for the tile so shapes remain crisp (1 = keep exact native size)
            int scale_int = 1; // try 1; use 2 or 3 if you want larger squares (still crisp)
            pixN = make_tiled_pixmap(ncodeTile, W, H, scale_int);
        }
        fz_catch(ctx) {
            std::cerr << "  ! Ncode_0.png missing - white fallback\n";
            pixN = fz_new_pixmap(ctx, fz_device_gray(ctx), W, H, NULL, 0);
            std::memset(pixN->samples, 255, (size_t)W * H * pixN->n);
        }

        if (imgN)      fz_drop_image(ctx, imgN);
        if (ncodeTile) fz_drop_pixmap(ctx, ncodeTile);

        std::cout << "  tiled Ncode: " << pixN->w << " x " << pixN->h << "\n";

        // 5) compose: pattern ? pure K; background ? CMY from inverted RGB
        const int TH = 128;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                unsigned char* pd = &pixDst->samples[(y * W + x) * pixDst->n];
                unsigned char* ps = &pixSrc->samples[(y * W + x) * pixSrc->n];
                unsigned char* pn = &pixN->samples[(y * W + x) * pixN->n];

                // luminance proxy from Ncode
                int L = (pixN->n >= 3)
                    ? (int)(0.2126 * pn[0] + 0.7152 * pn[1] + 0.0722 * pn[2])
                    : pn[0];

                if (L < TH) {                         // draw dot in pure K
                    pd[0] = 0; pd[1] = 0; pd[2] = 0; pd[3] = 255;
                }
                else {                               // background CMY = inverted RGB; K=0
                    pd[0] = 255 - ps[0];               // C
                    pd[1] = 255 - ps[1];               // M
                    pd[2] = 255 - ps[2];               // Y
                    pd[3] = 0;                          // K
                }
            }
        }

        // 6) Turn CMYK raster into a page image and draw it to fill the page (points)
        fz_image* pageImage = fz_new_image_from_pixmap(ctx, pixDst, NULL);
        char xname[32]; std::snprintf(xname, sizeof(xname), "I%d", i);

        if (create_page(xname, w_pt, h_pt, pageImage))
            ++pages_added;
        else
            std::cerr << "  ! create_page failed\n";

        // cleanup
        fz_drop_pixmap(ctx, pixN);
        fz_drop_pixmap(ctx, pixSrc);
        fz_drop_pixmap(ctx, pixDst);
    }

    if (pages_added == 0) {
        std::cerr << "No pages added. Not writing output.\n";
        goto finish;
    }

    std::cout << "Saving: " << output_filename << "\n";
    fz_try(ctx) { pdf_save_document(ctx, doc_dest, output_filename.c_str(), &opts); }
    fz_catch(ctx) { std::cerr << "pdf_save_document failed\n"; }

finish:
    if (doc_dest) pdf_drop_document(ctx, doc_dest);
    if (doc_src)  fz_drop_document(ctx, doc_src);
    fz_flush_warnings(ctx);
    if (ctx)      fz_drop_context(ctx);
    std::cout << "complete\n";
    return 0;
}
