#pragma once

#include <optional>
#include <string_view>

// Access to files baked into the binary at build time (see cmake/embed.cmake).
namespace es::embedded {

std::optional<std::string_view> get(std::string_view id);

} // namespace es::embedded
