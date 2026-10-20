#ifndef FLIRT_UI_SPAWN_H
#define FLIRT_UI_SPAWN_H

// The helper program flirt-tui runs next to flirt-convert (flirt-waveforms),
// started as JobManager starts flirt-convert (job.cc
// JobManager::start): its own process group, default signal actions, no
// signal blocked.

#include <string>
#include <sys/types.h>
#include <vector>

namespace flirt
{

// This program's file name, for "flirt-waveforms is missing next to flirt-tui".
std::string program_name();

// Starts `exe` with `args` (argv[0] is `exe`): `stdinFd` on its stdin and
// `stdoutFd` on its stdout (-1: /dev/null), its stderr /dev/null. The caller
// closes its own copies of the fds. False with the sentence: "<name> is
// missing next to <program>", or why it could not start.
bool spawn_helper(const std::string &exe, const std::vector<std::string> &args, int stdinFd, int stdoutFd,
                  pid_t &pid, std::string &why);

}

#endif
