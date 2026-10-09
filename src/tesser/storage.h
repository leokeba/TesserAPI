#pragma once

#include <string>

namespace tesser {

// Where Api persistence keeps its state: one JSON document, read and written
// whole (docs/DESIGN.md section 12).
class Storage {
public:
    virtual ~Storage() = default;
    // Fills `out` and returns true if something was stored.
    virtual bool load(std::string& out) = 0;
    virtual bool save(const std::string& data) = 0;
};

// In memory: for tests, or to hand the state to something else.
class MemoryStorage : public Storage {
public:
    bool load(std::string& out) override {
        if (data.empty()) return false;
        out = data;
        return true;
    }
    bool save(const std::string& d) override {
        data = d;
        saves++;
        return true;
    }
    std::string data;
    int saves = 0;
};

// A file, through stdio: works on any mounted VFS (LittleFS, SPIFFS, FAT) on
// ESP-IDF and Arduino, and on the host. Writes go to "<path>.tmp" first and
// are renamed into place, so a power cut leaves the old or the new state.
class FileStorage : public Storage {
public:
    explicit FileStorage(std::string path) : path_(std::move(path)) {}
    bool load(std::string& out) override;
    bool save(const std::string& data) override;

private:
    std::string path_;
};

#if defined(ESP_PLATFORM)
// An NVS blob. The application must have called nvs_flash_init() (Arduino
// does it at boot). Blobs are limited to about 500 KB, far above typical
// state.
class NvsStorage : public Storage {
public:
    explicit NvsStorage(const char* nameSpace = "tesser", const char* key = "state")
        : namespace_(nameSpace), key_(key) {}
    bool load(std::string& out) override;
    bool save(const std::string& data) override;

private:
    const char* namespace_;
    const char* key_;
};
#endif

}  // namespace tesser
