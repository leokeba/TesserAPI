#include "tesser/storage.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(ESP_PLATFORM)
#include "nvs.h"
#endif

namespace tesser {

std::string FileStorage::path(std::string_view key) const {
    std::string p = dir_;
    p += '/';
    p.append(key.data(), key.size());
    p += ".json";
    return p;
}

bool FileStorage::load(std::string_view key, std::string& out) {
    FILE* f = fopen(path(key).c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return !out.empty();
}

bool FileStorage::save(std::string_view key, const std::string& data) {
    mkdir(dir_.c_str(), 0755);  // usually exists already; SPIFFS has no directories at all
    std::string file = path(key);
    std::string tmp = file + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), file.c_str()) != 0) {
        // Some VFS drivers refuse to rename over an existing file.
        remove(file.c_str());
        if (rename(tmp.c_str(), file.c_str()) != 0) return false;
    }
    return true;
}

bool FileStorage::erase() {
    DIR* d = opendir(dir_.c_str());
    if (!d) return true;  // nothing stored
    bool ok = true;
    while (struct dirent* e = readdir(d)) {
        size_t len = strlen(e->d_name);
        bool record = len > 5 && strcmp(e->d_name + len - 5, ".json") == 0;
        bool tmp = len > 9 && strcmp(e->d_name + len - 9, ".json.tmp") == 0;
        if (!record && !tmp) continue;
        std::string file = dir_ + "/" + e->d_name;
        if (remove(file.c_str()) != 0) ok = false;
    }
    closedir(d);
    return ok;
}

#if defined(ESP_PLATFORM)
namespace {

// NVS keys hold at most 15 characters: longer ones keep their first 7 and
// get an FNV-1a hash of the whole key.
std::string nvsKey(std::string_view key) {
    if (key.size() <= 15) return std::string(key);
    uint32_t h = 2166136261u;
    for (char c : key) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.7s%08lx", std::string(key.substr(0, 7)).c_str(), static_cast<unsigned long>(h));
    return buf;
}

}  // namespace

bool NvsStorage::load(std::string_view key, std::string& out) {
    nvs_handle_t h;
    if (nvs_open(namespace_, NVS_READONLY, &h) != ESP_OK) return false;
    std::string k = nvsKey(key);
    size_t len = 0;
    bool ok = nvs_get_blob(h, k.c_str(), nullptr, &len) == ESP_OK && len > 0;
    if (ok) {
        out.resize(len);
        ok = nvs_get_blob(h, k.c_str(), &out[0], &len) == ESP_OK;
        out.resize(len);
    }
    nvs_close(h);
    return ok;
}

bool NvsStorage::save(std::string_view key, const std::string& data) {
    nvs_handle_t h;
    if (nvs_open(namespace_, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, nvsKey(key).c_str(), data.data(), data.size()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool NvsStorage::erase() {
    nvs_handle_t h;
    esp_err_t err = nvs_open(namespace_, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;  // never written
    if (err != ESP_OK) return false;
    bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
#endif

}  // namespace tesser
