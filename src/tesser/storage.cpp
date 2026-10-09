#include "tesser/storage.h"

#include <stdio.h>

#if defined(ESP_PLATFORM)
#include "nvs.h"
#endif

namespace tesser {

bool FileStorage::load(std::string& out) {
    FILE* f = fopen(path_.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return !out.empty();
}

bool FileStorage::save(const std::string& data) {
    std::string tmp = path_ + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path_.c_str()) != 0) {
        // Some VFS drivers refuse to rename over an existing file.
        remove(path_.c_str());
        if (rename(tmp.c_str(), path_.c_str()) != 0) return false;
    }
    return true;
}

#if defined(ESP_PLATFORM)
bool NvsStorage::load(std::string& out) {
    nvs_handle_t h;
    if (nvs_open(namespace_, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool ok = nvs_get_blob(h, key_, nullptr, &len) == ESP_OK && len > 0;
    if (ok) {
        out.resize(len);
        ok = nvs_get_blob(h, key_, &out[0], &len) == ESP_OK;
        out.resize(len);
    }
    nvs_close(h);
    return ok;
}

bool NvsStorage::save(const std::string& data) {
    nvs_handle_t h;
    if (nvs_open(namespace_, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, key_, data.data(), data.size()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
#endif

}  // namespace tesser
