#pragma once

#include <mbedtls/sha256.h>
#include "../../include/pov_bmp.h"

namespace NumberedPov {

class BmpImage {
public:
  enum class Error : uint8_t { None, Format, Memory, Read };
  BmpImage() { mbedtls_sha256_init(&_sha); }
  ~BmpImage() { release(); }
  BmpImage(const BmpImage&) = delete;
  BmpImage& operator=(const BmpImage&) = delete;

  static bool inspect(File& file, BmpLayout& layout) {
    uint8_t header[54];
    return file && file.seek(0) && file.read(header, sizeof(header)) == sizeof(header)
      && bmpLayout(header, file.size(), layout);
  }

  bool begin(const char* path) {
    cancel();
    _ready = false;
    _error = Error::None;
    _file = WLED_FS.open(path, "r");
    if (!inspect(_file, _layout)) return fail(Error::Format);
    if (_layout.dataSize > _capacity) {
      free(_pixels);
      _capacity = 0;
      _pixels = static_cast<uint8_t*>(allocate_buffer(_layout.dataSize, BFRALLOC_PREFER_PSRAM));
      if (!_pixels) return fail(Error::Memory);
      _capacity = _layout.dataSize;
    }
    if (!_file.seek(0)) return fail(Error::Read);
    _fileSize = _file.size();
    _copied = 0;
    mbedtls_sha256_starts(&_sha, 0);
    _loading = true;
    return true;
  }

  void step() {
    if (!_loading) return;
    uint8_t buffer[1024];
    const uint32_t offset = _file.position();
    const size_t count = _file.read(buffer, sizeof(buffer));
    if (!count || offset > _fileSize || count > _fileSize - offset) { fail(Error::Read); return; }
    mbedtls_sha256_update(&_sha, buffer, count);
    const uint32_t endOfPixels = _layout.offset + _layout.dataSize;
    const uint32_t begin = offset > _layout.offset ? offset : _layout.offset;
    const uint32_t end = offset + count < endOfPixels ? offset + count : endOfPixels;
    if (end > begin) {
      memcpy(_pixels + (begin - _layout.offset), buffer + (begin - offset), end - begin);
      _copied += end - begin;
    }
    if (_file.position() == _fileSize) {
      if (_copied != _layout.dataSize || _file.size() != _fileSize) { fail(Error::Read); return; }
      mbedtls_sha256_finish(&_sha, _digest);
      cancel();
      _ready = true;
    }
  }

  void cancel() {
    if (_file) _file.close();
    _loading = false;
    mbedtls_sha256_free(&_sha);
    mbedtls_sha256_init(&_sha);
  }
  void release() {
    cancel();
    free(_pixels);
    _pixels = nullptr;
    _capacity = 0;
    _ready = false;
  }
  bool ready() const { return _ready; }
  bool loading() const { return _loading; }
  Error error() const { return _error; }
  uint32_t width() const { return _layout.width; }
  uint32_t height() const { return _layout.height; }
  const uint8_t* hash() const { return _digest; }
  uint32_t pixel(uint32_t x, uint32_t y) const {
    if (!_ready || x >= width() || y >= height()) return BLACK;
    const uint32_t row = _layout.topDown ? y : height() - 1U - y;
    const uint8_t* bgr = _pixels + size_t(row) * _layout.rowSize + size_t(x) * 3U;
    return RGBW32(bgr[2], bgr[1], bgr[0], 0);
  }

private:
  bool fail(Error error) { cancel(); _ready = false; _error = error; return false; }
  File _file;
  BmpLayout _layout;
  mbedtls_sha256_context _sha;
  uint8_t* _pixels = nullptr;
  size_t _capacity = 0;
  uint32_t _fileSize = 0;
  uint32_t _copied = 0;
  uint8_t _digest[32] = {};
  bool _ready = false;
  bool _loading = false;
  Error _error = Error::None;
};

} // namespace NumberedPov
