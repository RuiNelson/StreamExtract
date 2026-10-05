#pragma once

#include <string>
#include <vector>

#include "ftp_client.hpp"

namespace streamextract {
// Resolves the local username and home paths without consulting ssh-agent or SSH config.
std::vector<std::string> prepare_ssh_settings(FtpConfig& config);
std::string current_username();
std::string user_home();
}  // namespace streamextract
