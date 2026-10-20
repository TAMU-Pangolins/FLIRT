#ifndef FLIRT_APPS_DITHER_OFFSET_H
#define FLIRT_APPS_DITHER_OFFSET_H

// Finding the dither offset of an official _raw.root (docs/design.md,
// "Finding a file's dither offset"): mvme_root_client adds
// gRandom->Uniform() to every hit (mvme_root_client.cc:1016-1020), gRandom a
// TRandom3(4357) whose draws continue across the runs of one client process.
// The first hits' fractions, y = frac * 2^32 (exact when |floor| < 2^21), in
// the client's fill order (mvme_root_client.cc:974-1016), are looked up in
// the TRandom3(4357) stream; the offset is the number of draws before them.

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace flirt
{

// Draws searched: 2^32 (about a minute, README "Dither offset").
constexpr uint64_t DitherSearchDraws = uint64_t(1) << 32;

// What an official _raw.root tells about its dither offset.
struct OfficialRaw
{
    std::string path;               // as given
    std::string problem;            // why no offset can be found; empty if found (or still to search)
    int64_t size = 0, mtimeNs = 0;
    bool runInfo = false;           // has mvme's MVMERunInfo
    std::string runId;              // its RunID
    struct Tree
    {
        std::string name;
        int64_t entries = 0;
        std::vector<std::string> branches;  // top-level (the modules)
    };
    std::vector<Tree> trees;
    bool heldTwice = false;         // the run twice in one file: the first copy's offset
    bool noHits = false;            // every offset gives the same file
    std::vector<std::pair<uint32_t, uint32_t>> anchors;    // (fill position, y) of the first usable hits
    bool found = false;
    uint64_t offset = 0;
};

// Reads the file's run information, trees and first hits; ROOT opens it as
// /dev/fd/<n>, never by its name. Sets `problem` when the file cannot give an
// offset. ROOT's messages are kept out of the terminal.
OfficialRaw read_official_raw(const std::string &path);

// Scans the first `draws` draws of TRandom3(4357) once for every file that
// has anchors and no problem, setting `found` and `offset`, or `problem` when
// it is not found. progress(draws done) about every 16 million draws; the
// scan stops early when keepGoing() returns false (`problem` then says so).
void search_dither_offsets(std::vector<OfficialRaw *> files, uint64_t draws,
                           const std::function<void(uint64_t)> &progress,
                           const std::function<bool()> &keepGoing);

nlohmann::json official_raw_json(const OfficialRaw &raw);
OfficialRaw official_raw_from_json(const nlohmann::json &json);

// The mode `flirt-convert --find-dither-offset <official _raw.root>...`:
// one line per file on stdout, converts nothing. Exit 0 if every offset was
// found, 1 if not, 2 for a usage error.
int run_find_dither_offset(int argc, char *argv[]);

// mvme's name for a run recorded live: its start time, yyMMdd_HHmmss
// (mvme_context.cc:1718); a replay's is the listfile's name (:1814).
bool live_run_id(const std::string &runId);

// The hidden helper `flirt-convert --official-raw-facts <file> [<runId>]` a conversion
// with --dither-offset-from runs in a process of its own (reading the file
// must not leave ROOT classes behind in the converting process): the facts
// and the offset as one JSON line on stdout; with <runId>, a file of another
// run (not a live recording's) is not searched. Stops if its parent goes away.
int run_official_raw_facts(int argc, char *argv[]);

// Runs that helper on `path` for the run `runId` and returns what it found;
// throws std::runtime_error if it cannot run or gives no answer.
OfficialRaw official_raw_facts(const std::string &path, const std::string &runId);

}

#endif
