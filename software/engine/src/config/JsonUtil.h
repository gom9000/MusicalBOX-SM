#pragma once

#include "nlohmann/json.hpp"

#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace mbx::jsonutil {

// Reads a JSON file. // and /* */ comments are allowed.
bool readFile(const std::filesystem::path& file, nlohmann::json& out, std::string& error);

// Adds a warning for every key of `obj` not in `allowed` (catches typos such
// as "atack_ms"). "$schema" and keys starting with "_" are always accepted,
// so "_comment" can be used for notes.
void checkKeys(const nlohmann::json& obj, std::initializer_list<const char*> allowed, const std::string& where,
               std::vector<std::string>& warnings);

} // namespace mbx::jsonutil
