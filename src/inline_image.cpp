#include "inline_image.h"
#include "epub.h"
#include "display.h"
#include "image_tone.h"
#include "debug_trace.h"
#include "config.h"
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <SD.h>
#include <algorithm>
#include <cstdio>

struct Raw4Ctx {
    int srcW = 0, srcH = 0;
    int dstW = 0, dstH = 0;
    File* out = nullptr;
    uint8_t* pixels = nullptr;
    uint16_t* pngLineBuf = nullptr;
    uint32_t deadlineMs = 0;
    uint32_t lastYieldMs = 0;
    bool aborted = false;
};

static Raw4Ctx* g_raw4_ctx = nullptr;
static PNG* g_png_active = nullptr;

static bool inline_image_maybe_abort_decode() {
    if (!g_raw4_ctx) return false;
    uint32_t now = millis();
    if (g_raw4_ctx->deadlineMs && (int32_t)(now - g_raw4_ctx->deadlineMs) >= 0) {
        g_raw4_ctx->aborted = true;
        return true;
    }
    if (now - g_raw4_ctx->lastYieldMs >= 16) {
        yield();
        g_raw4_ctx->lastYieldMs = now;
    }
    return false;
}

static void raw4_accumulate(int sx, int sy, uint8_t gray4) {
    Raw4Ctx* ctx = g_raw4_ctx;
    if (!ctx || !ctx->pixels || ctx->srcW <= 0 || ctx->srcH <= 0) return;
    int dx = (sx * ctx->dstW) / ctx->srcW;
    int dy = (sy * ctx->dstH) / ctx->srcH;
    if (dx < 0 || dy < 0 || dx >= ctx->dstW || dy < 0 || dy >= ctx->dstH) return;
    ctx->pixels[(size_t)dy * ctx->dstW + dx] = gray4 & 0x0F;
}

static int raw4JpegDraw(JPEGDRAW* pDraw) {
    if (!g_raw4_ctx || !pDraw) return 0;
    for (int yy = 0; yy < pDraw->iHeight; yy++) {
        if (inline_image_maybe_abort_decode()) return 0;
        for (int xx = 0; xx < pDraw->iWidth; xx++) {
            uint16_t px = pDraw->pPixels[yy * pDraw->iWidth + xx];
            raw4_accumulate(pDraw->x + xx, pDraw->y + yy, image_rgb565_to_gray4(px));
        }
    }
    return 1;
}

static int raw4PngDraw(PNGDRAW* pDraw) {
    if (!g_raw4_ctx || !pDraw || !g_raw4_ctx->pngLineBuf || !g_png_active) return 0;
    if (inline_image_maybe_abort_decode()) return 0;
    if (pDraw->iWidth <= 0 || pDraw->iWidth > g_raw4_ctx->srcW) return 0;

    g_png_active->getLineAsRGB565(pDraw, g_raw4_ctx->pngLineBuf, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    for (int xx = 0; xx < pDraw->iWidth; xx++) {
        raw4_accumulate(xx, pDraw->y, image_rgb565_to_gray4(g_raw4_ctx->pngLineBuf[xx]));
    }
    return 1;
}

static int noopJpegDraw(JPEGDRAW*) { return 1; }
static int noopPngDraw(PNGDRAW*) { return 1; }

static File* openSdFileHandle(const char* path, int32_t* outSize) {
    if (outSize) *outSize = 0;
    File* file = new File(SD.open(path, FILE_READ));
    if (!file || !(*file) || file->isDirectory()) {
        if (file) {
            if (*file) file->close();
            delete file;
        }
        return nullptr;
    }
    if (outSize) *outSize = (int32_t)file->size();
    return file;
}

static void* jpegFileOpen(const char* path, int32_t* outSize) {
    return openSdFileHandle(path, outSize);
}

static int32_t jpegFileRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
    if (!pFile || !pFile->fHandle) return 0;
    int32_t n = (int32_t)((File*)pFile->fHandle)->read(pBuf, len);
    if (n > 0) pFile->iPos += n;
    return n > 0 ? n : 0;
}

static int32_t jpegFileSeek(JPEGFILE* pFile, int32_t position) {
    if (!pFile || !pFile->fHandle) return -1;
    if (!((File*)pFile->fHandle)->seek(position)) return -1;
    pFile->iPos = position;
    return position;
}

static void jpegFileClose(void* handle) {
    if (!handle) return;
    File* file = (File*)handle;
    file->close();
    delete file;
}

static void* pngFileOpen(const char* path, int32_t* outSize) {
    return openSdFileHandle(path, outSize);
}

static int32_t pngFileRead(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
    if (!pFile || !pFile->fHandle) return 0;
    int32_t n = (int32_t)((File*)pFile->fHandle)->read(pBuf, len);
    if (n > 0) pFile->iPos += n;
    return n > 0 ? n : 0;
}

static int32_t pngFileSeek(PNGFILE* pFile, int32_t position) {
    if (!pFile || !pFile->fHandle) return -1;
    if (!((File*)pFile->fHandle)->seek(position)) return -1;
    pFile->iPos = position;
    return position;
}

static void pngFileClose(void* handle) {
    if (!handle) return;
    File* file = (File*)handle;
    file->close();
    delete file;
}

bool inline_image_is_marker(const String& line) {
    return line.length() > 5 && line[0] == IMG_MARKER_BYTE &&
           line.startsWith(IMG_MARKER_PREFIX);
}

bool inline_image_is_continuation(const String& line) {
    return line == IMG_CONT_MARKER;
}

bool inline_image_parse_raw(const String& line, String& outPath) {
    if (!inline_image_is_marker(line)) return false;
    int start = 5;
    int end = line.indexOf('\x01', start);
    if (end < 0) end = line.length();
    outPath = line.substring(start, end);
    return outPath.length() > 0;
}

bool inline_image_parse_enriched(const String& line, String& outPath,
                                 int& outW, int& outH, int& outLines) {
    if (!inline_image_is_marker(line)) return false;
    int p1 = 5;
    int p2 = line.indexOf('|', p1);
    if (p2 < 0) return false;
    int p3 = line.indexOf('|', p2 + 1);
    if (p3 < 0) return false;
    int p4 = line.indexOf('|', p3 + 1);
    if (p4 < 0) return false;
    int p5 = line.indexOf('\x01', p4 + 1);
    if (p5 < 0) p5 = line.length();

    outPath = line.substring(p1, p2);
    outW = line.substring(p2 + 1, p3).toInt();
    outH = line.substring(p3 + 1, p4).toInt();
    outLines = line.substring(p4 + 1, p5).toInt();
    return outPath.length() > 0 && outW > 0 && outH > 0 && outLines > 0;
}

String inline_image_build_marker(const String& assetPath, int w, int h, int lines) {
    return String(IMG_MARKER_BYTE) + "IMG|" + assetPath + "|" +
           String(w) + "|" + String(h) + "|" + String(lines) +
           String(IMG_MARKER_BYTE);
}

static bool ends_with_ci(const String& value, const char* suffix) {
    String lower = value;
    lower.toLowerCase();
    String suf = String(suffix);
    suf.toLowerCase();
    return lower.endsWith(suf);
}

static bool isJpeg(const String& path) {
    return ends_with_ci(path, ".jpg") || ends_with_ci(path, ".jpeg");
}

static bool isPng(const String& path) {
    return ends_with_ci(path, ".png");
}

static bool isSupportedImage(const String& path) {
    return isJpeg(path) || isPng(path);
}

static String image_extension(const String& path) {
    if (ends_with_ci(path, ".jpeg")) return ".jpeg";
    if (ends_with_ci(path, ".jpg")) return ".jpg";
    if (ends_with_ci(path, ".png")) return ".png";
    return ".img";
}

static void scaleToFit(int srcW, int srcH, int maxW, int maxH,
                       int& outW, int& outH) {
    if (srcW <= 0 || srcH <= 0) { outW = outH = 0; return; }
    float scaleW = (float)maxW / srcW;
    float scaleH = (float)maxH / srcH;
    float scale = std::min(scaleW, scaleH);
    if (scale > 1.0f) scale = 1.0f;
    outW = std::max(1, (int)(srcW * scale));
    outH = std::max(1, (int)(srcH * scale));
}

static bool inline_image_asset_ok(size_t assetSize, int decodedW, int decodedH, bool png) {
    if (assetSize == 0 || assetSize > 8 * 1024 * 1024) return false;
    if (decodedW <= 0 || decodedH <= 0) return false;
    if (decodedW > 4096 || decodedH > 4096) return false;
    uint64_t pixels = (uint64_t)decodedW * (uint64_t)decodedH;
    if (pixels > 2ULL * 1024ULL * 1024ULL) return false;
    if (png && decodedW > 1024) return false;
    return true;
}

static String vfs_path(const String& path) {
    if (path.startsWith("/sd")) return path;
    return String("/sd") + path;
}

static uint32_t fnv1a_hash(const String& a, const String& b) {
    uint32_t h = 2166136261u;
    auto mix = [&](const String& s) {
        for (int i = 0; i < (int)s.length(); i++) {
            h ^= (uint8_t)s[i];
            h *= 16777619u;
        }
    };
    mix(a);
    h ^= (uint8_t)'|';
    h *= 16777619u;
    mix(b);
    return h;
}

static String inline_cache_dir() {
    return String(LINE_CACHE_DIR) + "/inline";
}

static bool ensure_inline_cache_dir() {
    if (!SD.exists(LINE_CACHE_DIR) && !SD.mkdir(LINE_CACHE_DIR)) return false;
    String dir = inline_cache_dir();
    if (SD.exists(dir)) return true;
    return SD.mkdir(dir);
}

static String inline_cache_path_for(const String& bookPath, const String& zipPath,
                                    const String& assetSignature) {
    uint32_t h = fnv1a_hash(bookPath + "|" + zipPath, assetSignature);
    return inline_cache_dir() + "/img_" + String(h, HEX) + image_extension(zipPath);
}

static bool file_has_content(const String& path, size_t* outSize = nullptr) {
    if (outSize) *outSize = 0;
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        return false;
    }
    size_t sz = f.size();
    f.close();
    if (outSize) *outSize = sz;
    return sz > 0;
}

static bool extract_asset_to_cache(EpubParser& parser, const String& bookPath,
                                   const String& zipPath, String& outPath, size_t* outSize) {
    if (outSize) *outSize = 0;
    if (!isSupportedImage(zipPath)) return false;
    if (!ensure_inline_cache_dir()) return false;

    String assetSignature = parser.getAssetSignature(zipPath);
    if (assetSignature.length() == 0) {
        debug_trace_mark("inline_image_extract:no_signature", zipPath);
        return false;
    }
    outPath = inline_cache_path_for(bookPath, zipPath, assetSignature);
    if (file_has_content(outPath, outSize)) {
        debug_trace_mark("inline_image_extract:cache_hit", outPath);
        return true;
    }

    String tmpPath = outPath + ".tmp";

    size_t dataSize = 0;
    uint8_t* data = parser.readAsset(zipPath, &dataSize);
    if (!data || dataSize == 0) {
        debug_trace_mark("inline_image_extract:read_failed", zipPath);
        if (data) free(data);
        SD.remove(tmpPath);
        return false;
    }
    if (dataSize > 8 * 1024 * 1024) {
        debug_trace_mark("inline_image_extract:too_large", String((uint32_t)dataSize));
        free(data);
        SD.remove(tmpPath);
        return false;
    }

    SD.remove(tmpPath);
    File tmp = SD.open(tmpPath, FILE_WRITE);
    if (!tmp || tmp.isDirectory()) {
        debug_trace_mark("inline_image_extract:tmp_open_failed", tmpPath);
        if (tmp) tmp.close();
        free(data);
        SD.remove(tmpPath);
        return false;
    }
    size_t written = tmp.write(data, dataSize);
    tmp.close();
    free(data);
    if (written != dataSize) {
        debug_trace_mark("inline_image_extract:write_failed", String((uint32_t)written) + "/" + String((uint32_t)dataSize));
        SD.remove(tmpPath);
        return false;
    }

    SD.remove(outPath);
    if (!SD.rename(tmpPath, outPath)) {
        debug_trace_mark("inline_image_extract:rename_failed", outPath);
        SD.remove(tmpPath);
        return false;
    }

    debug_trace_mark("inline_image_extract:ok", outPath + ":" + String((uint32_t)dataSize));
    if (outSize) *outSize = dataSize;
    return true;
}

static bool probe_image_file(const String& assetPath, int& imgW, int& imgH, size_t* outSize) {
    imgW = 0;
    imgH = 0;
    size_t assetSize = 0;
    if (!file_has_content(assetPath, &assetSize)) {
        debug_trace_mark("inline_image_probe_file:no_asset", assetPath);
        return false;
    }
    if (outSize) *outSize = assetSize;

    if (isJpeg(assetPath)) {
        JPEGDEC* jpeg = new JPEGDEC();
        if (!jpeg) return false;
        bool ok = jpeg->open(assetPath.c_str(), jpegFileOpen, jpegFileClose, jpegFileRead, jpegFileSeek, noopJpegDraw);
        if (!ok) debug_trace_mark("inline_image_probe_file:jpeg_open_failed", assetPath);
        if (ok) {
            imgW = jpeg->getWidth();
            imgH = jpeg->getHeight();
            jpeg->close();
        }
        delete jpeg;
        return ok;
    }

    PNG* png = new PNG();
    if (!png) return false;
    bool ok = png->open(assetPath.c_str(), pngFileOpen, pngFileClose, pngFileRead, pngFileSeek, noopPngDraw) == PNG_SUCCESS;
    if (!ok) debug_trace_mark("inline_image_probe_file:png_open_failed", assetPath);
    if (ok) {
        imgW = png->getWidth();
        imgH = png->getHeight();
        png->close();
    }
    delete png;
    return ok;
}

static String raw4_cache_path_for(const String& assetPath, int w, int h) {
    uint32_t hval = fnv1a_hash(assetPath, String(w) + "x" + String(h));
    return inline_cache_dir() + "/raw4_" + String(hval, HEX) + "_" + String(w) + "x" + String(h) + ".r4";
}

static bool raw4_has_valid_size(const String& path, int w, int h) {
    size_t sz = 0;
    if (!file_has_content(path, &sz)) return false;
    return sz == (size_t)w * (size_t)h;
}

static bool write_raw4_cache_from_image(const String& assetPath, const String& rawPath,
                                        int srcW, int srcH, int dstW, int dstH,
                                        size_t assetSize) {
    if (!isSupportedImage(assetPath) || dstW <= 0 || dstH <= 0) return false;
    if (!inline_image_asset_ok(assetSize, srcW, srcH, isPng(assetPath))) return false;
    if ((uint64_t)dstW * (uint64_t)dstH > 540ULL * 960ULL) return false;

    // SAFETY: PNGdec + SD file callbacks are heap-corrupting on the ESP32-S3
    // in this branch during raw4 generation. Do not attempt PNG pre-rendering
    // on-device until that decoder path is fixed; JPEG inline images can still
    // render, and PNGs degrade to the existing placeholder instead of rebooting.
    if (isPng(assetPath)) {
        debug_trace_mark("inline_image_raw4:png_disabled", assetPath);
        return false;
    }

    if (!ensure_inline_cache_dir()) return false;

    String tmpPath = rawPath + ".tmp";
    SD.remove(tmpPath);
    File out = SD.open(tmpPath, FILE_WRITE);
    if (!out) return false;

    Raw4Ctx ctx;
    ctx.srcW = srcW;
    ctx.srcH = srcH;
    ctx.dstW = dstW;
    ctx.dstH = dstH;
    ctx.out = &out;
    ctx.deadlineMs = millis() + 2500;
    ctx.lastYieldMs = millis();

    size_t pixelCount = (size_t)dstW * (size_t)dstH;
    ctx.pixels = (uint8_t*)ps_malloc(pixelCount);
    if (!ctx.pixels) {
        out.close();
        SD.remove(tmpPath);
        return false;
    }
    memset(ctx.pixels, 15, pixelCount);

    bool ok = false;
    if (isJpeg(assetPath)) {
        debug_trace_mark("inline_image_raw4:jpeg", assetPath);
        JPEGDEC* jpeg = new JPEGDEC();
        if (jpeg && jpeg->open(assetPath.c_str(), jpegFileOpen, jpegFileClose, jpegFileRead, jpegFileSeek, raw4JpegDraw)) {
            g_raw4_ctx = &ctx;
            int dec = jpeg->decode(0, 0, 0);
            ok = dec == 1 && !ctx.aborted;
            debug_trace_mark("inline_image_raw4:jpeg_decoded", String(dec) + ":" + String(ctx.aborted ? 1 : 0));
            g_raw4_ctx = nullptr;
            jpeg->close();
        } else {
            debug_trace_mark("inline_image_raw4:jpeg_open_failed", assetPath);
        }
        delete jpeg;
    } else if (isPng(assetPath)) {
        debug_trace_mark("inline_image_raw4:png", assetPath);
        PNG* png = new PNG();
        if (png && png->open(assetPath.c_str(), pngFileOpen, pngFileClose, pngFileRead, pngFileSeek, raw4PngDraw) == PNG_SUCCESS) {
            ctx.pngLineBuf = (uint16_t*)ps_malloc((size_t)srcW * sizeof(uint16_t));
            if (ctx.pngLineBuf) {
                g_raw4_ctx = &ctx;
                g_png_active = png;
                ok = png->decode(nullptr, 0) == PNG_SUCCESS && !ctx.aborted;
                g_png_active = nullptr;
                g_raw4_ctx = nullptr;
                free(ctx.pngLineBuf);
                ctx.pngLineBuf = nullptr;
            }
            png->close();
        }
        delete png;
    }

    if (ok) {
        size_t written = out.write(ctx.pixels, pixelCount);
        ok = (written == pixelCount);
        debug_trace_mark("inline_image_raw4:write", String((uint32_t)written) + "/" + String((uint32_t)pixelCount));
    }

    free(ctx.pixels);
    out.close();

    if (!ok) {
        debug_trace_mark("inline_image_raw4:not_ok", assetPath);
        SD.remove(tmpPath);
        return false;
    }

    SD.remove(rawPath);
    if (!SD.rename(tmpPath, rawPath)) {
        debug_trace_mark("inline_image_raw4:rename_failed", rawPath);
        SD.remove(tmpPath);
        return false;
    }
    bool valid = raw4_has_valid_size(rawPath, dstW, dstH);
    debug_trace_mark("inline_image_raw4:valid", String(valid ? 1 : 0));
    return valid;
}

bool inline_image_probe(EpubParser& parser, const String& bookPath, const String& zipPath,
                        int maxW, int maxH, InlineImageInfo& out) {
    debug_trace_mark("inline_image_probe:start", zipPath);
    if (!isSupportedImage(zipPath)) return false;

    String assetPath;
    size_t assetSize = 0;
    if (!extract_asset_to_cache(parser, bookPath, zipPath, assetPath, &assetSize)) {
        debug_trace_mark("inline_image_probe:extract_failed", zipPath);
        return false;
    }

    int imgW = 0, imgH = 0;
    if (!probe_image_file(assetPath, imgW, imgH, &assetSize)) {
        debug_trace_mark("inline_image_probe:probe_file_failed", assetPath);
        return false;
    }

    debug_trace_mark("inline_image_probe:decoded", String(imgW) + "x" + String(imgH));
    if (!inline_image_asset_ok(assetSize, imgW, imgH, isPng(assetPath))) return false;
    if (imgW < 10 && imgH < 10) return false;

    scaleToFit(imgW, imgH, maxW, maxH, out.displayW, out.displayH);
    if (out.displayW <= 0 || out.displayH <= 0) return false;

    String rawPath = raw4_cache_path_for(assetPath, out.displayW, out.displayH);
    if (!raw4_has_valid_size(rawPath, out.displayW, out.displayH)) {
        debug_trace_mark("inline_image_probe:raw4_build", rawPath);
        if (!write_raw4_cache_from_image(assetPath, rawPath, imgW, imgH,
                                         out.displayW, out.displayH, assetSize)) {
            debug_trace_mark("inline_image_probe:raw4_failed", assetPath);
            return false;
        }
    }

    out.assetPath = rawPath;
    return true;
}

bool inline_image_render(const String& raw4Path,
                         int dstX, int dstY, int dstW, int dstH) {
    debug_trace_mark("inline_image_render:raw4", raw4Path);
    if (dstW <= 0 || dstH <= 0) return false;
    if (!raw4_has_valid_size(raw4Path, dstW, dstH)) {
        debug_trace_mark("inline_image_render:invalid_raw4", raw4Path);
        return false;
    }

    File f = SD.open(raw4Path, FILE_READ);
    if (!f || f.isDirectory()) {
        debug_trace_mark("inline_image_render:open_failed", raw4Path);
        if (f) f.close();
        return false;
    }

    const int chunk = 64;
    uint8_t buf[chunk];
    int x = 0;
    int y = 0;
    uint32_t lastYieldMs = millis();
    bool ok = true;

    while (y < dstH) {
        int remaining = dstW * dstH - (y * dstW + x);
        int want = remaining > chunk ? chunk : remaining;
        int n = f.read(buf, want);
        if (n != want) { ok = false; break; }
        for (int i = 0; i < n; i++) {
            int px = dstX + x;
            int py = dstY + y;
            if (px >= 0 && py >= 0 && px < display_width() && py < display_height()) {
                display_draw_pixel(px, py, buf[i] & 0x0F);
            }
            x++;
            if (x >= dstW) {
                x = 0;
                y++;
                if (y >= dstH) break;
            }
        }
        uint32_t now = millis();
        if (now - lastYieldMs >= 16) {
            yield();
            lastYieldMs = now;
        }
    }

    f.close();
    debug_trace_mark("inline_image_render:done", ok ? "ok" : "fail");
    return ok;
}
