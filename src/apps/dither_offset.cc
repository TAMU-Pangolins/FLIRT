#include "dither_offset.h"

#include <TBranch.h>
#include <TClass.h>
#include <TError.h>
#include <TFile.h>
#include <TKey.h>
#include <TLeaf.h>
#include <TRandom3.h>
#include <TTree.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

#include "common/names.h"
#include "common/platform.h"
#include "common/utf8.h"

extern char **environ;

namespace flirt
{

using json = nlohmann::json;

namespace
{

// Anchors: the first usable hits within the first 2^16 fill positions; three
// make the key (96 bits), the others confirm a match.
constexpr size_t KeyAnchors = 3, MaxAnchors = 24;
constexpr uint32_t PositionLimit = 1u << 16;
// Hits whose fraction is checked before deciding the file is not dithered.
constexpr int CheckedHits = 256;
// Entries of each half compared to find a run held twice.
constexpr int64_t TwiceEntries = 200;
constexpr double TwoTo21 = 2097152.0, TwoTo32 = 4294967296.0;

std::string rootErrors;

// ROOT's warnings (e.g. "no dictionary for class") are dropped, its errors
// kept for the reason; nothing reaches the terminal.
void collect_root_errors(int level, Bool_t abort, const char *location, const char *message)
{
    if (abort)
    {
        fprintf(stderr, "Fatal in <%s>: %s\n", location ? location : "", message ? message : "");
        std::abort();
    }
    if (level < kError)
        return;
    rootErrors += (rootErrors.empty() ? "" : "; ") + std::string(message ? message : "");
}

// The name of a file in a message: invalid UTF-8 and control bytes as '?'.
std::string printable(const std::string &text)
{
    auto out = valid_utf8(text);
    for (auto &c: out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            c = '?';
    return out;
}

// The highest cycle of each TTree key (as tools/compare reads files).
std::vector<TTree *> event_trees(TFile *f)
{
    std::map<std::string, short> best;
    TIter next(f->GetListOfKeys());
    while (auto key = static_cast<TKey *>(next()))
    {
        auto cls = TClass::GetClass(key->GetClassName());
        if (cls && cls->InheritsFrom(TTree::Class()))
        {
            auto &c = best[key->GetName()];
            c = std::max(c, key->GetCycle());
        }
    }
    std::vector<TTree *> out;
    for (auto &[name, cycle]: best)
        if (auto t = f->Get<TTree>((name + ";" + std::to_string(cycle)).c_str()))
            out.push_back(t);
    return out;
}

void collect(TObjArray *branches, std::vector<TBranch *> &out)
{
    TIter next(branches);
    while (auto b = static_cast<TBranch *>(next()))
    {
        out.push_back(b);
        collect(b->GetListOfBranches(), out);
    }
}

// The Double_t leaves of a tree in depth-first branch order, which is the
// order mvme_root_client fills an entry's hits in: data sources in order,
// each in ascending index (mvme_root_client.cc:974-1016, event_server.cc:545-588).
struct Leaves
{
    std::vector<TBranch *> branches;
    std::vector<std::vector<double>> buffers;

    bool read(int64_t entry)
    {
        for (auto b: branches)
            if (b->GetEntry(entry) < 0)
                return false;
        return true;
    }
};

std::string bind_leaves(TTree *tree, Leaves &leaves)
{
    tree->SetMakeClass(1);
    std::vector<TBranch *> all;
    collect(tree->GetListOfBranches(), all);
    for (auto b: all)
    {
        if (b->GetListOfBranches()->GetEntries() > 0)
            continue;
        auto leaf = static_cast<TLeaf *>(b->GetListOfLeaves()->At(0));
        if (!leaf || std::string(leaf->GetTypeName()) != "Double_t")
            continue;
        if (b->GetListOfLeaves()->GetEntries() != 1 || leaf->GetLeafCount())
            return std::string("its branch ") + b->GetName() + " is not a fixed-size array as mvme_root_client writes";
        leaves.branches.push_back(b);
        leaves.buffers.emplace_back(std::max(1, leaf->GetLenStatic()));
    }
    for (size_t i = 0; i < leaves.branches.size(); ++i)
        leaves.branches[i]->SetAddress(leaves.buffers[i].data());
    return {};
}

// The first hits' fractions; sets anchors, noHits or problem.
void read_anchors(TTree *tree, OfficialRaw &raw)
{
    Leaves leaves;
    if (auto why = bind_leaves(tree, leaves); !why.empty())
    {
        raw.problem = why;
        return;
    }
    const int64_t entries = tree->GetEntries();
    uint32_t position = 0;
    int checked = 0, zero = 0, notDraws = 0;
    bool done = false;
    for (int64_t e = 0; e < entries && !done; ++e)
    {
        if (!leaves.read(e))
        {
            raw.problem = "entry " + std::to_string(e) + " cannot be read: the file is damaged";
            return;
        }
        for (const auto &buffer: leaves.buffers)
            for (double v: buffer)
            {
                if (std::isnan(v))
                    continue;
                const double fl = std::floor(v);
                if (std::fabs(fl) < TwoTo21 && checked < CheckedHits)
                {
                    ++checked;
                    const double y = (v - fl) * TwoTo32;
                    if (y != std::floor(y))
                        ++notDraws;
                    else if (y == 0)
                        ++zero;
                    else if (raw.anchors.size() < MaxAnchors && position < PositionLimit)
                        raw.anchors.emplace_back(position, static_cast<uint32_t>(y));
                }
                ++position;
                done = checked >= CheckedHits || position >= PositionLimit;
            }
    }
    tree->ResetBranchAddresses();

    if (position == 0)
    {
        raw.noHits = true;
        raw.problem = "the file has no hits, so every dither offset gives the same file";
    }
    else if (checked == 0)
        raw.problem = "none of its first values is below 2^21 (2097152), so no fraction can be read exactly";
    else if (zero == checked)
        raw.problem = "its values have no fractional part: it was not dithered, so it was not made by "
                      "mvme_root_client (which adds a random fraction to every value) or by FLIRT";
    else if (notDraws > 0 || zero > 0)
        raw.problem = "its values' fractional parts are not mvme_root_client's random draws (ROOT's "
                      "TRandom3): it was not made by mvme_root_client or by FLIRT";
    else if (raw.anchors.size() < KeyAnchors)
        raw.problem = "only " + std::to_string(raw.anchors.size()) + " of its first hits have a fraction that "
                      "can be read exactly (values below 2^21); 3 are needed";
}

// A run replayed twice into one file (mvme can write such a file): the second
// half repeats the first half's integer parts.
bool held_twice(TTree *tree)
{
    const int64_t entries = tree->GetEntries();
    if (entries < 2 || entries % 2)
        return false;
    Leaves a, b;
    if (!bind_leaves(tree, a).empty())
        return false;
    tree->ResetBranchAddresses();
    if (!bind_leaves(tree, b).empty())
        return false;
    // Two sets of buffers on the same branches: read each half in turn.
    const int64_t half = entries / 2, n = std::min(half, TwiceEntries);
    int64_t hits = 0;
    bool same = true;
    for (int64_t e = 0; e < n && same; ++e)
    {
        for (size_t i = 0; i < a.branches.size(); ++i)
            a.branches[i]->SetAddress(a.buffers[i].data());
        if (!a.read(e))
            return false;
        for (size_t i = 0; i < b.branches.size(); ++i)
            b.branches[i]->SetAddress(b.buffers[i].data());
        if (!b.read(half + e))
            return false;
        for (size_t i = 0; i < a.buffers.size() && same; ++i)
            for (size_t j = 0; j < a.buffers[i].size() && same; ++j)
            {
                const double x = a.buffers[i][j], y = b.buffers[i][j];
                if (std::isnan(x) != std::isnan(y) || (!std::isnan(x) && std::floor(x) != std::floor(y)))
                    same = false;
                hits += !std::isnan(x);
            }
    }
    tree->ResetBranchAddresses();
    return same && hits >= 20;
}

inline uint32_t draw_y(TRandom3 &r)
{
    // Rndm() is y * 2^-32 for TRandom3's 32-bit y (a y of 0 is drawn again).
    return static_cast<uint32_t>(r.Rndm() * TwoTo32);
}

std::string with_separators(uint64_t n)
{
    auto s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
        s.insert(static_cast<size_t>(i), ",");
    return s;
}

}

OfficialRaw read_official_raw(const std::string &path)
{
    OfficialRaw raw;
    raw.path = path;
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
    {
        raw.problem = errno == ENOENT || errno == ENOTDIR ? "no such file" : std::string("cannot read it: ") + std::strerror(errno);
        return raw;
    }
    if (S_ISDIR(st.st_mode))
    {
        raw.problem = "it is a folder, not a _raw.root file";
        return raw;
    }
    if (!S_ISREG(st.st_mode))
    {
        raw.problem = "it is not a regular file";
        return raw;
    }
    // O_NONBLOCK: a FIFO put in its place since the stat cannot block the open.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    struct stat fst;
    if (fd < 0 || fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode))
    {
        raw.problem = fd < 0 ? std::string("cannot open it: ") + std::strerror(errno) : "it is not a regular file";
        if (fd >= 0)
            ::close(fd);
        return raw;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    raw.size = static_cast<int64_t>(fst.st_size);
    const auto mtime = stat_mtime(fst);
    raw.mtimeNs = static_cast<int64_t>(mtime.tv_sec) * 1000000000 + mtime.tv_nsec;

    // ROOT cuts a local name at '#' or '?' and expands '$' and '~': it opens
    // the descriptor's name instead, whatever the file is called.
    const auto previous = SetErrorHandler(collect_root_errors);
    rootErrors.clear();
    {
        const auto fdName = "/dev/fd/" + std::to_string(fd);
        std::unique_ptr<TFile> file(TFile::Open(fdName.c_str(), "READ"));
        if (!file || file->IsZombie() || !file->IsOpen())
            raw.problem = "it is not a ROOT file, or it is damaged";
        else
        {
            if (auto info = file->Get<std::map<std::string, std::string>>("MVMERunInfo"))
            {
                raw.runInfo = true;
                if (auto it = info->find("RunID"); it != info->end())
                    raw.runId = it->second;
                delete info;
            }
            const auto trees = event_trees(file.get());
            for (auto t: trees)
            {
                OfficialRaw::Tree tree{t->GetName(), t->GetEntries(), {}};
                TIter next(t->GetListOfBranches());
                while (auto b = next())
                    tree.branches.push_back(b->GetName());
                raw.trees.push_back(tree);
            }
            if (trees.empty())
                raw.problem = "it holds no event tree: a _histos.root? The offset is found in the run's _raw.root";
            else if (!raw.runInfo)
                raw.problem = "it is not a _raw.root written by mvme_root_client or FLIRT (it has no MVMERunInfo)";
            else if (trees.size() > 1)
                raw.problem = "it has " + std::to_string(trees.size()) + " event trees; the fill order across "
                              "VME events is not in the file (FLIRT converts runs with one VME event)";
            else
            {
                read_anchors(trees[0], raw);
                if (raw.problem.empty())
                    raw.heldTwice = held_twice(trees[0]);
            }
        }
        file.reset();
    }
    ::close(fd);
    SetErrorHandler(previous);
    if (!raw.problem.empty() && !rootErrors.empty() && !raw.noHits)
        raw.problem += " (ROOT: " + rootErrors + ")";
    return raw;
}

void search_dither_offsets(std::vector<OfficialRaw *> all, uint64_t draws,
                           const std::function<void(uint64_t)> &progress,
                           const std::function<bool()> &keepGoing)
{
    std::vector<OfficialRaw *> files;
    for (auto f: all)
        if (f->problem.empty() && !f->found && f->anchors.size() >= KeyAnchors)
            files.push_back(f);
    if (files.empty())
        return;

    // Keyed on the third anchor; the first two are looked up in a ring of the
    // last draws, the others checked ahead on a copy of the generator.
    std::unordered_multimap<uint32_t, OfficialRaw *> byKey;
    std::vector<uint8_t> filter(1u << 24, 0);
    for (auto f: files)
    {
        byKey.emplace(f->anchors[2].second, f);
        filter[f->anchors[2].second >> 8] = 1;
    }
    constexpr uint64_t RingSize = PositionLimit;
    std::vector<uint32_t> ring(RingSize);
    size_t remaining = files.size();
    TRandom3 r(4357);
    uint64_t i = 0;
    bool stopped = false;
    for (; i < draws && remaining; ++i)
    {
        const uint32_t y = draw_y(r);
        ring[i % RingSize] = y;
        if ((i & 0xffffff) == 0xffffff)
        {
            if (progress)
                progress(i + 1);
            if (keepGoing && !keepGoing())
            {
                stopped = true;
                ++i;
                break;
            }
        }
        if (!filter[y >> 8])
            continue;
        auto range = byKey.equal_range(y);
        for (auto it = range.first; it != range.second; ++it)
        {
            auto &f = *it->second;
            const auto &a = f.anchors;
            if (f.found || i < a[2].first)
                continue;
            const uint64_t offset = i - a[2].first;
            if (ring[(offset + a[0].first) % RingSize] != a[0].second
                || ring[(offset + a[1].first) % RingSize] != a[1].second)
                continue;
            TRandom3 ahead(r);
            uint64_t at = i;
            bool all = true;
            for (size_t k = KeyAnchors; k < a.size() && all; ++k)
            {
                uint32_t yk = 0;
                while (at < offset + a[k].first)
                {
                    yk = draw_y(ahead);
                    ++at;
                }
                all = yk == a[k].second;
            }
            if (all)
            {
                f.found = true;
                f.offset = offset;
                --remaining;
            }
        }
    }
    for (auto f: files)
        if (!f->found)
            f->problem = stopped
                ? "the search was stopped after " + with_separators(i) + " draws"
                : "not found in the first " + with_separators(i) + " draws of TRandom3(4357): the file was not "
                  "made by mvme_root_client or FLIRT, or its mvme_root_client had dithered more values than that "
                  "before this run";
}

json official_raw_json(const OfficialRaw &raw)
{
    json trees = json::array();
    for (const auto &t: raw.trees)
        trees.push_back({ {"name", t.name}, {"entries", t.entries}, {"branches", t.branches} });
    json anchors = json::array();
    for (const auto &[position, y]: raw.anchors)
        anchors.push_back({ position, y });
    return { {"path", raw.path}, {"problem", raw.problem}, {"size", raw.size}, {"mtimeNs", raw.mtimeNs},
             {"runInfo", raw.runInfo}, {"runId", raw.runId}, {"trees", trees}, {"heldTwice", raw.heldTwice},
             {"noHits", raw.noHits}, {"anchors", anchors}, {"found", raw.found}, {"offset", raw.offset} };
}

OfficialRaw official_raw_from_json(const json &j)
{
    OfficialRaw raw;
    raw.path = j.at("path").get<std::string>();
    raw.problem = j.at("problem").get<std::string>();
    raw.size = j.at("size").get<int64_t>();
    raw.mtimeNs = j.at("mtimeNs").get<int64_t>();
    raw.runInfo = j.at("runInfo").get<bool>();
    raw.runId = j.at("runId").get<std::string>();
    for (const auto &t: j.at("trees"))
        raw.trees.push_back({ t.at("name").get<std::string>(), t.at("entries").get<int64_t>(),
                              t.at("branches").get<std::vector<std::string>>() });
    raw.heldTwice = j.at("heldTwice").get<bool>();
    raw.noHits = j.at("noHits").get<bool>();
    for (const auto &a: j.at("anchors"))
        raw.anchors.emplace_back(a.at(0).get<uint32_t>(), a.at(1).get<uint32_t>());
    raw.found = j.at("found").get<bool>();
    raw.offset = j.at("offset").get<uint64_t>();
    return raw;
}

namespace
{

void find_usage(const char *argv0)
{
    std::cerr << "usage: " << argv0 << " --find-dither-offset <official _raw.root>...\n"
              << "\n"
              << "Prints the dither offset of each _raw.root that mvme_root_client wrote, one\n"
              << "line per file, and converts nothing. Convert with it: --dither-offset <N>\n"
              << "(README, \"Dither offset\"). Searches the first 4,294,967,296 draws.\n"
              << "--json-events: events on stdout instead, for programs (README, \"Events for programs\").\n"
              << "Exit codes: 0 every offset found, 1 not every one, 2 usage.\n";
}

bool is_listfile_input(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        return true;
    return path.size() >= 4 && path.compare(path.size() - 4, 4, ".zip") == 0;
}

}

int run_find_dither_offset(int argc, char *argv[])
{
    // --json-events: events on stdout (README "Events for programs"), on a
    // copy of stdout while fd 1 points at stderr (stray library output cannot
    // corrupt them).
    bool events = false;
    for (int i = 1; i < argc; ++i)
        events = events || std::string(argv[i]) == "--json-events";
    FILE *out = stdout;
    if (events)
    {
        const int fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        if (fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0 || !(out = fdopen(fd, "w")))
        {
            std::perror("flirt-convert: cannot set up stdout");
            return 1;
        }
    }
    auto event = [&](const json &e) {
        const auto text = dump_valid_utf8(e) + "\n";
        std::fwrite(text.data(), 1, text.size(), out);
        std::fflush(out);
    };
    auto usage_error = [&](const std::string &error) {
        std::cerr << "error: " << error << "\n";
        if (events)
        {
            event({ {"event", "usage-error"}, {"message", error} });
            event({ {"event", "end"}, {"exitCode", 2} });
        }
        return 2;
    };

    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        std::string error;
        if (arg == "--find-dither-offset" || arg == "--json-events")
            continue;
        if (arg == "-h" || arg == "--help")
        {
            find_usage(argv[0]);
            return 0;
        }
        if (arg == "-o" || arg == "--output-dir")
            error = "--find-dither-offset writes nothing, so it takes no output directory (-o)";
        else if (arg == "--watch")
            error = "--find-dither-offset does not work with --watch";
        else if (!arg.empty() && arg[0] == '-')
            error = "--find-dither-offset takes only official _raw.root files, not " + arg;
        else if (is_listfile_input(arg))
            error = printable(arg) + " is a listfile or a folder: --find-dither-offset reads official _raw.root files";
        else if (const auto refusal = name_refusal(own_name(arg)); !refusal.empty())
            error = "--find-dither-offset " + refusal;
        if (!error.empty())
            return usage_error(error);
        paths.push_back(arg);
    }
    if (paths.empty())
    {
        if (!events)
        {
            std::cerr << "error: no files\n";
            find_usage(argv[0]);
            return 2;
        }
        return usage_error("no files");
    }

    std::vector<OfficialRaw> raws;
    for (const auto &p: paths)
        raws.push_back(read_official_raw(p));
    std::vector<OfficialRaw *> pointers;
    for (auto &r: raws)
        pointers.push_back(&r);

    const bool terminal = !events && isatty(STDERR_FILENO);
    const bool searching = std::any_of(raws.begin(), raws.end(), [](const OfficialRaw &r) { return r.problem.empty(); });
    if (terminal && searching)
        std::cerr << "searching the first " << with_separators(DitherSearchDraws) << " draws of TRandom3(4357)..." << std::flush;
    // The events' progress: at the start, then at most 4 a second.
    using Clock = std::chrono::steady_clock;
    auto lastProgress = Clock::now();
    if (events && searching)
        event({ {"event", "progress"}, {"done", 0}, {"total", DitherSearchDraws} });
    search_dither_offsets(pointers, DitherSearchDraws, [&](uint64_t done) {
        if (terminal)
            std::cerr << "\r\033[Ksearching TRandom3(4357): " << with_separators(done) << " of "
                      << with_separators(DitherSearchDraws) << " draws ("
                      << static_cast<int>(100.0 * done / DitherSearchDraws) << "%)" << std::flush;
        if (events && Clock::now() - lastProgress >= std::chrono::milliseconds(250))
        {
            lastProgress = Clock::now();
            event({ {"event", "progress"}, {"done", done}, {"total", DitherSearchDraws} });
        }
    }, {});
    if (terminal && searching)
        std::cerr << "\r\033[K" << std::flush;

    int status = 0;
    for (const auto &r: raws)
    {
        status = r.found ? status : 1;
        if (events)
        {
            event({ {"event", "dither-offset"}, {"file", r.path}, {"offset", r.found ? json(r.offset) : json()},
                    {"heldTwice", r.found && r.heldTwice}, {"problem", r.found ? json() : json(r.problem)} });
            continue;
        }
        std::string line = printable(r.path) + ": ";
        if (r.found)
        {
            line += "dither offset " + std::to_string(r.offset);
            if (r.heldTwice)
                line += " (the file holds the run twice, replayed twice into it; this is the first copy's offset)";
        }
        else
            line += "no dither offset: " + r.problem;
        std::cout << line << "\n";
    }
    if (events)
        event({ {"event", "end"}, {"exitCode", status} });
    std::cout << std::flush;
    return status;
}

bool live_run_id(const std::string &runId)
{
    if (runId.size() != 13 || runId[6] != '_')
        return false;
    for (size_t i = 0; i < runId.size(); ++i)
        if (i != 6 && !std::isdigit(static_cast<unsigned char>(runId[i])))
            return false;
    return true;
}

int run_official_raw_facts(int argc, char *argv[])
{
    if (argc != 3 && argc != 4)
        return 2;
    // The answer goes to a copy of stdout; fd 1 then points at stderr, so
    // stray library output cannot corrupt it.
    const int answerFd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (answerFd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
        return 1;
    const pid_t parent = getppid();
    auto raw = read_official_raw(argv[2]);
    // Another run's file is refused without a search.
    const bool otherRun = argc == 4 && raw.runId != argv[3] && !live_run_id(raw.runId);
    std::vector<OfficialRaw *> files{ &raw };
    if (!otherRun)
        search_dither_offsets(files, DitherSearchDraws, {}, [parent] { return getppid() == parent; });
    const auto text = dump_valid_utf8(official_raw_json(raw)) + "\n";
    return write(answerFd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) ? 0 : 1;
}

OfficialRaw official_raw_facts(const std::string &path, const std::string &runId)
{
    int fds[2];
    if (pipe_cloexec(fds) != 0)
        throw std::runtime_error(std::string("cannot create a pipe: ") + std::strerror(errno));
    std::vector<std::string> args = { self_exe(), "--official-raw-facts", path, runId };
    std::vector<char *> argv;
    for (auto &a: args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (int sig: { SIGINT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD })
        sigaddset(&defaults, sig);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid = 0;
    const int err = posix_spawn(&pid, args[0].c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(fds[1]);
    if (err)
    {
        ::close(fds[0]);
        throw std::runtime_error(std::string("cannot start the search: ") + std::strerror(err));
    }

    std::string text;
    char buf[65536];
    for (;;)
    {
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n > 0)
            text.append(buf, static_cast<size_t>(n));
        else if (n == 0 || errno != EINTR)
            break;
    }
    ::close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    { }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("cannot read " + printable(path) + ": its reader "
                                 + (WIFSIGNALED(status) ? "was killed by signal " + std::to_string(WTERMSIG(status))
                                                        : "ended with exit code " + std::to_string(WEXITSTATUS(status))));
    try
    {
        auto raw = official_raw_from_json(json::parse(text));
        raw.path = path;
        return raw;
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error("cannot read " + printable(path) + ": its reader gave no answer (" + e.what() + ")");
    }
}

}
