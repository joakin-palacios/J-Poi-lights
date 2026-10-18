#include "wled.h"

#ifndef ARDUINO_ARCH_ESP32
#error Pair Sync requires ESP32
#endif

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <mbedtls/sha256.h>

void requestNumberedPovRescan() __attribute__((weak));

namespace {

constexpr uint8_t ROLE_DISABLED = 0;
constexpr uint8_t ROLE_LEADER = 1;
constexpr uint8_t ROLE_FOLLOWER = 2;
constexpr uint8_t SYNC_GROUP = 0x01;
constexpr uint32_t CONTROL_POLL_MS = 1000;
constexpr uint32_t RESOURCE_SCAN_MS = 10000;
constexpr uint32_t HTTP_TIMEOUT_MS = 3500;
constexpr size_t MAX_BMP_SIZE = 768U * 1024U;

struct ResourceEntry {
  uint8_t id;
  uint32_t size;
  uint8_t sha256[32];
};

class PairSyncUsermod : public Usermod {
public:
  void setup() override {
    recoverResourceTransactions();
    _resources[0] = static_cast<ResourceEntry*>(allocate_buffer(sizeof(ResourceEntry) * 255U, BFRALLOC_PREFER_PSRAM));
    _resources[1] = static_cast<ResourceEntry*>(allocate_buffer(sizeof(ResourceEntry) * 255U, BFRALLOC_PREFER_PSRAM));
    _bootId = esp_random();
    registerRoutes();
    _setupDone = true;
    _rolePending = true;
  }

  void loop() override {
    if (_rolePending) applyRole();

    if (_role == ROLE_LEADER) {
      keepExclusiveAp();
      if (elapsed(_lastResourceScan, RESOURCE_SCAN_MS) && !strip.isUpdating()) {
        _lastResourceScan = millis();
        scanResources();
      }
      return;
    }

    if (_role != ROLE_FOLLOWER) return;

    if (Network.isConnected() && elapsed(_lastControlPoll, CONTROL_POLL_MS)) {
      _lastControlPoll = millis();
      pollControl();
    }
  }

  void connected() override {
    if (_role == ROLE_FOLLOWER) {
      _lastControlPoll = 0;
      _localSceneRevision = 0;
    }
  }

  void onStateChange(uint8_t mode) override {
    if (_applyingRemote) return;
    if (_role == ROLE_LEADER && mode != CALL_MODE_INIT && mode != CALL_MODE_NOTIFICATION) {
      _sceneRevision++;
      if (_sceneRevision == 0) _sceneRevision = 1;
    } else if (_role == ROLE_FOLLOWER && mode != CALL_MODE_INIT && mode != CALL_MODE_NOTIFICATION) {
      _localSceneRevision = 0;
    }
  }

  void addToJsonState(JsonObject& root) override {
    JsonObject state = root.createNestedObject(F("PairSync"));
    state[F("role")] = _role;
    state[F("sceneRevision")] = _role == ROLE_LEADER ? _sceneRevision : _localSceneRevision;
    state[F("resourceGeneration")] = _role == ROLE_LEADER ? _resourceGeneration : _localResourceGeneration;
  }

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root[F("u")];
    if (user.isNull()) user = root.createNestedObject(F("u"));
    JsonArray status = user.createNestedArray(F("Pair Sync"));
    status.add(roleName());
    if (_role == ROLE_LEADER) {
      status.add(followerOnline() ? F("follower online") : F("follower offline"));
    } else if (_role == ROLE_FOLLOWER) {
      status.add(Network.isConnected() ? F("leader connected") : F("leader disconnected"));
    }
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(F("PairSync"));
    top[F("role")] = _role;
    top[F("leaderHost")] = _leaderHost;
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[F("PairSync")];
    const uint8_t oldRole = _role;
    bool complete = !top.isNull();
    complete &= getJsonValue(top[F("role")], _role, ROLE_DISABLED);
    complete &= getJsonValue(top[F("leaderHost")], _leaderHost, String(F("4.3.2.1")));
    if (_role > ROLE_FOLLOWER) _role = ROLE_DISABLED;
    if (_setupDone && oldRole != _role) _rolePending = true;
    return complete;
  }

  void appendConfigData(Print& script) override {
    script.print(F("dd=addDropdown('PairSync','role');"
                   "addOption(dd,'Disabled',0);"
                   "addOption(dd,'Leader',1);"
                   "addOption(dd,'Follower',2);"
                   "addInfo('PairSync:leaderHost',1,' Leader address (default 4.3.2.1)');"));
  }

  uint16_t getId() override { return USERMOD_ID_PAIR_SYNC; }

  bool broadcastIP(IPAddress& address) const {
    if (_role != ROLE_LEADER) return false;
    address = IPAddress(4, 3, 2, 255);
    return true;
  }

private:
  static bool elapsed(uint32_t since, uint32_t interval) {
    return uint32_t(millis() - since) >= interval;
  }

  const __FlashStringHelper* roleName() const {
    if (_role == ROLE_LEADER) return F("leader");
    if (_role == ROLE_FOLLOWER) return F("follower");
    return F("disabled");
  }

  bool followerOnline() const {
    return _followerLastSeen && uint32_t(millis() - _followerLastSeen) < 5000U;
  }

  void applyRole() {
    _rolePending = false;
    _lastError = String();

    if (_role == ROLE_LEADER) {
      apBehavior = AP_BEHAVIOR_ALWAYS;
      configureUdp(true);
      _sceneRevision = 1;
      _lastResourceScan = 0;
      keepExclusiveAp();
    } else if (_role == ROLE_FOLLOWER) {
      apBehavior = AP_BEHAVIOR_BUTTON_ONLY;
      configureUdp(false);
      _leaderBootId = 0;
      _localSceneRevision = 0;
      _localResourceGeneration = 0;
      if (apActive) stopAp();
      forceReconnect = true;
    } else {
      disablePairNetworking();
      forceReconnect = true;
    }
  }

  static void disablePairNetworking() {
    apBehavior = AP_BEHAVIOR_NO_CONN;
    sendNotifications = false;
    sendNotificationsRT = false;
    syncGroups = 0;
    receiveGroups = 0;
    udpNumRetries = 0;
  }

  void configureUdp(bool leader) {
    sendNotifications = leader;
    sendNotificationsRT = leader;
    syncGroups = leader ? SYNC_GROUP : 0;
    receiveGroups = leader ? 0 : SYNC_GROUP;
    notifyDirect = true;
    notifyButton = true;
    receiveNotificationBrightness = true;
    receiveNotificationColor = true;
    receiveNotificationEffects = true;
    receiveNotificationPalette = true;
    receiveSegmentOptions = true;
    receiveSegmentBounds = true;
    udpNumRetries = 1;
  }

  void keepExclusiveAp() {
    apBehavior = AP_BEHAVIOR_ALWAYS;
    if (!apActive) WLED::instance().initAP();
    if (Network.isConnected()) WiFi.disconnect(false, false);
    interfacesInited = false;
    forceReconnect = false;
    lastReconnectAttempt = millis();
  }

  static void stopAp() {
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    apActive = false;
  }

  void registerRoutes() {
    server.on(F("/pair/control"), HTTP_GET, [this](AsyncWebServerRequest* request) {
      if (_role != ROLE_LEADER) {
        request->send(403, F("application/json"), F("{\"error\":\"not leader\"}"));
        return;
      }
      _followerLastSeen = millis();
      String body;
      body.reserve(180);
      body += F("{\"schema\":1,\"boot\":");
      body += _bootId;
      body += F(",\"scene\":");
      body += _sceneRevision;
      body += F(",\"resources\":");
      body += _resourceGeneration;
      body += '}';
      request->send(200, F("application/json"), body);
    });

    server.on(F("/pair/state"), HTTP_GET, [this](AsyncWebServerRequest* request) {
      if (_role != ROLE_LEADER) {
        request->send(403, F("application/json"), F("{\"error\":\"not leader\"}"));
        return;
      }
      String body;
      if (!serializeScene(body)) {
        request->send(503, F("application/json"), F("{\"error\":\"state unavailable\"}"));
        return;
      }
      request->send(200, F("application/json"), body);
    });

    server.on(F("/pair/manifest"), HTTP_GET, [this](AsyncWebServerRequest* request) {
      if (_role != ROLE_LEADER) {
        request->send(403, F("application/json"), F("{\"error\":\"not leader\"}"));
        return;
      }
      request->send(200, F("application/json"), String(_manifestJson[_manifestIndex]));
    });

    server.on(F("/pair/file"), HTTP_GET, [this](AsyncWebServerRequest* request) {
      if (_role != ROLE_LEADER || !request->hasParam(F("id"))) {
        request->send(404, F("text/plain"), F("Not found"));
        return;
      }
      const int id = request->getParam(F("id"))->value().toInt();
      if (id < 1 || id > 255) {
        request->send(404, F("text/plain"), F("Not found"));
        return;
      }
      char path[10];
      snprintf(path, sizeof(path), "/%d.bmp", id);
      request->send(WLED_FS, path, F("image/bmp"));
    });
  }

  bool serializeScene(String& output) {
    JSONBufferGuard guard(USERMOD_ID_PAIR_SYNC);
    if (!guard) return false;
    JsonObject envelope = pDoc->to<JsonObject>();
    envelope[F("schema")] = 1;
    envelope[F("boot")] = _bootId;
    envelope[F("revision")] = _sceneRevision;
    JsonObject state = envelope.createNestedObject(F("state"));
    serializeState(state, false, true, true, false);
    state.remove(F("ps"));
    state.remove(F("pl"));
    state.remove(F("ledmap"));
    state.remove(F("udpn"));
    state.remove(F("PairSync"));
    if (pDoc->overflowed()) return false;
    output.reserve(measureJson(*pDoc) + 1U);
    serializeJson(*pDoc, output);
    return true;
  }

  void pollControl() {
    PSRAMDynamicJsonDocument doc(1024);
    if (!httpGetJson(endpoint(F("/pair/control")), doc)) return;
    if (doc[F("schema")].as<uint8_t>() != 1) return;

    const uint32_t boot = doc[F("boot")].as<uint32_t>();
    const uint32_t scene = doc[F("scene")].as<uint32_t>();
    const uint32_t resources = doc[F("resources")].as<uint32_t>();

    if (boot != _leaderBootId) {
      _leaderBootId = boot;
      _localSceneRevision = 0;
    }

    if (scene != _localSceneRevision && fetchScene()) return;
    if (resources != _localResourceGeneration) syncResources(resources);
  }

  bool fetchScene() {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, endpoint(F("/pair/state")))) return false;
    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
      setHttpError(F("state"), status);
      http.end();
      return false;
    }

    PSRAMDynamicJsonDocument doc(2U * JSON_BUFFER_SIZE);
    const DeserializationError error = deserializeJson(doc, http.getStream());
    http.end();
    if (error || doc[F("schema")].as<uint8_t>() != 1 || doc[F("state")].isNull()) {
      _lastError = F("invalid leader state");
      return false;
    }

    _applyingRemote = true;
    deserializeState(doc[F("state")].as<JsonObject>(), CALL_MODE_NO_NOTIFY);
    _applyingRemote = false;
    _localSceneRevision = doc[F("revision")].as<uint32_t>();
    _lastError = String();
    return true;
  }

  template<typename Document>
  bool httpGetJson(const String& url, Document& doc) {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) return false;
    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
      setHttpError(F("control"), status);
      http.end();
      return false;
    }
    const DeserializationError error = deserializeJson(doc, http.getStream());
    http.end();
    if (error) {
      _lastError = F("invalid pair response");
      return false;
    }
    _lastError = String();
    return true;
  }

  String endpoint(const __FlashStringHelper* path) const {
    String url(F("http://"));
    url += _leaderHost;
    url += path;
    return url;
  }

  void setHttpError(const __FlashStringHelper* operation, int status) {
    _lastError = operation;
    _lastError += F(" HTTP ");
    _lastError += status;
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

  template<typename T>
  static bool readValue(File& file, T& value) {
    return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == sizeof(T);
  }

  static bool validBmp(const char* path) {
    File file = WLED_FS.open(path, "r");
    if (!file) return false;
    uint16_t signature, planes, depth;
    uint32_t ignored, offset, dibSize, compression;
    int32_t width, height;
    bool valid = readValue(file, signature) && signature == 0x4D42
      && readValue(file, ignored) && readValue(file, ignored) && readValue(file, offset)
      && readValue(file, dibSize) && dibSize >= 40
      && readValue(file, width) && readValue(file, height)
      && readValue(file, planes) && readValue(file, depth) && readValue(file, compression)
      && width > 0 && height != 0 && height != INT32_MIN && planes == 1 && depth == 24 && compression == 0;
    if (valid) {
      const uint32_t uWidth = uint32_t(width);
      const uint32_t uHeight = height < 0 ? uint32_t(-height) : uint32_t(height);
      valid = uWidth <= (UINT32_MAX - 3U) / 3U;
      if (valid) {
        const uint32_t rowSize = (uWidth * 3U + 3U) & ~3U;
        const size_t fileSize = file.size();
        valid = uHeight <= SIZE_MAX / rowSize && offset >= 14U + dibSize && offset <= fileSize
          && size_t(rowSize) * uHeight <= fileSize - offset;
      }
    }
    file.close();
    return valid;
  }

  static bool hashFile(const char* path, uint8_t result[32], uint32_t& size) {
    File file = WLED_FS.open(path, "r");
    if (!file) return false;
    size = file.size();
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    mbedtls_sha256_starts(&context, 0);
    uint8_t buffer[1024];
    while (file.available()) {
      const size_t count = file.read(buffer, sizeof(buffer));
      if (!count) break;
      mbedtls_sha256_update(&context, buffer, count);
      yield();
    }
    const bool complete = file.position() == file.size();
    file.close();
    mbedtls_sha256_finish(&context, result);
    mbedtls_sha256_free(&context);
    return complete;
  }

  static void sortResources(ResourceEntry* entries, uint16_t count) {
    for (uint16_t i = 1; i < count; i++) {
      const ResourceEntry value = entries[i];
      int j = int(i) - 1;
      while (j >= 0 && entries[j].id > value.id) {
        entries[j + 1] = entries[j];
        j--;
      }
      entries[j + 1] = value;
    }
  }

  static uint32_t resourceGeneration(const ResourceEntry* entries, uint16_t count) {
    uint32_t hash = 2166136261UL;
    for (uint16_t i = 0; i < count; i++) {
      hash = (hash ^ entries[i].id) * 16777619UL;
      for (uint8_t shift = 0; shift < 32; shift += 8) hash = (hash ^ uint8_t(entries[i].size >> shift)) * 16777619UL;
      for (uint8_t b : entries[i].sha256) hash = (hash ^ b) * 16777619UL;
    }
    return hash ? hash : 1;
  }

  static void appendHex(String& output, const uint8_t hash[32]) {
    static const char digits[] = "0123456789abcdef";
    for (uint8_t i = 0; i < 32; i++) {
      output += digits[hash[i] >> 4];
      output += digits[hash[i] & 0x0F];
    }
  }

  static bool parseHex(const char* input, uint8_t output[32]) {
    if (!input || strlen(input) != 64) return false;
    for (uint8_t i = 0; i < 32; i++) {
      const int high = hexNibble(input[i * 2]);
      const int low = hexNibble(input[i * 2 + 1]);
      if (high < 0 || low < 0) return false;
      output[i] = uint8_t((high << 4) | low);
    }
    return true;
  }

  static int hexNibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  }

  void scanResources() {
    if (!_resources[0] || !_resources[1]) {
      _lastError = F("resource memory unavailable");
      return;
    }
    ResourceEntry* found = _resources[1U - _resourceIndex];
    uint16_t count = 0;
    File root = WLED_FS.open("/", "r");
    if (!root) {
      _lastError = F("resource scan failed");
      return;
    }
    File file = root.openNextFile();
    while (file) {
      const uint8_t id = file.isDirectory() ? 0 : parseImageId(file.name());
      char path[10];
      if (id && count < 255) {
        snprintf(path, sizeof(path), "/%u.bmp", unsigned(id));
        if (validBmp(path) && hashFile(path, found[count].sha256, found[count].size)) {
          found[count++].id = id;
        }
      }
      file.close();
      file = root.openNextFile();
    }
    root.close();
    sortResources(found, count);
    const uint32_t generation = resourceGeneration(found, count);
    if (generation == _resourceGeneration) return;

    _resourceIndex = 1U - _resourceIndex;
    _resourceCount = count;
    _resourceGeneration = generation;
    const uint8_t manifest = 1U - _manifestIndex;
    String& json = _manifestJson[manifest];
    json = F("{\"schema\":1,\"generation\":");
    json += generation;
    json += F(",\"files\":[");
    for (uint16_t i = 0; i < count; i++) {
      if (i) json += ',';
      json += F("{\"id\":");
      json += found[i].id;
      json += F(",\"size\":");
      json += found[i].size;
      json += F(",\"sha256\":\"");
      appendHex(json, found[i].sha256);
      json += F("\"}");
    }
    json += F("]}");
    _manifestIndex = manifest;
  }

  bool syncResources(uint32_t expectedGeneration) {
    PSRAMDynamicJsonDocument doc(32768);
    if (!httpGetJson(endpoint(F("/pair/manifest")), doc)) return false;
    if (doc[F("schema")].as<uint8_t>() != 1 || doc[F("generation")].as<uint32_t>() != expectedGeneration) return false;
    JsonArray files = doc[F("files")].as<JsonArray>();
    if (files.size() > 255) return false;

    ResourceEntry* desired = static_cast<ResourceEntry*>(allocate_buffer(sizeof(ResourceEntry) * 255U, BFRALLOC_PREFER_PSRAM));
    if (!desired) return false;
    uint16_t count = 0;
    bool valid = true;
    for (JsonObject item : files) {
      const int id = item[F("id")].as<int>();
      const uint32_t size = item[F("size")].as<uint32_t>();
      if (id < 1 || id > 255 || size == 0 || size > MAX_BMP_SIZE || !parseHex(item[F("sha256")], desired[count].sha256)) {
        valid = false;
        break;
      }
      desired[count].id = uint8_t(id);
      desired[count].size = size;
      count++;
    }
    if (!valid) {
      free(desired);
      _lastError = F("invalid resource manifest");
      return false;
    }

    sortResources(desired, count);
    for (uint16_t i = 0; i < count; i++) {
      if (!resourceMatches(desired[i]) && !downloadResource(desired[i])) {
        free(desired);
        return false;
      }
    }

    removeStaleResources(desired, count);
    free(desired);
    _localResourceGeneration = expectedGeneration;
    updateFSInfo();
    if (requestNumberedPovRescan) requestNumberedPovRescan();
    _lastError = String();
    return true;
  }

  static bool resourceMatches(const ResourceEntry& expected) {
    char path[10];
    snprintf(path, sizeof(path), "/%u.bmp", unsigned(expected.id));
    uint8_t actual[32];
    uint32_t size;
    return hashFile(path, actual, size) && size == expected.size && memcmp(actual, expected.sha256, sizeof(actual)) == 0 && validBmp(path);
  }

  bool downloadResource(const ResourceEntry& expected) {
    String url = endpoint(F("/pair/file?id="));
    url += expected.id;
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) return false;
    const int status = http.GET();
    if (status != HTTP_CODE_OK || (http.getSize() >= 0 && uint32_t(http.getSize()) != expected.size)) {
      setHttpError(F("BMP"), status);
      http.end();
      return false;
    }

    char temporary[12];
    char backup[12];
    snprintf(temporary, sizeof(temporary), "/%u.tmp", unsigned(expected.id));
    snprintf(backup, sizeof(backup), "/%u.bak", unsigned(expected.id));
    WLED_FS.remove(temporary);
    File output = WLED_FS.open(temporary, "w");
    if (!output) {
      http.end();
      _lastError = F("cannot create BMP temporary file");
      return false;
    }
    const int written = http.writeToStream(&output);
    output.close();
    http.end();

    uint8_t actual[32];
    uint32_t size;
    if (written < 0 || !hashFile(temporary, actual, size) || size != expected.size
        || memcmp(actual, expected.sha256, sizeof(actual)) != 0 || !validBmp(temporary)) {
      WLED_FS.remove(temporary);
      _lastError = F("BMP verification failed");
      return false;
    }

    char destination[10];
    snprintf(destination, sizeof(destination), "/%u.bmp", unsigned(expected.id));
    WLED_FS.remove(backup);
    const bool hadOriginal = WLED_FS.exists(destination);
    if (hadOriginal && !WLED_FS.rename(destination, backup)) {
      WLED_FS.remove(temporary);
      _lastError = F("cannot back up BMP");
      return false;
    }
    if (!WLED_FS.rename(temporary, destination)) {
      if (hadOriginal) WLED_FS.rename(backup, destination);
      WLED_FS.remove(temporary);
      _lastError = F("cannot install BMP");
      return false;
    }
    WLED_FS.remove(backup);
    return true;
  }

  static bool containsResource(const ResourceEntry* resources, uint16_t count, uint8_t id) {
    for (uint16_t i = 0; i < count; i++) if (resources[i].id == id) return true;
    return false;
  }

  static void recoverResourceTransactions() {
    for (uint16_t id = 1; id <= 255; id++) {
      char destination[10];
      char temporary[12];
      char backup[12];
      snprintf(destination, sizeof(destination), "/%u.bmp", unsigned(id));
      snprintf(temporary, sizeof(temporary), "/%u.tmp", unsigned(id));
      snprintf(backup, sizeof(backup), "/%u.bak", unsigned(id));
      WLED_FS.remove(temporary);
      if (WLED_FS.exists(backup)) {
        if (WLED_FS.exists(destination)) WLED_FS.remove(backup);
        else WLED_FS.rename(backup, destination);
      }
    }
  }

  static void removeStaleResources(const ResourceEntry* desired, uint16_t count) {
    uint8_t stale[255];
    uint16_t staleCount = 0;
    File root = WLED_FS.open("/", "r");
    if (root) {
      File file = root.openNextFile();
      while (file) {
        const uint8_t id = file.isDirectory() ? 0 : parseImageId(file.name());
        if (id && !containsResource(desired, count, id)) stale[staleCount++] = id;
        file.close();
        file = root.openNextFile();
      }
      root.close();
    }
    for (uint16_t i = 0; i < staleCount; i++) {
      char path[10];
      snprintf(path, sizeof(path), "/%u.bmp", unsigned(stale[i]));
      WLED_FS.remove(path);
    }
  }

  uint8_t _role = ROLE_DISABLED;
  String _leaderHost = F("4.3.2.1");
  String _lastError;
  bool _setupDone = false;
  bool _rolePending = false;
  bool _applyingRemote = false;
  uint32_t _bootId = 0;
  uint32_t _leaderBootId = 0;
  uint32_t _sceneRevision = 1;
  uint32_t _localSceneRevision = 0;
  uint32_t _resourceGeneration = 0;
  uint32_t _localResourceGeneration = 0;
  uint32_t _followerLastSeen = 0;
  uint32_t _lastControlPoll = 0;
  uint32_t _lastResourceScan = 0;
  ResourceEntry* _resources[2] = {nullptr, nullptr};
  uint16_t _resourceCount = 0;
  uint8_t _resourceIndex = 0;
  String _manifestJson[2] = {F("{\"schema\":1,\"generation\":0,\"files\":[]}"), F("{\"schema\":1,\"generation\":0,\"files\":[]}")};
  uint8_t _manifestIndex = 0;
};

PairSyncUsermod pairSync;
REGISTER_USERMOD(pairSync);

} // namespace

bool usermodBroadcastIP(IPAddress& address) {
  return pairSync.broadcastIP(address);
}
