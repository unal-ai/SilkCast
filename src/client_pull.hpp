#pragma once

#include <string>

// Starts the client-side pull loop.
// connect_to: "ip" or "ip:port" (default port 8080 if not specified)
int run_client(const std::string &connect_to,
               const std::string &device_id = "video0");
