#ifndef FLIRT_COMMON_COMPRESSION_H
#define FLIRT_COMMON_COMPRESSION_H

// The ROOT files' compression (flirt-convert --compression). Plain C++, no
// ROOT.

#include <string>
#include <vector>

namespace flirt
{

struct Compression
{
    const char *name;   // what --compression takes and the report records
    int setting;        // ROOT's algorithm * 100 + level, as TFile takes it
};

// The choices, the default first: "mvme", the TFile default mvme_root_client
// writes with (ZLIB level 1, 101), the only one whose files are identical to
// official ones. A choice is renamed in compression.cc's table alone.
const std::vector<Compression> &compressions();

const Compression &default_compression();

// The choice called `name`, nullptr if there is none.
const Compression *find_compression(const std::string &name);

// "mvme, fast or small", for messages.
std::string compression_names();

}

#endif
