#include "wled.h"

namespace {

constexpr uint16_t MIN_COLUMN_WAIT_MS = 4;
constexpr uint16_t MAX_COLUMN_WAIT_MS = 260;

class BmpImage {
public:
  ~BmpImage() { reset(); }

  bool load(const char* path) {
    File file = WLED_FS.open(path, "r");
    if (!file) return false;

    uint16_t signature;
    uint32_t ignored;
    uint32_t imageOffset;
    uint32_t dibSize;
    int32_t width;
    int32_t signedHeight;
    uint16_t planes;
    uint16_t depth;
    uint32_t compression;

    bool headerOk = read(file, signature) && signature == 0x4D42
      && read(file, ignored) && read(file, ignored) && read(file, imageOffset)
      && read(file, dibSize) && dibSize >= 40
      && read(file, width) && read(file, signedHeight)
      && read(file, planes) && read(file, depth) && read(file, compression);

    if (!headerOk || width <= 0 || signedHeight == 0 || signedHeight == INT32_MIN
        || planes != 1 || depth != 24 || compression != 0) {
      file.close();
      return false;
    }

    const uint32_t height = signedHeight < 0 ? uint32_t(-signedHeight) : uint32_t(signedHeight);
    const uint32_t uWidth = uint32_t(width);
    if (uWidth > (UINT32_MAX - 3U) / 3U) {
      file.close();
      return false;
    }

    const uint32_t rowSize = (uWidth * 3U + 3U) & ~3U;
    if (height > SIZE_MAX / rowSize) {
      file.close();
      return false;
    }

    const size_t dataSize = size_t(rowSize) * height;
    const size_t fileSize = file.size();
    if (imageOffset < 14U + dibSize || imageOffset > fileSize
        || dataSize > fileSize - imageOffset || !file.seek(imageOffset)) {
      file.close();
      return false;
    }

    uint8_t* next = static_cast<uint8_t*>(allocate_buffer(dataSize, BFRALLOC_PREFER_PSRAM));
    if (!next) {
      file.close();
      return false;
    }

    const size_t bytesRead = file.read(next, dataSize);
    file.close();
    if (bytesRead != dataSize) {
      free(next);
      return false;
    }

    reset();
    _pixels = next;
    _width = uWidth;
    _height = height;
    _rowSize = rowSize;
    _topDown = signedHeight < 0;
    return true;
  }

  void reset() {
    if (_pixels) free(_pixels);
    _pixels = nullptr;
    _width = 0;
    _height = 0;
    _rowSize = 0;
    _topDown = false;
  }

  bool loaded() const { return _pixels != nullptr; }
  uint32_t width() const { return _width; }
  uint32_t height() const { return _height; }

  uint32_t pixel(uint32_t x, uint32_t y) const {
    if (!_pixels || x >= _width || y >= _height) return 0;
    const uint32_t storedY = _topDown ? y : (_height - 1U - y);
    const uint8_t* bgr = _pixels + size_t(storedY) * _rowSize + size_t(x) * 3U;
    return RGBW32(bgr[2], bgr[1], bgr[0], 0);
  }

private:
  template<typename T>
  static bool read(File& file, T& value) {
    return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == sizeof(T);
  }

  uint8_t* _pixels = nullptr;
  uint32_t _width = 0;
  uint32_t _height = 0;
  uint32_t _rowSize = 0;
  bool _topDown = false;
};

class NumberedPovUsermod;
NumberedPovUsermod* instance = nullptr;

static const char _data_FX_MODE_NUMBERED_POV[] PROGMEM =
  "Numbered POV@Column wait (ms)#pov-wait,,Image#pov-images;;;1;sx=0,c1=0";

void mode_numbered_pov();

class NumberedPovUsermod : public Usermod {
public:
  NumberedPovUsermod() { instance = this; }

  void requestRescan() {
    _scanRequested = true;
    _attemptedImage = 0;
    _loadedImage = 0;
  }

  void setup() override {
    _effectId = strip.addEffect(255, &mode_numbered_pov, _data_FX_MODE_NUMBERED_POV);
    publishImageJson(false);
    server.on(F("/json/pov-images"), HTTP_GET, [this](AsyncWebServerRequest* request) {
      char json[1400];
      portENTER_CRITICAL(&_jsonMux);
      strlcpy(json, _imageJson, sizeof(json));
      portEXIT_CRITICAL(&_jsonMux);
      request->send(200, F("application/json"), json);
    });
  }

  void loop() override {
    if (_effectId == 255) return;
    const bool active = isActive();
    if (active != _wasActive) {
      _wasActive = active;
      if (active) {
        _previousFps = strip.getTargetFps();
        if (_previousFps > 0 && _previousFps < 250) strip.setTargetFps(250);
        scanImages();
        _attemptedImage = 0;
        _lastColumnUs = micros();
      } else {
        if (_previousFps > 0 && strip.getTargetFps() == 250) strip.setTargetFps(_previousFps);
        _image.reset();
        _loadedImage = 0;
        _attemptedImage = 0;
        publishImageJson(false);
      }
    }
    if (!active) return;

    if (_scanRequested && !strip.isUpdating()) {
      _scanRequested = false;
      scanImages();
    }

    synchronizeSelection();
    if (_selectedImage != _loadedImage && _selectedImage != _attemptedImage && !strip.isUpdating()) loadSelectedImage();
    updateColumn();
  }

  void render() {
    if (SEGENV.call == 0) _scanRequested = true;
    if (_imageCount == 0 || !_image.loaded()) {
      SEGMENT.fill((millis() / 500U) & 1U ? RED : BLACK);
      return;
    }

    const uint32_t segmentLength = SEGLEN;
    const uint32_t imageHeight = _image.height();
    for (uint32_t led = 0; led < segmentLength; led++) {
      SEGMENT.setPixelColor(led, led < imageHeight ? _image.pixel(_column, led) : BLACK);
    }
  }

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root[F("u")];
    if (user.isNull()) user = root.createNestedObject(F("u"));
    JsonArray status = user.createNestedArray(F("Numbered POV"));
    status.add(_imageCount == 0 ? F("no images") : (_image.loaded() ? F("ready") : F("image error")));
    status.add(_selectedImage);
    status.add(_image.width());
    status.add(_image.height());
    status.add(F("fixed timer"));
    status.add(columnWaitMs(_speedControl));
    status.add(F(" ms/column"));
  }

  uint16_t getId() override { return USERMOD_ID_POV_DISPLAY; }

private:
  bool isActive() const {
    for (uint8_t i = 0; i < strip.getSegmentsNum(); i++) {
      const Segment& segment = strip.getSegment(i);
      if (segment.isActive() && segment.mode == _effectId) return true;
    }
    return false;
  }

  static uint8_t parseImageId(const char* path) {
    if (!path) return 0;
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (name[0] < '1' || name[0] > '9') return 0;

    uint16_t value = 0;
    const char* cursor = name;
    while (*cursor >= '0' && *cursor <= '9') {
      value = value * 10U + uint16_t(*cursor - '0');
      if (value > 255) return 0;
      cursor++;
    }
    if (strcmp(cursor, ".bmp") != 0) return 0;
    return uint8_t(value);
  }

  bool hasImage(uint8_t id) const {
    for (uint16_t i = 0; i < _imageCount; i++) if (_imageIds[i] == id) return true;
    return false;
  }

  static uint16_t columnWaitMs(uint8_t control) {
    return MIN_COLUMN_WAIT_MS
      + (uint32_t(control) * (MAX_COLUMN_WAIT_MS - MIN_COLUMN_WAIT_MS) + 127U) / 255U;
  }

  void scanImages() {
    _attemptedImage = 0;
    for (uint8_t i = 0; i < strip.getSegmentsNum(); i++) {
      const Segment& segment = strip.getSegment(i);
      if (segment.mode == _effectId) {
        _speedControl = segment.speed;
        break;
      }
    }
    uint8_t found[255];
    uint16_t count = 0;
    File root = WLED_FS.open("/", "r");
    if (root) {
      File file = root.openNextFile();
      while (file) {
        if (!file.isDirectory()) {
          const uint8_t id = parseImageId(file.name());
          if (id && count < sizeof(found)) found[count++] = id;
        }
        file.close();
        file = root.openNextFile();
      }
      root.close();
    }

    for (uint16_t i = 1; i < count; i++) {
      const uint8_t value = found[i];
      int j = i - 1;
      while (j >= 0 && found[j] > value) {
        found[j + 1] = found[j];
        j--;
      }
      found[j + 1] = value;
    }

    _imageCount = 0;
    for (uint16_t i = 0; i < count; i++) {
      if (_imageCount == 0 || found[i] != _imageIds[_imageCount - 1]) {
        _imageIds[_imageCount++] = found[i];
      }
    }

    if (_imageCount == 0) {
      _selectedImage = 0;
      _loadedImage = 0;
      _image.reset();
    } else if (!hasImage(_selectedImage)) {
      uint8_t requested = 0;
      for (uint8_t i = 0; i < strip.getSegmentsNum(); i++) {
        const Segment& segment = strip.getSegment(i);
        if (segment.mode == _effectId && hasImage(segment.custom1)) {
          requested = segment.custom1;
          break;
        }
      }
      _selectedImage = requested ? requested : _imageIds[0];
    }
    synchronizeSegments();
    publishImageJson(true);
  }

  void publishImageJson(bool ready) {
    String json = F("{\"ready\":");
    json += ready ? F("true") : F("false");
    json += F(",\"fx\":");
    json += unsigned(_effectId);
    json += F(",\"selected\":");
    json += unsigned(_selectedImage);
    json += F(",\"images\":[");
    if (ready) {
      for (uint16_t i = 0; i < _imageCount; i++) {
        if (i) json += ',';
        json += unsigned(_imageIds[i]);
      }
    }
    json += F("]}");

    portENTER_CRITICAL(&_jsonMux);
    strlcpy(_imageJson, json.c_str(), sizeof(_imageJson));
    portEXIT_CRITICAL(&_jsonMux);
  }

  void synchronizeSelection() {
    uint8_t requested = 0;
    uint8_t requestedSpeed = _speedControl;
    bool speedRequested = false;
    for (uint8_t i = 0; i < strip.getSegmentsNum(); i++) {
      const Segment& segment = strip.getSegment(i);
      if (segment.mode == _effectId && segment.selected && hasImage(segment.custom1)) {
        requested = segment.custom1;
        requestedSpeed = segment.speed;
        speedRequested = true;
        break;
      }
    }
    if (!requested) {
      for (uint8_t i = 0; i < strip.getSegmentsNum(); i++) {
        const Segment& segment = strip.getSegment(i);
        if (segment.mode == _effectId && segment.custom1 != _selectedImage && hasImage(segment.custom1)) {
          requested = segment.custom1;
          break;
        }
      }
    }
    if (!speedRequested) {
      for (uint8_t i = 0; i < strip.getSegmentsNum(); i++) {
        const Segment& segment = strip.getSegment(i);
        if (segment.mode == _effectId && segment.speed != _speedControl) {
          requestedSpeed = segment.speed;
          speedRequested = true;
          break;
        }
      }
    }
    if (requested && requested != _selectedImage) {
      _selectedImage = requested;
      _attemptedImage = 0;
      _column = 0;
      _lastColumnUs = micros();
    }
    if (speedRequested && requestedSpeed != _speedControl) {
      _speedControl = requestedSpeed;
      _lastColumnUs = micros();
    }
    synchronizeSegments();
  }

  void synchronizeSegments() const {
    for (uint8_t i = 0; i < strip.getSegmentsNum(); i++) {
      Segment& segment = strip.getSegment(i);
      if (segment.mode == _effectId) {
        segment.custom1 = _selectedImage;
        segment.speed = _speedControl;
      }
    }
  }

  void loadSelectedImage() {
    _image.reset();
    _loadedImage = 0;
    _attemptedImage = _selectedImage;
    _column = 0;
    _lastColumnUs = micros();
    if (!_selectedImage) return;

    char path[10];
    snprintf(path, sizeof(path), "/%u.bmp", _selectedImage);
    if (_image.load(path)) _loadedImage = _selectedImage;
  }

  void updateColumn() {
    if (!_image.loaded() || _image.width() == 0) return;
    const uint32_t now = micros();
    const uint32_t intervalUs = uint32_t(columnWaitMs(_speedControl)) * 1000U;
    const uint32_t elapsedUs = now - _lastColumnUs;
    if (elapsedUs < intervalUs) return;
    const uint32_t columns = elapsedUs / intervalUs;
    _lastColumnUs += columns * intervalUs;
    _column = (_column + columns) % _image.width();
  }

  BmpImage _image;
  uint8_t _imageIds[255] = {};
  uint16_t _imageCount = 0;
  uint8_t _selectedImage = 0;
  uint8_t _loadedImage = 0;
  uint8_t _attemptedImage = 0;
  uint8_t _speedControl = 0;
  uint8_t _effectId = 255;
  uint8_t _previousFps = WLED_FPS;
  uint32_t _column = 0;
  uint32_t _lastColumnUs = 0;
  bool _wasActive = false;
  bool _scanRequested = false;
  portMUX_TYPE _jsonMux = portMUX_INITIALIZER_UNLOCKED;
  char _imageJson[1400] = {};
};

void mode_numbered_pov() {
  if (instance) instance->render();
}

NumberedPovUsermod numberedPov;
REGISTER_USERMOD(numberedPov);

} // namespace

void requestNumberedPovRescan() {
  numberedPov.requestRescan();
}
