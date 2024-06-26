// This file is generated by tools/codegen.sh
// DO NOT EDIT IT.

// clang-format off

//  To parse this JSON data, first install
//
//      json.hpp  https://github.com/nlohmann/json
//
//  Then include this file, and then do
//
//     BuilderProjectSource.hpp data = nlohmann::json::parse(jsonString);

#pragma once

#include <optional>
#include <nlohmann/json.hpp>
#include "linglong/api/types/v1/helper.hpp"

namespace linglong {
namespace api {
namespace types {
namespace v1 {
using nlohmann::json;

struct BuilderProjectSource {
std::optional<std::string> commit;
std::optional<std::string> digest;
std::string kind;
std::optional<std::string> url;
std::optional<std::string> version;
};
}
}
}
}

// clang-format on
