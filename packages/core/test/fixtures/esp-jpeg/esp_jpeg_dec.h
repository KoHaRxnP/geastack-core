#pragma once
// Minimal host stand-in for the esp_new_jpeg 1.0.2 decoder API. Tests implement
// these calls; firmware compilation separately uses Espressif's real header.
#include <cstdint>
extern "C" {
enum jpeg_error_t { JPEG_ERR_OK = 0, JPEG_ERR_FAIL = -1, JPEG_ERR_NO_MEM = -2,
    JPEG_ERR_INVALID_PARAM = -4, JPEG_ERR_BAD_DATA = -5 };
enum jpeg_pixel_format_t { JPEG_PIXEL_FORMAT_RGB888, JPEG_PIXEL_FORMAT_RGB565_BE, JPEG_PIXEL_FORMAT_RGB565_LE };
enum jpeg_rotate_t { JPEG_ROTATE_0D };
struct jpeg_resolution_t { std::uint16_t width, height; };
struct jpeg_dec_config_t {
    jpeg_pixel_format_t output_type;
    jpeg_resolution_t scale, clipper;
    jpeg_rotate_t rotate;
    bool block_enable;
};
#define DEFAULT_JPEG_DEC_CONFIG() { JPEG_PIXEL_FORMAT_RGB888, {0, 0}, {0, 0}, JPEG_ROTATE_0D, false }
using jpeg_dec_handle_t = void *;
struct jpeg_dec_header_info_t { std::uint16_t width, height; };
struct jpeg_dec_io_t { std::uint8_t *inbuf; int inbuf_len, inbuf_remain; std::uint8_t *outbuf; int out_size; };
jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *, jpeg_dec_handle_t *);
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t, jpeg_dec_io_t *, jpeg_dec_header_info_t *);
jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t, int *);
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t, jpeg_dec_io_t *);
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t);
}
