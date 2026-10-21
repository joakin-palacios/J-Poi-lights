#include "wled.h"
#include <esp_timer.h>
#include "../../include/pov_sync.h"
#include "bmp_image.h"
#include "joker.h"
#include "playlist.h"

namespace {

class NumberedPovUsermod;
NumberedPovUsermod* instance = nullptr;

static const char _data_FX_MODE_NUMBERED_POV[] PROGMEM =
  "Numbered POV@Column wait (ms)#pov-wait,,Image#pov-images,Joker colors#pov-jokers,Repeats per BMP#pov-repeat,Cycle BMPs;1,2,3;;1;sx=0,c1=0,c2=3,c3=1,o1=0";

void mode_numbered_pov();

class NumberedPovUsermod : public Usermod {
public:
  NumberedPovUsermod() { instance = this; }

  void requestRescan() { _scanRequested = true; }

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
        _scanRequested = true;
      } else {
        if (_previousFps > 0 && strip.getTargetFps() == 250) strip.setTargetFps(_previousFps);
        for (auto& image : _images) image.release();
        memset(_bufferIds, 0, sizeof(_bufferIds));
        _everDisplayed = false;
        _displayId = 0;
        publishImageJson(false);
      }
    }
    if (!active) {
      PovSync::Playback playback;
      if (pairSyncPovPlayback) pairSyncPovPlayback(_settings, false, _localStartUs, playback);
      _syncStatus = PovSync::Status::Local;
      return;
    }
    if (_scanRequested && !strip.isUpdating()) scanImages();
    refreshFrame(); // Establish the role before accepting local controls.
    if (!_pairedFollower) readControls();
    refreshFrame();
    if (!strip.isUpdating()) loadStep();
    refreshFrame();
    synchronizeSegments();
  }

  void render() {
    if (SEGENV.call == 0) _scanRequested = true;
    if (!_renderFrameValid || _renderFrameTime != strip.now) {
      refreshFrame();
      _renderFrameTime = strip.now;
      _renderFrameValid = true;
    }
    const auto& image = _images[_current];
    if (!_playlist.count() || !image.ready()) {
      if (_pairedFollower && _syncStatus == PovSync::Status::Acquiring) SEGMENT.fill(BLACK);
      else SEGMENT.fill((millis() / 500U) & 1U ? RED : BLACK);
      return;
    }
    const auto blend = NumberedPov::jokerBlend(_column, image.width(), SEGMENT.custom2);
    const uint32_t swatches[] = {SEGCOLOR(0), SEGCOLOR(1), SEGCOLOR(2)};
    const uint32_t joker = color_blend(swatches[blend.first], swatches[blend.second], blend.amount);
    for (uint32_t led = 0; led < SEGLEN; ++led) {
      const uint32_t pixel = image.pixel(_column, led);
      SEGMENT.setPixelColor(led, NumberedPov::jokerPixel(pixel, joker));
    }
  }

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root[F("u")];
    if (user.isNull()) user = root.createNestedObject(F("u"));
    JsonArray status = user.createNestedArray(F("Numbered POV"));
    status.add(_playlist.count() == 0 ? F("no valid images")
      : (_images[_current].ready() ? F("ready") : (_loadErrorId ? F("image error") : F("loading"))));
    status.add(_displayId);
    status.add(_images[_current].width());
    status.add(_images[_current].height());
    switch (_syncStatus) {
      case PovSync::Status::Leader: status.add(F("leader timeline")); break;
      case PovSync::Status::Acquiring: status.add(F("acquiring synchronization")); break;
      case PovSync::Status::Synced: status.add(F("synchronized")); break;
      case PovSync::Status::Stale: status.add(F("stale clock (holdover)")); break;
      case PovSync::Status::ImageNotReady: status.add(_images[_current].ready() ? F("image not ready (holding previous)") : F("image not ready")); break;
      default: status.add(F("local timer")); break;
    }
    status.add(PovSync::intervalUs(_settings.speed) / 1000U);
    status.add(F(" ms/column"));
    if (_settings.cycling) { status.add(F("cycling, repeats")); status.add(_settings.repeats); }
    if (_loadErrorId) {
      status.add(_loadError == NumberedPov::BmpImage::Error::Memory ? F("not enough memory for BMP") : F("cannot load BMP"));
      status.add(_loadErrorId);
    }
  }

  uint16_t getId() override { return USERMOD_ID_POV_DISPLAY; }

private:
  bool isActive() const {
    for (uint8_t i = 0; i < strip.getSegmentsNum(); ++i) {
      const Segment& segment = strip.getSegment(i);
      if (segment.isActive() && segment.mode == _effectId) return true;
    }
    return false;
  }

  const Segment* controls() const {
    const Segment* first = nullptr;
    for (uint8_t i = 0; i < strip.getSegmentsNum(); ++i) {
      const Segment& segment = strip.getSegment(i);
      if (!segment.isActive() || segment.mode != _effectId) continue;
      if (!first) first = &segment;
      if (segment.selected) return &segment;
    }
    return first;
  }

  static uint8_t parseImageId(const char* path) {
    if (!path) return 0;
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (*name < '1' || *name > '9') return 0;
    uint16_t value = 0;
    while (*name >= '0' && *name <= '9') {
      value = value * 10U + uint16_t(*name++ - '0');
      if (value > 255) return 0;
    }
    return strcmp(name, ".bmp") == 0 ? uint8_t(value) : 0;
  }

  void scanImages() {
    _scanRequested = false;
    _images[1U - _current].cancel();
    memset(_bufferIds, 0, sizeof(_bufferIds)); // Keep the displayed pixels while new files are verified.
    memset(_verified, 0, sizeof(_verified));
    memset(_failed, 0, sizeof(_failed));
    memset(_settings.skipped, 0, sizeof(_settings.skipped));
    _loadErrorId = 0;
    _playlist.clear();
    File root = WLED_FS.open("/", "r");
    if (root) {
      File file = root.openNextFile();
      while (file) {
        const uint8_t id = file.isDirectory() ? 0 : parseImageId(file.name());
        NumberedPov::BmpLayout layout;
        if (id && NumberedPov::BmpImage::inspect(file, layout)) _playlist.add(id, layout.width);
        file.close();
        file = root.openNextFile();
      }
      root.close();
    }
    _playlistSignature = _playlist.signature();
    _scheduleDirty = true;
    const Segment* segment = controls();
    if (!_playlist.contains(_selectedImage)) {
      _selectedImage = segment && _playlist.contains(segment->custom1) ? segment->custom1 : _playlist.id(0);
    }
    _settings.image = _selectedImage;
    _settings.playlist = _playlistSignature;
    _localStartUs = esp_timer_get_time();
    _renderFrameValid = false;
    publishImageJson(true);
  }

  void publishImageJson(bool ready) {
    String json = F("{\"ready\":");
    json += ready ? F("true") : F("false");
    json += F(",\"fx\":"); json += unsigned(_effectId);
    json += F(",\"selected\":"); json += unsigned(_selectedImage);
    json += F(",\"images\":[");
    if (ready) for (uint16_t i = 0; i < _playlist.count(); ++i) {
      if (i) json += ',';
      json += unsigned(_playlist.id(i));
    }
    json += F("]}");
    portENTER_CRITICAL(&_jsonMux);
    strlcpy(_imageJson, json.c_str(), sizeof(_imageJson));
    portEXIT_CRITICAL(&_jsonMux);
  }

  void readControls() {
    const Segment* segment = controls();
    if (!segment) return;
    const uint8_t requested = _playlist.contains(segment->custom1) ? segment->custom1 : _selectedImage;
    const uint8_t repeats = NumberedPov::repeatCount(segment->custom3);
    const bool cycling = segment->check1;
    if (requested == _lastPublishedImage && segment->speed == _settings.speed
        && (!cycling || repeats == _settings.repeats) && cycling == _settings.cycling) {
      _settings.repeats = repeats;
      return;
    }
    uint8_t anchor = requested;
    if (!cycling && _settings.cycling && _displayId) anchor = _displayId;
    if (requested != _lastPublishedImage) _failed[requested >> 3] &= ~(1U << (requested & 7));
    _settings.image = anchor;
    _settings.speed = segment->speed;
    _settings.repeats = repeats;
    _settings.cycling = cycling;
    memset(_settings.skipped, 0, sizeof(_settings.skipped));
    if (cycling) memcpy(_settings.skipped, _failed, sizeof(_failed));
    _selectedImage = anchor;
    _localStartUs = esp_timer_get_time();
    _images[1U - _current].cancel();
    _renderFrameValid = false;
  }

  void synchronizeSegments() {
    for (uint8_t i = 0; i < strip.getSegmentsNum(); ++i) {
      Segment& segment = strip.getSegment(i);
      if (segment.mode != _effectId) continue;
      segment.custom1 = _selectedImage;
      segment.speed = _settings.speed;
      segment.custom3 = _settings.repeats;
      segment.check1 = _settings.cycling;
    }
    _lastPublishedImage = _selectedImage;
  }

  void refreshFrame() {
    const uint64_t local = esp_timer_get_time();
    PovSync::Settings input = _settings;
    input.playlist = _pairedFollower && _scanRequested ? 0 : _playlistSignature;
    PovSync::Playback playback;
    const bool paired = pairSyncPovPlayback && pairSyncPovPlayback(input, _playlist.count() != 0, _localStartUs, playback);
    _pairedFollower = paired && playback.follower;
    _pairReady = !_pairedFollower || playback.ready;
    _syncStatus = paired ? playback.status : PovSync::Status::Local;
    if (_pairedFollower && playback.active) _settings = playback.settings;
    if (_scheduleDirty || memcmp(_scheduleSkipped, _settings.skipped, sizeof(_scheduleSkipped))) {
      memcpy(_scheduleSkipped, _settings.skipped, sizeof(_scheduleSkipped));
      _playlist.configure(_scheduleSkipped);
      _scheduleDirty = false;
    }
    const uint64_t now = paired ? playback.nowUs : local;
    if (_pairReady) {
      _clockOffset = int64_t(now) - int64_t(local);
      _position = _playlist.at(now, paired ? playback.startUs : _localStartUs,
        PovSync::intervalUs(_settings.speed), _settings.repeats, _settings.cycling, _settings.image);
      _targetId = _position.image;
      if (_targetId) _selectedImage = _targetId;
      for (uint8_t slot = 0; slot < 2; ++slot) {
        if (_bufferIds[slot] != _targetId || !_targetId || !_images[slot].ready() || !_verified[slot]) continue;
        _current = slot;
        _everDisplayed = true;
        _displayId = _targetId;
        _displayStartUs = _position.batchStartUs;
        _column = _position.column;
        return;
      }
    } else {
      _targetId = _settings.image;
    }
    if (_images[_current].ready()) {
      const int64_t heldTime = int64_t(local) + _clockOffset;
      _column = PovSync::columnAt(heldTime > 0 ? uint64_t(heldTime) : 0, _displayStartUs,
        PovSync::intervalUs(_settings.speed), _images[_current].width());
    }
    if (_syncStatus == PovSync::Status::Synced || _syncStatus == PovSync::Status::Leader || _syncStatus == PovSync::Status::Local)
      _syncStatus = PovSync::Status::ImageNotReady;
  }

  void recordFailure(uint8_t image, NumberedPov::BmpImage::Error error) {
    if (!image) return;
    _failed[image >> 3] |= 1U << (image & 7);
    _loadErrorId = image;
    _loadError = error;
    if (_settings.cycling && !_pairedFollower) {
      memcpy(_settings.skipped, _failed, sizeof(_failed));
      _settings.image = _playlist.contains(_displayId) && !NumberedPov::excluded(_failed, _displayId)
        ? _displayId : _playlist.next(image, _failed);
      if (!_settings.image) _settings.image = image;
      _localStartUs = esp_timer_get_time();
    }
  }

  void loadStep() {
    const uint8_t slot = 1U - _current;
    auto& image = _images[slot];
    if (_pairedFollower && !_pairReady) {
      if (image.loading()) { image.cancel(); _bufferIds[slot] = 0; _verified[slot] = false; }
      return;
    }
    if (!image.loading()) {
      uint8_t wanted = _targetId;
      if (_bufferIds[_current] == wanted && _images[_current].ready() && _verified[_current]) {
        wanted = _settings.cycling ? _playlist.next(wanted, _failed) : 0;
      }
      if (NumberedPov::excluded(_failed, wanted)) wanted = _settings.cycling ? _playlist.next(wanted, _failed) : 0;
      if (!wanted || (_bufferIds[_current] == wanted && _images[_current].ready())
          || (_bufferIds[slot] == wanted && image.ready() && _verified[slot])) return;
      _bufferIds[slot] = wanted;
      _verified[slot] = false;
      char path[10];
      snprintf(path, sizeof(path), "/%u.bmp", unsigned(wanted));
      if (!image.begin(path)) { recordFailure(wanted, image.error()); _bufferIds[slot] = 0; return; }
    }
    image.step(); // At most one 1 KiB read/hash/copy per loop, never during rendering.
    if (image.loading()) return;
    if (!image.ready()) {
      recordFailure(_bufferIds[slot], image.error());
      _bufferIds[slot] = 0;
      return;
    }
    _verified[slot] = !pairSyncPovImageReady || pairSyncPovImageReady(_bufferIds[slot], image.hash());
    if (!_verified[slot]) { _bufferIds[slot] = 0; return; }
    if (_loadErrorId == _bufferIds[slot]) _loadErrorId = 0;
    if (!_everDisplayed && !_pairedFollower) {
      _settings.image = _bufferIds[slot];
      _localStartUs = esp_timer_get_time();
    }
  }

  NumberedPov::BmpImage _images[2];
  NumberedPov::Playlist _playlist;
  PovSync::Settings _settings;
  NumberedPov::Position _position;
  uint8_t _current = 0;
  uint8_t _bufferIds[2] = {};
  bool _verified[2] = {};
  uint8_t _failed[32] = {};
  uint8_t _scheduleSkipped[32] = {};
  uint32_t _playlistSignature = 0;
  bool _scheduleDirty = true;
  uint8_t _selectedImage = 0;
  uint8_t _lastPublishedImage = 0;
  uint8_t _displayId = 0;
  uint8_t _targetId = 0;
  uint8_t _effectId = 255;
  uint8_t _previousFps = WLED_FPS;
  uint32_t _column = 0;
  uint32_t _renderFrameTime = 0;
  bool _renderFrameValid = false;
  uint64_t _localStartUs = 0;
  uint64_t _displayStartUs = 0;
  int64_t _clockOffset = 0;
  bool _everDisplayed = false;
  bool _pairedFollower = false;
  bool _pairReady = true;
  PovSync::Status _syncStatus = PovSync::Status::Local;
  uint8_t _loadErrorId = 0;
  NumberedPov::BmpImage::Error _loadError = NumberedPov::BmpImage::Error::None;
  bool _wasActive = false;
  bool _scanRequested = false;
  portMUX_TYPE _jsonMux = portMUX_INITIALIZER_UNLOCKED;
  char _imageJson[1400] = {};
};

void mode_numbered_pov() { if (instance) instance->render(); }

NumberedPovUsermod numberedPov;
REGISTER_USERMOD(numberedPov);

} // namespace

void requestNumberedPovRescan() { numberedPov.requestRescan(); }
