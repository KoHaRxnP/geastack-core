#include "image.h"
#include "memory.h"
#include "pixel.h"
#include "esp32/rom/tjpgd.h"
#include "stb_image.h"
#if GEA_EMBEDDED_ESP_JPEG
#include "esp_jpeg_dec.h"
#endif

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace {
std::unordered_map<void *, std::size_t> allocations;
std::size_t liveBytes = 0, peakBytes = 0;
int prepareCalls = 0, decompCalls = 0, failureMode = 0;
std::vector<unsigned char> decodedRgb;
// Hardware-independent ROM callback fixture. stb prepares the fixture before
// allocation accounting starts; jd_decomp then emits ROM-shaped MCU blocks.
constexpr int width = 410, height = 502;
#if GEA_EMBEDDED_ESP_JPEG
int espFailureMode = 1, espOpenCalls = 0, espCloseCalls = 0;
jpeg_pixel_format_t espOutputFormat{};
#endif
}

namespace gea::framework::memory {
void *Allocator::allocatePreferSpiram(std::size_t bytes, std::size_t alignment)
{
    void *p = nullptr;
    if (posix_memalign(&p, std::max(alignment, sizeof(void *)), std::max(bytes, std::size_t(1)))) return nullptr;
    allocations[p] = bytes;
    liveBytes += bytes;
    peakBytes = std::max(peakBytes, liveBytes);
    return p;
}
void Allocator::free(void *p) noexcept
{
    if (!p) return;
    const auto it = allocations.find(p);
    assert(it != allocations.end());
    liveBytes -= it->second;
    allocations.erase(it);
    std::free(p);
}
}

#if GEA_EMBEDDED_ESP_JPEG
extern "C" jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *config, jpeg_dec_handle_t *handle)
{
    ++espOpenCalls;
    assert(!config->scale.width && !config->scale.height && !config->clipper.width && !config->clipper.height);
    assert(config->rotate == JPEG_ROTATE_0D && !config->block_enable);
    espOutputFormat = config->output_type;
    if (espFailureMode == 1) return JPEG_ERR_FAIL;
    *handle = &espOutputFormat;
    return JPEG_ERR_OK;
}
extern "C" jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t handle, jpeg_dec_io_t *io, jpeg_dec_header_info_t *info)
{
    assert(handle && io->inbuf && io->inbuf_len > 0 && !io->outbuf);
    if (espFailureMode == 2) return JPEG_ERR_BAD_DATA;
    info->width = espFailureMode == 7 ? 0 : espFailureMode == 8 ? 65535 : width;
    info->height = espFailureMode == 8 ? 65535 : height;
    return JPEG_ERR_OK;
}
extern "C" jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t, int *bytes)
{
    *bytes = width * height * 2 + (espFailureMode == 4 ? 16 : 0);
    return espFailureMode == 3 ? JPEG_ERR_FAIL : JPEG_ERR_OK;
}
extern "C" jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t, jpeg_dec_io_t *io)
{
    assert(reinterpret_cast<std::uintptr_t>(io->outbuf) % 16 == 0);
    for (int i = 0; i < width * height; ++i) {
        const auto *rgb = &decodedRgb[i * 3];
        const std::uint16_t value = ((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3);
        const bool bigEndian = espOutputFormat == JPEG_PIXEL_FORMAT_RGB565_BE;
        io->outbuf[i * 2] = bigEndian ? value >> 8 : value & 255;
        io->outbuf[i * 2 + 1] = bigEndian ? value & 255 : value >> 8;
        if (espFailureMode == 5) return JPEG_ERR_BAD_DATA;
    }
    io->out_size = width * height * 2 - (espFailureMode == 6 ? 2 : 0);
    return JPEG_ERR_OK;
}
extern "C" jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t handle)
{
    assert(handle);
    ++espCloseCalls;
    return JPEG_ERR_OK;
}
#endif

extern "C" JRESULT jd_prepare(JDEC *jd, UINT (*input)(JDEC *, BYTE *, UINT), void *, UINT poolSize, void *device)
{
    ++prepareCalls;
    assert(poolSize == 8192);
    jd->device = device;
    BYTE prefix[3]{};
    assert(input(jd, prefix, 3) == 3 && prefix[0] == 0xff && prefix[1] == 0xd8);
    assert(input(jd, nullptr, 2) == 2);
    if (failureMode == 1) return JDR_FMT3;
    jd->width = width;
    jd->height = height;
    return JDR_OK;
}

extern "C" JRESULT jd_decomp(JDEC *jd, UINT (*output)(JDEC *, void *, JRECT *), BYTE scale)
{
    ++decompCalls;
    assert(scale == 0); // 502px must not become 251px.
    BYTE block[16 * 16 * 3];
    for (int y = 0; y < height; y += 16) for (int x = 0; x < width; x += 16) {
        JRECT rect{static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(std::min(x + 15, width - 1)),
                   static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(std::min(y + 15, height - 1))};
        int i = 0;
        for (int by = rect.top; by <= rect.bottom; ++by) for (int bx = rect.left; bx <= rect.right; ++bx)
            for (int c = 0; c < 3; ++c) block[i++] = decodedRgb[(by * width + bx) * 3 + c];
        if (!output(jd, block, &rect)) return JDR_INTR;
        if (failureMode == 2) return JDR_INP;
        if (failureMode == 3) {
            rect.right = width;
            assert(output(jd, block, &rect) == 0);
            return JDR_INTR;
        }
    }
    return JDR_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> jpeg((std::istreambuf_iterator<char>(file)), {});
    assert(!jpeg.empty());
    int w = 0, h = 0, channels = 0;
    auto *rgb = stbi_load_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &w, &h, &channels, 3);
    assert(rgb && w == width && h == height);
    decodedRgb.assign(rgb, rgb + w * h * 3);
    stbi_image_free(rgb);
    assert(liveBytes == 0);
    auto &images = gea::framework::graphics::ImageStore::instance();
    namespace pixel = gea::framework::graphics::pixel;
    for (failureMode = 0; failureMode <= 3; ++failureMode) {
        peakBytes = 0;
        const int id = images.decodeOpaque(jpeg.data(), static_cast<int>(jpeg.size()), -1);
        assert(id >= 0 && images.width(id) == width && images.height(id) == height);
        assert(images.currentAlpha(id) == nullptr);
        const auto *pixels = images.currentPixels(id);
        for (int i = 0; i < width * height; ++i)
            assert(pixels[i] == pixel::fromRgb888(decodedRgb[i * 3], decodedRgb[i * 3 + 1], decodedRgb[i * 3 + 2]));
        if (failureMode == 0) {
            assert(peakBytes == width * height * sizeof(pixel::native_t) + 8192);
            std::printf("ROM callback path: native=%zu peak=%zu, full colour %dx%d\n", liveBytes, peakBytes, w, h);
        }
        images.dispose(id);
        assert(liveBytes == 0 && allocations.empty());
    }
    assert(prepareCalls == 4 && decompCalls == 3);
#if GEA_EMBEDDED_ESP_JPEG
    failureMode = 0;
    for (espFailureMode = 0; espFailureMode <= 8; ++espFailureMode) {
        peakBytes = 0;
        const int romCallsBefore = prepareCalls;
        const int closeCallsBefore = espCloseCalls;
        const int id = images.decode(jpeg.data(), static_cast<int>(jpeg.size()), -1);
        assert(id >= 0 && images.width(id) == width && images.height(id) == height);
        assert(images.currentAlpha(id) == nullptr);
        const auto *pixels = images.currentPixels(id);
        for (int i = 0; i < width * height; ++i)
            assert(pixels[i] == pixel::fromRgb888(decodedRgb[i * 3], decodedRgb[i * 3 + 1], decodedRgb[i * 3 + 2]));
        assert(prepareCalls == romCallsBefore + (espFailureMode != 0));
        assert(espCloseCalls == closeCallsBefore + (espFailureMode != 1));
        if (espFailureMode == 0) {
            assert(peakBytes == width * height * sizeof(pixel::native_t));
            std::printf("SIMD API path: final native allocation only=%zu, panel endian=%d\n", peakBytes, GEA_EMBEDDED_PIXEL_PANEL_ENDIAN);
        }
        images.dispose(id);
        assert(liveBytes == 0 && allocations.empty());
    }
    std::puts("PASS: SIMD API direct RGB565, dimensions/alignment/endian, all error paths close/free and fall back to ROM");
#endif
    std::puts("PASS: native JPEG dimensions/colour/edge blocks, unsupported and interrupted decode fallback, no leaks");
}
