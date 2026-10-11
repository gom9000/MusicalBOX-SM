#include "config/JsonUtil.h"

#include <cstring>
#include <fstream>

namespace mbx::jsonutil {

bool readFile(const std::filesystem::path& file, nlohmann::json& out, std::string& error) {
    std::ifstream in(file);
    if (!in) {
        error = "cannot open " + file.string();
        return false;
    }
    try {
        out = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    } catch (const nlohmann::json::parse_error& e) {
        error = file.string() + ": " + e.what();
        return false;
    }
    return true;
}

void checkKeys(const nlohmann::json& obj, std::initializer_list<const char*> allowed, const std::string& where,
               std::vector<std::string>& warnings) {
    if (!obj.is_object()) return;
    for (const auto& item : obj.items()) {
        const std::string& key = item.key();
        if (key == "$schema" || (!key.empty() && key[0] == '_')) continue;
        bool known = false;
        for (const char* a : allowed)
            if (key == a) known = true;
        if (!known) warnings.push_back(where + ": unknown key \"" + key + "\" ignored");
    }
}

} // namespace mbx::jsonutil
