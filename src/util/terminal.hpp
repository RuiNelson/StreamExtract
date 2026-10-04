#pragma once

#include <optional>
#include <string>

namespace streamextract {

bool stdin_is_terminal();
bool stdout_is_terminal();

// Asks for a secret on the terminal without echoing it. The prompt goes to
// stderr. Returns std::nullopt if no line could be read.
std::optional<std::string> prompt_hidden(const std::string& prompt);

// Puts the Windows console in UTF-8 mode; no-op elsewhere.
void setup_console();

// SIGINT/SIGTERM set a flag instead of killing the process, so a transfer can
// be cancelled cleanly.
void install_interrupt_handler();
bool interrupt_requested();

}  // namespace streamextract
