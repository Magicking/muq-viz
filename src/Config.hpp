#pragma once
// Simple INI config file for muq-viz Options.
//
//   [section]
//   key = value      ; '#' or ';' start a comment line
//
// Unknown keys and invalid values produce warnings; everything else is applied.

#include "Options.hpp"

#include <string>
#include <vector>

namespace muq {

/// $XDG_CONFIG_HOME/muq-viz/muq-viz.ini, else $HOME/.config/muq-viz/muq-viz.ini (empty if neither is set).
std::string DefaultConfigPath();

/// Applies an INI string on top of `opt`. Returns the list of warnings (line-numbered).
std::vector<std::string> ParseConfig(const std::string& text, Options& opt);

/// Reads and applies `path`. Missing file: returns false, no warnings. Read error: false + warning.
bool LoadConfig(const std::string& path, Options& opt, std::vector<std::string>& warnings);

/// Serializes all persistable options.
std::string FormatConfig(const Options& opt);

/// Writes FormatConfig(opt) atomically (tmp + rename), creating parent directories.
bool SaveConfig(const std::string& path, const Options& opt, std::string& error);

} // namespace muq
