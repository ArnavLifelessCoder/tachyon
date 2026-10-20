#pragma once

#include <filesystem>
#include <span>

int parser_cli(std::span<const std::filesystem::path> args);
