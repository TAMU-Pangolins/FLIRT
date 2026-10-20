#include "compression.h"

namespace flirt
{

const std::vector<Compression> &compressions()
{
    // ROOT's settings (Compression.h): 101 ZLIB level 1, 4xx LZ4, 5xx ZSTD.
    // Reading any of them needs nothing new in ROOT; ZSTD needs ROOT >= 6.20.
    // The names are what the user sees: each is renamed on its line alone
    // (docs/design.md, "Compression": measurements).
    static const std::vector<Compression> table = {
        { "mvme", 101, "files identical to official ones (ZLIB level 1)" },
        { "fast", 501, "faster, files a little bigger (ZSTD level 1)" },
        { "small", 509, "smaller files, many times slower (ZSTD level 9)" },
    };
    return table;
}

const Compression &default_compression()
{
    return compressions().front();
}

const Compression *find_compression(const std::string &name)
{
    for (const auto &c: compressions())
        if (name == c.name)
            return &c;
    return nullptr;
}

std::string compression_names()
{
    const auto &all = compressions();
    std::string s;
    for (size_t i = 0; i < all.size(); ++i)
        s += (i == 0 ? "" : i + 1 == all.size() ? " or " : ", ") + std::string(all[i].name);
    return s;
}

}
