#pragma once
#include <string>
#include <vector>

// Runs a command-line task if the arguments ask for one. Returns its exit code, or -1 when the
// arguments are for the windowed app.
int runHeadless(const std::vector<std::string>& args);
