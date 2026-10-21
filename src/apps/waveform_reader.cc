#include "waveform_reader.h"

#include <TError.h>
#include <TFile.h>
#include <TKey.h>
#include <TLeaf.h>
#include <TList.h>
#include <TTree.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include "common/platform.h"

namespace flirt
{

using json = nlohmann::json;

namespace
{

// The branches of every waveforms tree (docs/usage.md "Waveforms").
const char *const Branches[] = { "event", "channel", "timestamp", "config", "phase", "length", "nsamples", "samples" };

std::string rootMessages;

void root_message_handler(int level, Bool_t abort, const char *location, const char *message)
{
    if (level < kWarning && !abort)
        return;
    std::string line = std::string(level >= kError ? "Error" : "Warning") + " in <" + (location ? location : "")
        + ">: " + (message ? message : "");
    if (abort)
    {
        // As ROOT's default handler: the program cannot go on.
        fprintf(stderr, "%s\n", line.c_str());
        std::abort();
    }
    rootMessages += (rootMessages.empty() ? "" : "; ") + line;
}

// A request's error: the sentence, and ROOT's messages with the /dev/fd name
// replaced by the file's.
struct Failure: std::runtime_error
{
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string &sentence)
{
    throw Failure(sentence);
}

std::string with_root_messages(const std::string &sentence, const std::string &fdName, const std::string &path)
{
    auto messages = take_root_messages();
    if (messages.empty())
        return sentence;
    for (size_t at = 0; !fdName.empty() && (at = messages.find(fdName, at)) != std::string::npos; at += path.size())
        messages.replace(at, fdName.size(), path);
    return sentence + " (ROOT: " + messages + ")";
}

// The typical y range (docs/design.md, "TUI", "Waveform view"): from the first
// 20,000 traces with samples, the 1st percentile of their minima and the 99th
// of their maxima (numpy's default, linear between the two nearest), and the
// most common trace length (the shortest of equally common ones); null when no
// trace has samples.
constexpr Long64_t TypicalTraces = 20000;

double percentile(std::vector<double> &values, double p)
{
    std::sort(values.begin(), values.end());
    const double at = p / 100 * static_cast<double>(values.size() - 1);
    const auto below = static_cast<size_t>(at);
    const size_t above = std::min(below + 1, values.size() - 1);
    return values[below] + (values[above] - values[below]) * (at - static_cast<double>(below));
}

bool has_waveform_branches(TTree *tree)
{
    for (const char *name: Branches)
        if (!tree->GetBranch(name))
            return false;
    return true;
}

// The request's file: "fileBytes" (hex) for a name that is not valid UTF-8,
// which a JSON string cannot carry, else "file".
std::string request_path(const json &request)
{
    const auto bytes = request.find("fileBytes");
    if (bytes == request.end())
        return request.value("file", "");
    const auto hex = bytes->is_string() ? bytes->get<std::string>() : "";
    std::string path;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        if (digit(hex[i]) < 0 || digit(hex[i + 1]) < 0)
            fail("fileBytes must be the path's bytes in hexadecimal");
        path += static_cast<char>(digit(hex[i]) * 16 + digit(hex[i + 1]));
    }
    if (hex.size() % 2)
        fail("fileBytes must be the path's bytes in hexadecimal");
    return path;
}

int64_t whole_number(const json &value, const char *what)
{
    if (value.is_number_unsigned())
        return value.get<uint64_t>() > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : value.get<int64_t>();
    if (!value.is_number_integer())
        fail(std::string(what) + " must be a whole number");
    return value.get<int64_t>();
}

}

std::string take_root_messages()
{
    std::string out;
    out.swap(rootMessages);
    return out;
}

void collect_root_messages()
{
    SetErrorHandler(root_message_handler);
}

WaveformFileReader::WaveformFileReader() = default;

WaveformFileReader::~WaveformFileReader()
{
    close();
}

void WaveformFileReader::close()
{
    indexes_.clear();
    file_.reset();
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
    path_.clear();
}

void WaveformFileReader::open(const std::string &path)
{
    if (path.empty() || path[0] != '/')
        fail("the file must be given by its absolute path: " + path);
    // stat first: opening a FIFO or a device could block or have effects.
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
    {
        if (errno == ENOENT || errno == ENOTDIR)
            fail("no such file: " + path);
        fail("cannot read " + path + ": " + std::strerror(errno));
    }
    if (S_ISDIR(st.st_mode))
        fail(path + " is a directory, not a waveforms file");
    if (!S_ISREG(st.st_mode))
        fail(path + " is not a regular file");
    const auto mtime = stat_mtime(st);
    const int64_t mtimeNs = static_cast<int64_t>(mtime.tv_sec) * 1000000000 + mtime.tv_nsec;
    if (file_ && path == path_ && st.st_dev == dev_ && st.st_ino == ino_ && st.st_size == size_ && mtimeNs == mtimeNs_)
        return;
    close();

    // O_NONBLOCK: if the name was replaced by a FIFO since the stat, the open
    // does not wait for a writer; fstat then refuses it.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        fail("cannot open " + path + ": " + std::strerror(errno));
    struct stat fst;
    if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode))
    {
        ::close(fd);
        fail(path + " is not a regular file");
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

    // ROOT cuts a local file name at '#' or '?' (URL anchor and options), so
    // it opens the descriptor's name instead, whatever the file is called.
    // The descriptor stays open with the TFile.
    const auto fdName = "/dev/fd/" + std::to_string(fd);
    take_root_messages();
    std::unique_ptr<TFile> file(TFile::Open(fdName.c_str(), "READ"));
    if (!file || file->IsZombie() || !file->IsOpen())
    {
        file.reset();
        ::close(fd);
        fail(with_root_messages(path + " is not a ROOT file, or it is damaged", fdName, path));
    }
    take_root_messages();
    file_ = std::move(file);
    fd_ = fd;
    path_ = path;
    dev_ = fst.st_dev;
    ino_ = fst.st_ino;
    size_ = fst.st_size;
    mtimeNs_ = mtimeNs;
}

TTree *WaveformFileReader::waveform_tree(const std::string &name)
{
    auto *tree = file_->Get<TTree>(name.c_str());
    return tree && has_waveform_branches(tree) ? tree : nullptr;
}

json WaveformFileReader::trees(const json &request)
{
    const auto path = request_path(request);
    open(path);
    json trees = json::array();
    std::set<std::string> seen;
    for (auto *object: *file_->GetListOfKeys())
    {
        auto *key = static_cast<TKey *>(object);
        const std::string name = key->GetName();
        if (!seen.insert(name).second || std::string(key->GetClassName()) != "TTree")
            continue;
        if (auto *tree = waveform_tree(name))
            trees.push_back({ {"name", name}, {"title", tree->GetTitle()}, {"traces", tree->GetEntries()},
                              {"typical", typical(tree, name)} });
    }
    if (trees.empty())
        fail(with_root_messages(path + " is not a FLIRT waveforms file (no tree with the waveform branches)",
                                "/dev/fd/" + std::to_string(fd_), path));
    return { {"id", request.value("id", json())}, {"file", request["file"]}, {"trees", trees} };
}

WaveformFileReader::json WaveformFileReader::typical(TTree *tree, const std::string &name)
{
    Int_t nsamples = 0;
    int most = 1;
    if (auto *leaf = tree->GetLeaf("nsamples"))
        most = std::max(1, leaf->GetMaximum());
    std::vector<Short_t> samples(static_cast<size_t>(most));
    auto *nsamplesBranch = tree->GetBranch("nsamples");
    auto *samplesBranch = tree->GetBranch("samples");
    nsamplesBranch->SetAddress(&nsamples);
    samplesBranch->SetAddress(samples.data());
    std::vector<double> lows, highs;
    std::map<int, int> lengths;
    const Long64_t entries = tree->GetEntries();
    for (Long64_t i = 0; i < entries && static_cast<Long64_t>(lows.size()) < TypicalTraces; ++i)
    {
        if (nsamplesBranch->GetEntry(i) <= 0)
            fail(with_root_messages("cannot read entry " + std::to_string(i) + " of tree '" + name + "' in " + path_,
                                    "/dev/fd/" + std::to_string(fd_), path_));
        if (nsamples <= 0)
            continue;
        if (nsamples > static_cast<Int_t>(samples.size()))
        {
            samples.resize(static_cast<size_t>(nsamples));
            samplesBranch->SetAddress(samples.data());
        }
        if (samplesBranch->GetEntry(i) < 0)
            fail(with_root_messages("cannot read entry " + std::to_string(i) + " of tree '" + name + "' in " + path_,
                                    "/dev/fd/" + std::to_string(fd_), path_));
        const auto range = std::minmax_element(samples.begin(), samples.begin() + nsamples);
        lows.push_back(*range.first);
        highs.push_back(*range.second);
        ++lengths[nsamples];
    }
    nsamplesBranch->ResetAddress();
    samplesBranch->ResetAddress();
    if (lows.empty())
        return nullptr;
    int common = 0, count = 0;
    for (const auto &[length, n]: lengths)
        if (n > count)
        {
            common = length;
            count = n;
        }
    const auto traces = lows.size();
    return { {"traces", traces}, {"low", percentile(lows, 1)}, {"high", percentile(highs, 99)}, {"samples", common} };
}

WaveformFileReader::TreeIndex &WaveformFileReader::index(const std::string &name)
{
    const auto found = indexes_.find(name);
    if (found != indexes_.end())
        return found->second;
    auto *tree = waveform_tree(name);
    if (!tree)
    {
        bool any = false;
        for (auto *object: *file_->GetListOfKeys())
            if (std::string(static_cast<TKey *>(object)->GetClassName()) == "TTree"
                && waveform_tree(object->GetName()))
                any = true;
        fail(any ? "no waveform tree '" + name + "' in " + path_
                 : path_ + " is not a FLIRT waveforms file (no tree with the waveform branches)");
    }

    TreeIndex t;
    t.tree = tree;
    if (auto *leaf = tree->GetLeaf("nsamples"))
        t.maxSamples = std::max(0, leaf->GetMaximum());
    // Only `event` and `channel`, branch by branch.
    Long64_t event = 0;
    Int_t channel = 0;
    auto *eventBranch = tree->GetBranch("event");
    auto *channelBranch = tree->GetBranch("channel");
    eventBranch->SetAddress(&event);
    channelBranch->SetAddress(&channel);
    const Long64_t entries = tree->GetEntries();
    if (entries > UINT32_MAX)
        fail("tree '" + name + "' has too many entries");
    for (Long64_t i = 0; i < entries; ++i)
    {
        if (eventBranch->GetEntry(i) <= 0 || channelBranch->GetEntry(i) <= 0)
            fail(with_root_messages("cannot read entry " + std::to_string(i) + " of tree '" + name + "' in " + path_,
                                    "/dev/fd/" + std::to_string(fd_), path_));
        if (t.event.empty() || t.event.back() != event)
        {
            t.first.push_back(i);
            t.count.push_back(0);
            t.event.push_back(event);
        }
        ++t.count.back();
        const auto frameIndex = static_cast<uint32_t>(t.event.size() - 1);
        auto &frames = t.channelFrames[channel];
        if (frames.empty() || frames.back() != frameIndex)
            frames.push_back(frameIndex);
    }
    eventBranch->ResetAddress();
    channelBranch->ResetAddress();
    return indexes_.emplace(name, std::move(t)).first->second;
}

json WaveformFileReader::frame(const json &request)
{
    const auto path = request_path(request);
    open(path);
    const auto treeName = request.find("tree");
    if (treeName == request.end() || !treeName->is_string())
        fail("a frame request needs the tree's name");
    // The filter: null (all), a channel, or a non-empty set of channels.
    const json channel = request.value("channel", json());
    std::vector<int> wantedChannels;
    const char *const channelError = "channel must be null, a channel number or a non-empty list of channel numbers";
    auto channel_number = [&](const json &c) {
        if (!c.is_number_integer() || c.get<int64_t>() < INT_MIN || c.get<int64_t>() > INT_MAX)
            fail(channelError);
        return c.get<int>();
    };
    if (channel.is_array())
    {
        if (channel.empty())
            fail(channelError);
        for (const auto &c: channel)
            wantedChannels.push_back(channel_number(c));
        std::sort(wantedChannels.begin(), wantedChannels.end());
        wantedChannels.erase(std::unique(wantedChannels.begin(), wantedChannels.end()), wantedChannels.end());
    }
    else if (!channel.is_null())
        wantedChannels.push_back(channel_number(channel));
    int64_t depth = 1;
    if (request.find("depth") != request.end())
    {
        const auto &d = request["depth"];
        if (!d.is_number_integer() || d.get<int64_t>() < 1 || d.get<int64_t>() > 100)
            fail("depth must be a whole number from 1 to 100");
        depth = d.get<int64_t>();
    }
    const bool byEvent = (request.find("event") != request.end());
    if (!byEvent && request.find("position") == request.end())
        fail("a frame request needs a position or an event");
    const int64_t wanted = whole_number(byEvent ? request["event"] : request["position"], byEvent ? "event" : "position");

    auto &t = index(treeName->get<std::string>());

    // The frames of this filter: all, the channel's, or the set's union
    // (computed once, the last set kept).
    const std::vector<uint32_t> *channelFrames = nullptr;
    static const std::vector<uint32_t> none;
    if (wantedChannels.size() == 1)
    {
        const auto it = t.channelFrames.find(wantedChannels[0]);
        channelFrames = it != t.channelFrames.end() ? &it->second : &none;
    }
    else if (!wantedChannels.empty())
    {
        if (t.setChannels != wantedChannels)
        {
            std::vector<uint32_t> frames;
            for (const int c: wantedChannels)
            {
                const auto it = t.channelFrames.find(c);
                if (it == t.channelFrames.end())
                    continue;
                std::vector<uint32_t> merged;
                merged.reserve(frames.size() + it->second.size());
                std::set_union(frames.begin(), frames.end(), it->second.begin(), it->second.end(),
                               std::back_inserter(merged));
                frames.swap(merged);
            }
            t.setChannels = wantedChannels;
            t.setFrames.swap(frames);
        }
        channelFrames = &t.setFrames;
    }
    const int64_t frames = channelFrames ? static_cast<int64_t>(channelFrames->size())
                                         : static_cast<int64_t>(t.event.size());
    auto frame_at = [&](int64_t k) { return channelFrames ? (*channelFrames)[static_cast<size_t>(k)] : static_cast<uint32_t>(k); };
    auto in_filter = [&](int c) {
        return wantedChannels.empty() || std::binary_search(wantedChannels.begin(), wantedChannels.end(), c);
    };

    int64_t position = -1;
    if (frames > 0)
    {
        if (byEvent)
        {
            // The first frame whose event is >= E, else the last.
            position = frames - 1;
            for (int64_t k = 0; k < frames; ++k)
                if (t.event[frame_at(k)] >= wanted)
                {
                    position = k;
                    break;
                }
        }
        else
            position = std::max<int64_t>(0, std::min(wanted, frames - 1));
    }

    json channels = json::array();
    for (const auto &c: t.channelFrames)
        channels.push_back(c.first);

    json answer = { {"id", request.value("id", json())}, {"file", request["file"]}, {"tree", treeName->get<std::string>()},
                    {"channel", channel}, {"channels", channels}, {"frames", frames}, {"position", position},
                    {"depth", 0}, {"event", nullptr}, {"timestamp", nullptr}, {"traces", json::array()} };
    if (position < 0)
        return answer;

    auto *tree = t.tree;
    Int_t entryChannel = 0, nsamples = 0;
    ULong64_t timestamp = 0;
    std::vector<Short_t> samples(static_cast<size_t>(std::max(t.maxSamples, 1)));
    auto *channelBranch = tree->GetBranch("channel");
    auto *timestampBranch = tree->GetBranch("timestamp");
    auto *nsamplesBranch = tree->GetBranch("nsamples");
    auto *samplesBranch = tree->GetBranch("samples");
    channelBranch->SetAddress(&entryChannel);
    timestampBranch->SetAddress(&timestamp);
    nsamplesBranch->SetAddress(&nsamples);
    samplesBranch->SetAddress(samples.data());
    auto readFailed = [&](Long64_t entry) {
        fail(with_root_messages("cannot read entry " + std::to_string(entry) + " of tree '" + tree->GetName()
                                + "' in " + path_, "/dev/fd/" + std::to_string(fd_), path_));
    };
    // The oldest frame first, each frame's traces in entry order, so a
    // caller draws the newest last.
    const int64_t oldest = std::max<int64_t>(0, position - depth + 1);
    json traces = json::array();
    for (int64_t k = oldest; k <= position; ++k)
    {
        const auto f = frame_at(k);
        const int64_t age = position - k;
        bool first = true;
        for (Long64_t e = t.first[f]; e < t.first[f] + t.count[f]; ++e)
        {
            // Only `channel` for the entries of other channels.
            if (channelBranch->GetEntry(e) <= 0)
                readFailed(e);
            if (!in_filter(entryChannel))
                continue;
            if (timestampBranch->GetEntry(e) <= 0 || nsamplesBranch->GetEntry(e) <= 0)
                readFailed(e);
            if (nsamples > static_cast<Int_t>(samples.size()))
            {
                samples.resize(static_cast<size_t>(nsamples));
                samplesBranch->SetAddress(samples.data());
            }
            if (samplesBranch->GetEntry(e) < 0)
                readFailed(e);
            if (first && age == 0)
                answer["timestamp"] = static_cast<uint64_t>(timestamp);
            first = false;
            const auto n = static_cast<size_t>(std::max(0, nsamples));
            traces.push_back({ {"channel", entryChannel},
                               {"samples", std::vector<int>(samples.begin(), samples.begin() + static_cast<long>(n))},
                               {"age", age} });
        }
    }
    for (auto *b: { channelBranch, timestampBranch, nsamplesBranch, samplesBranch })
        b->ResetAddress();
    answer["depth"] = position - oldest + 1;
    answer["event"] = t.event[frame_at(position)];
    answer["traces"] = std::move(traces);
    return answer;
}

json WaveformFileReader::handle(const json &request)
{
    const json id = request.is_object() ? request.value("id", json()) : json();
    try
    {
        if (!request.is_object())
            fail("a request must be a JSON object");
        const auto op = request.find("op");
        if (op == request.end() || !op->is_string())
            fail("a request needs an op: trees or frame");
        const auto file = request.find("file");
        if (file == request.end() || !file->is_string())
            fail("a request needs the file's absolute path");
        take_root_messages();
        if (*op == "trees")
            return trees(request);
        if (*op == "frame")
            return frame(request);
        fail("unknown op '" + op->get<std::string>() + "'; the ops are trees and frame");
    }
    catch (const std::exception &e)
    {
        take_root_messages();
        return { {"id", id}, {"error", e.what()} };
    }
}

}
