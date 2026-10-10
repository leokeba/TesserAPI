#pragma once

#include <map>
#include <string>
#include <string_view>

namespace tesser {

// Where Api persistence keeps its state (docs/DESIGN.md section 12): one
// record per top-level node holding persisted values, stored under the
// node's name, so a change rewrites only its own record.
class Storage {
public:
    virtual ~Storage() = default;
    // Fills `out` with the record stored under `key`; false if there is none.
    virtual bool load(std::string_view key, std::string& out) = 0;
    virtual bool save(std::string_view key, const std::string& data) = 0;
    // Removes every record (factory reset). The application's state keeps
    // its current values until the next boot.
    virtual bool erase() = 0;
};

// In memory: for tests, or to hand the state to something else.
class MemoryStorage : public Storage {
public:
    bool load(std::string_view key, std::string& out) override {
        auto it = records.find(std::string(key));
        if (it == records.end()) return false;
        out = it->second;
        return true;
    }
    bool save(std::string_view key, const std::string& data) override {
        records[std::string(key)] = data;
        saves++;
        return true;
    }
    bool erase() override {
        records.clear();
        return true;
    }
    std::map<std::string, std::string> records;
    int saves = 0;  // records written
};

// Files in a directory, through stdio: works on any mounted VFS (LittleFS,
// SPIFFS, FAT) on ESP-IDF and Arduino, and on the host. A record is
// "<dir>/<key>.json". Writes go to "<file>.tmp" first and are renamed into
// place, so a power cut leaves the old or the new record.
class FileStorage : public Storage {
public:
    // The directory is created when the first record is saved.
    explicit FileStorage(std::string dir) : dir_(std::move(dir)) {}
    bool load(std::string_view key, std::string& out) override;
    bool save(std::string_view key, const std::string& data) override;
    bool erase() override;

private:
    std::string path(std::string_view key) const;
    std::string dir_;
};

#if defined(ESP_PLATFORM)
// NVS blobs, one per record, in one namespace. NVS spreads writes across
// its pages. Keys longer than NVS's 15 characters are shortened with a hash.
// The application must have called nvs_flash_init() (Arduino does it at
// boot). A blob is limited to about 500 KB, far above typical state.
class NvsStorage : public Storage {
public:
    explicit NvsStorage(const char* nameSpace = "tesser") : namespace_(nameSpace) {}
    bool load(std::string_view key, std::string& out) override;
    bool save(std::string_view key, const std::string& data) override;
    bool erase() override;

private:
    const char* namespace_;
};
#endif

}  // namespace tesser
