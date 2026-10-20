// flirt-convert: mvme MVLC listfile archives -> <runId>_raw.root and
// <runId>_histos.root as official mvme + mvme_root_client produce them, plus a
// conversion report <runId>.flirt.json. Inputs are only ever read.
//
// Without --convert-one this is the batch parent (batch.cc), which runs one
// child process per listfile. The child, `flirt-convert --convert-one
// <listfile.zip> -o <outdir> ...`, converts a single listfile; it is the code
// below.
//
// Input integrity checks run on every conversion (docs/design.md, "Input
// integrity, strict by default"). If one fails, nothing is published and the
// exit code is 1, unless --accept-damaged-input is given. SIGINT, SIGTERM or
// SIGHUP stops the conversion: nothing is published and the exit code is 130;
// once it is publishing, it finishes. Outputs are flushed to disk (fsync) and
// published with the report last (RootWriter::publish), so a report means a
// complete set. SIGHUP stays ignored if it was ignored when the process started
// (nohup).
//
// --progress-fd <N> (internal, given by the batch parent): progress lines on
// fd N, `progress <bytesProcessed> <bytesTotal>` while the listfile is
// replayed, then `finishing`.

#include <QCryptographicHash>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "common/compression.h"
#include "common/names.h"
#include "common/platform.h"
#include "common/utf8.h"
#include "flirt/analysis_config.h"
#include "flirt/default_filters.h"
#include "flirt/hit_flags.h"
#include "flirt/integrity.h"
#include "flirt/listfile.h"
#include "flirt/module_templates.h"
#include "flirt/pipeline.h"
#include "flirt/root_output.h"
#include "flirt/run_info.h"
#include "flirt/run_summary.h"
#include "flirt/sampling_config.h"
#include "flirt/settings_changes.h"
#include "flirt/threaded_sink.h"
#include "flirt/vme_config.h"
#include "flirt/waveform_output.h"
#include "flirt_version.h"
#include "batch.h"
#include "dither_offset.h"

namespace es = mvme::event_server;
namespace parser = mesytec::mvlc::readout_parser;
namespace check = flirt::checks;

namespace
{

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// Set by SIGINT, SIGTERM or SIGHUP; the conversion then stops before publishing.
std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

// SIGHUP was ignored when the process started (nohup; an ignored disposition
// survives exec), so it stays ignored.
bool hupIgnoredAtStart = false;

void on_stop_signal(int)
{
    stopRequested = true;
}

void install_stop_handler(int sig)
{
    struct sigaction sa = {};
    sa.sa_handler = on_stop_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

// ROOT has no SIGHUP handler (ESignals, TSysEvtHandler.h), so this can run
// first thing: a hangup while the RootWriter creates its private directory
// is caught too.
void install_hangup_handler()
{
    struct sigaction old = {};
    sigaction(SIGHUP, nullptr, &old);
    hupIgnoredAtStart = old.sa_handler == SIG_IGN;
    if (!hupIgnoredAtStart)
        install_stop_handler(SIGHUP);
}

// ROOT installs its own signal handlers when it initializes, so this must run
// after the RootWriter (and with it gROOT) exists. ROOT also ignores SIGINT
// while it writes to a file (seen in /proc/<pid>/status SigIgn during the
// fill), so a SIGINT sent to this process alone can be lost; the batch parent
// always follows up with SIGTERM.
void install_stop_handlers()
{
    install_stop_handler(SIGINT);
    install_stop_handler(SIGTERM);
    if (!hupIgnoredAtStart)
        install_stop_handler(SIGHUP);
}

// Progress lines for the batch parent (--progress-fd): one write(2) per line,
// nothing more after a failed write; never fails the conversion.
class ProgressWriter
{
    public:
        explicit ProgressWriter(int fd): fd_(fd) { }

        // At most every 0.5 s, the first on the first call.
        void progress(uint64_t processed, uint64_t total)
        {
            if (fd_ < 0)
                return;
            const auto now = Clock::now();
            if (written_ && now - last_ < std::chrono::milliseconds(500))
                return;
            last_ = now;
            written_ = true;
            line("progress " + std::to_string(processed) + " " + std::to_string(total) + "\n");
        }

        void finishing() { line("finishing\n"); }

    private:
        void line(const std::string &text)
        {
            if (fd_ >= 0 && write(fd_, text.data(), text.size()) != static_cast<ssize_t>(text.size()))
                fd_ = -1;
        }

        int fd_;
        bool written_ = false;
        Clock::time_point last_;
};

struct interrupted: std::runtime_error
{
    interrupted(): std::runtime_error("interrupted") { }
};

void throw_if_stop_requested()
{
    if (stopRequested)
        throw interrupted();
}

void usage(const char *argv0)
{
    std::cerr << "usage: " << argv0 << " <listfile.zip> -o <outdir>"
              << " [--analysis <file.analysis> | --default-filters]"
              << " [--experiment-name <name>] [--experiment-title <title>]"
              << " [--dither-offset <N> | --dither-offset-from <official _raw.root>] [--threads <T>] [--no-waveforms]"
              << " [--official-waveform-branches]"
              << " [--extract-hit-flags] [--compression <name>] [--accept-damaged-input] [--progress-fd <N>]\n"
              << "\n"
              << "  --analysis          use this mvme analysis instead of the one in the archive\n"
              << "  --default-filters   use the analysis mvme's \"Generate default filters\" builds\n"
              << "                      for every module (multi-event settings from the archive's)\n"
              << "  --experiment-name   mvme workspace setting Experiment/Name (default: Experiment)\n"
              << "  --experiment-title  mvme workspace setting Experiment/Title (default: empty)\n"
              << "  --dither-offset     skip N draws of TRandom3(4357) first; 0 matches a fresh\n"
              << "                      mvme_root_client --single-run (default: 0)\n"
              << "  --dither-offset-from\n"
              << "                      find the dither offset of this run's official _raw.root\n"
              << "                      and convert with it\n"
              << "  --threads           threads for ROOT compression and waveform decoding;\n"
              << "                      with 1, 2, 4 and 8 compare finds the output IDENTICAL\n"
              << "                      (tested; the bytes on disk can differ)\n"
              << "                      (default: 1)\n"
              << "  --no-waveforms      do not write <runId>_waveforms.root (all MDPP sample\n"
              << "                      traces), which is written when a module records samples\n"
              << "  --official-waveform-branches\n"
              << "                      write the analysis's waveform sources into _raw.root and\n"
              << "                      _histos.root as mvme does (channel 0, 8 bits)\n"
              << "  --extract-hit-flags add a flag column for each MDPP amplitude (0 clean,\n"
              << "                      1 over/underflow, 2 pile-up, 3 both; QDC: 1 overflow) and\n"
              << "                      write <runId>_hit_flags.analysis, with which official mvme\n"
              << "                      makes the identical files; every value's random part then\n"
              << "                      differs from files made without this option\n"
              << "  --compression       the ROOT files' compression: " << flirt::compression_names() << "\n"
              << "                      (default: " << flirt::default_compression().name << ", the only one\n"
              << "                      whose files are identical to official ones)\n"
              << "  --accept-damaged-input\n"
              << "                      convert and publish even if integrity checks fail; the\n"
              << "                      report lists the failed checks\n"
              << "  --progress-fd <N>   (internal, used by the batch) write progress lines to fd N\n";
}

// Read with the path's own bytes: QString::fromStdString would turn a name
// that is not valid UTF-8 into another name (docs/design.md, "Text that is
// not valid UTF-8").
std::string read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    if (!in)
        throw std::runtime_error("cannot read " + path);
    text << in.rdbuf();
    return text.str();
}

std::string sha256_hex(const std::string &data)
{
    return QCryptographicHash::hash(QByteArray::fromStdString(data), QCryptographicHash::Sha256)
        .toHex().toStdString();
}

std::string real_path(const std::string &path)
{
    char buf[PATH_MAX];
    if (!realpath(path.c_str(), buf))
        throw std::runtime_error("cannot resolve " + path);
    return buf;
}

// The mvme-like prefix this executable was built into: <prefix>/bin/flirt-convert.
std::string own_prefix()
{
    auto exe = real_path(flirt::self_exe());
    auto bin = exe.substr(0, exe.rfind('/'));
    return bin.substr(0, bin.rfind('/'));
}

// The sha256 of a file, computed in a background thread alongside the
// conversion, so it adds no wall time.
class BackgroundSha256
{
    public:
        explicit BackgroundSha256(const std::string &path)
            : path_(path), thread_([this] { run(); })
        { }

        ~BackgroundSha256()
        {
            cancel_ = true;
            if (thread_.joinable())
                thread_.join();
        }

        // Waits for the hash; throws if the file could not be read.
        std::string result()
        {
            if (thread_.joinable())
                thread_.join();
            if (!error_.empty())
                throw std::runtime_error(error_);
            return hex_;
        }

        // Time the hashing took; valid after result().
        double seconds() const { return seconds_; }

    private:
        void run()
        {
            const auto t0 = Clock::now();
            // The path's own bytes, as read_file().
            const int fd = open(path_.c_str(), O_RDONLY | O_CLOEXEC);

            if (fd < 0)
            {
                error_ = "cannot read " + path_;
                return;
            }

            QCryptographicHash hash(QCryptographicHash::Sha256);
            std::vector<char> buffer(4u << 20);

            while (!cancel_)
            {
                auto bytes = read(fd, buffer.data(), buffer.size());
                if (bytes < 0 && errno == EINTR)
                    continue;
                if (bytes < 0)
                {
                    error_ = "error reading " + path_;
                    close(fd);
                    return;
                }
                if (bytes == 0)
                    break;
                hash.addData(buffer.data(), static_cast<int>(bytes));
            }
            close(fd);

            hex_ = hash.result().toHex().toStdString();
            seconds_ = seconds_since(t0);
        }

        std::string path_;
        std::string hex_, error_;
        double seconds_ = 0;
        std::atomic<bool> cancel_{false};
        std::thread thread_; // last: starts once the members above exist
};

// Results of the integrity checks, in the order they were evaluated.
// "unavailable" counts as a failure; "warn" does not.
class Checks
{
    public:
        void set(const std::string &name, const std::string &result, const std::string &detail)
        {
            entries_.push_back({ name, result, detail });
        }

        bool failed() const
        {
            return std::any_of(entries_.begin(), entries_.end(),
                               [](const Entry &e) { return is_failure(e.result); });
        }

        void printFailures(std::ostream &out) const
        {
            for (const auto &e: entries_)
                if (is_failure(e.result))
                    out << "  " << e.name << ": " << e.result << ": " << e.detail << "\n";
        }

        es::json toJson() const
        {
            es::json result = es::json::object();
            for (const auto &e: entries_)
                result[e.name] = { {"result", e.result}, {"detail", e.detail} };
            return result;
        }

    private:
        struct Entry { std::string name, result, detail; };

        static bool is_failure(const std::string &result)
        {
            return result == "fail" || result == "unavailable";
        }

        std::vector<Entry> entries_;
};

// Nothing has been published: say why and return the exit code.
int refuse_to_publish(const Checks &checks, bool acceptable)
{
    std::cerr << "failed checks:\n";
    checks.printFailures(std::cerr);
    std::cerr << "nothing was published";
    if (acceptable)
        std::cerr << "; --accept-damaged-input converts and publishes anyway";
    std::cerr << "\n";
    return 1;
}

// I4: the parser threw or returned anything but Ok. A warning, never a failure:
// once I2 has verified the bytes, these describe the recording (e.g. a
// NoStackFrameFound buffer the DAQ wrote), and mvme exports such runs
// as they are (mvlc_stream_worker.cc:904-950).
std::pair<std::string, std::string> parser_health(const flirt::PipelineResult &pipeline)
{
    const auto &pc = pipeline.parserCounters;
    const auto okIndex = static_cast<size_t>(parser::ParseResult::Ok);
    const size_t exceptions = std::max<size_t>(pipeline.parserExceptions, pc.parserExceptions);
    std::string errors;

    for (size_t i = 0; i < pc.parseResults.size(); ++i)
    {
        if (i == okIndex || !pc.parseResults[i])
            continue;
        errors += (errors.empty() ? "" : ", ")
            + std::string(parser::get_parse_result_name(static_cast<parser::ParseResult>(i)))
            + " " + std::to_string(pc.parseResults[i]);
    }

    if (!exceptions && errors.empty() && !pipeline.buffersNotParsedOk)
        return { "pass", std::to_string(pc.parseResults[okIndex]) + " parse results, all Ok;"
                         " no parser exceptions" };

    std::string detail = std::to_string(exceptions) + " parser exceptions, "
        + std::to_string(pipeline.buffersNotParsedOk) + " buffers not parsed Ok";
    if (!errors.empty())
        detail += ", parse results: " + errors;
    return { "warn", detail };
}

// Counters that describe the recording rather than the file; mvme exports such
// runs as they are, so they are warnings, never failures. Empty if all zero.
std::string data_quality(const flirt::PipelineResult &pipeline)
{
    const auto &pc = pipeline.parserCounters;
    const auto &sc = pipeline.splitterCounters;
    std::string result;

    auto add = [&](uint64_t count, const char *what)
    {
        if (count)
            result += (result.empty() ? "" : ", ") + std::to_string(count) + " " + what;
    };

    auto sum = [](const std::vector<std::vector<size_t>> &perEventModule)
    {
        uint64_t total = 0;
        for (const auto &modules: perEventModule)
            for (auto count: modules)
                total += count;
        return total;
    };

    add(pipeline.buffersWithErrors, "buffers with errors");
    add(pc.unusedBytes, "unused bytes");
    add(pc.emptyStackFrames, "empty stack frames");
    add(pc.ethPacketLoss, "lost ETH packets");
    add(pc.internalBufferLoss, "lost readout buffers");
    add(sum(sc.moduleHeaderMismatches), "module header mismatches (multi-event splitting)");
    add(sum(sc.moduleEventSizeExceedsBuffer), "module event sizes exceeding the data");
    add(pipeline.waveformDecoderMessages, "waveform decoder messages");
    // mesytec-mvlc's messages are not printed any more (library_log.h); the
    // report keeps the first 100.
    const auto &pw = pipeline.parserWarnings;
    add(pw.count, pw.count == 1 ? "readout parser warning" : "readout parser warnings");
    if (pw.count)
        result += pw.count > pw.messages.size() ? "; the report lists the first " + std::to_string(pw.messages.size())
                                                : std::string("; the report lists them");
    return result;
}

// A value that may be unknown, as JSON: null if unknown.
template <typename T>
es::json or_null(const std::optional<T> &value)
{
    return value ? es::json(*value) : es::json(nullptr);
}

// The waveform config check (sampling_config.h), warnings only: per module the
// report entry, and the warning and note lines for stderr.
struct WaveformConfigCheck
{
    std::string result, detail;
    es::json modules = es::json::array();
    std::vector<std::string> messages;
};

WaveformConfigCheck waveform_config_check(const std::vector<flirt::SamplingSetting> &settings,
                                          const flirt::VMEConfig &vmeConfig,
                                          const flirt::AnalysisConfig &analysis,
                                          const flirt::WaveformWriter *waveforms,
                                          const std::map<std::pair<int, int>, uint64_t> &otherTraces,
                                          bool waveformsRequested)
{
    WaveformConfigCheck check;
    std::string warnings;
    size_t passes = 0, notes = 0;

    for (const auto &s: settings)
    {
        const auto *src = flirt::mdpp_sample_decoder(analysis, s.eventIndex, s.moduleIndex);
        flirt::ModuleWaveforms mw;
        mw.analysisSource = src ? src->name.toStdString() : std::string();
        mw.tree = flirt::is_waveform_module(analysis, s.eventIndex, s.moduleIndex);
        if (!mw.tree)
            mw.whyNoTree = flirt::why_no_waveforms(vmeConfig, analysis, s);

        // A tree module's traces, else those an analysis decoder decoded.
        std::optional<flirt::ModuleTraceCounts> counts;
        for (size_t i = 0; waveforms && i < waveforms->trees().size(); ++i)
        {
            const auto &spec = waveforms->trees()[i];
            if (spec.eventIndex == s.eventIndex && spec.moduleIndex == s.moduleIndex)
                counts = flirt::ModuleTraceCounts{ waveforms->stats()[i].traces,
                                                   waveforms->stats()[i].tracesPerLength };
        }
        if (!counts && src && waveformsRequested)
        {
            const auto it = otherTraces.find({ s.eventIndex, s.moduleIndex });
            counts = flirt::ModuleTraceCounts{ it == otherTraces.end() ? 0 : it->second, {} };
        }

        const auto r = flirt::compare_waveform_config(s, mw, counts ? &*counts : nullptr, waveformsRequested);

        check.modules.push_back({ {"event", s.eventIndex}, {"module", s.module}, {"moduleType", s.moduleType},
                                  {"sampling", or_null(s.sampling)}, {"samplingFrom", s.samplingFrom},
                                  {"numberSamples", or_null(s.numberSamples)},
                                  {"numberSamplesFrom", s.numberSamplesFrom},
                                  {"decoderSource", src ? es::json(mw.analysisSource) : es::json(nullptr)},
                                  {"traces", or_null(r.traces)},
                                  {"tracesWithConfiguredLength", or_null(r.tracesWithConfiguredLength)},
                                  {"result", r.result}, {"message", r.message} });

        if (r.result == "warn")
        {
            check.messages.push_back("warning: waveforms: " + s.module + ": " + r.message);
            warnings += (warnings.empty() ? "" : "; ") + s.module + ": " + r.message;
        }
        else if (r.result == "note")
        {
            check.messages.push_back("note: waveforms: " + s.module + ": " + r.message);
            ++notes;
        }
        else if (r.result == "pass")
            ++passes;
    }

    if (!waveformsRequested)
        check.result = "skipped", check.detail = "--no-waveforms";
    else if (!warnings.empty())
        check.result = "warn", check.detail = warnings;
    else
        check.result = "pass", check.detail = std::to_string(settings.size()) + " MDPP module(s): "
            + std::to_string(passes) + " pass, " + std::to_string(notes) + " note(s)";
    return check;
}

// Producer thread, before the batch goes to the ROOT writer: numbers each
// EventData message with its entry in its VME event's tree in _raw.root
// (mvme_root_client fills once per EventData message,
// mvme_root_client.cc:1060), moves its tree modules' traces to the waveform
// writer (if any) and counts the traces of modules without a tree.
void take_traces(flirt::MessageBatch &batch, std::vector<int64_t> &entries,
                 flirt::WaveformBatch &out, flirt::WaveformWriter *writer,
                 std::map<std::pair<int, int>, uint64_t> &otherTraces)
{
    for (size_t i = 0; i < batch.size; ++i)
    {
        const auto &msg = batch.messages[i];
        if (msg.type != es::MessageType::EventData || msg.contents.empty())
            continue;

        const size_t eventIndex = msg.contents[0];
        if (eventIndex >= entries.size())
            throw std::runtime_error("EventData message for event " + std::to_string(eventIndex)
                                     + " out of range");
        const int64_t entry = entries[eventIndex]++;

        for (auto &mt: batch.traces[i])
        {
            if (!mt.inFile || !writer)
            {
                otherTraces[{ static_cast<int>(eventIndex), mt.moduleIndex }] += mt.decoded.traces.size();
                continue;
            }
            out.events.emplace_back();
            auto &ev = out.events.back();
            ev.eventIndex = static_cast<int>(eventIndex);
            ev.moduleIndex = mt.moduleIndex;
            ev.entry = entry;
            ev.decoded = std::move(mt.decoded);
        }
        batch.traces[i].clear();
    }

    if (writer && !out.events.empty())
        writer->push(out);
}

bool same_file(const std::string &path, const flirt::FileIdentity &identity)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && st.st_dev == identity.dev && st.st_ino == identity.ino;
}

bool still_unchanged(const std::string &path, const flirt::FileIdentity &before)
{
    try
    {
        return flirt::unchanged(before, flirt::file_identity(path));
    }
    catch (const std::exception &)
    {
        return false; // gone or unreadable
    }
}

// One listfile: the child of a batch (--convert-one). argv[0] is the program.
// Why the official file of --dither-offset-from is not this run's: its event
// trees, their modules and entries against FLIRT's output; empty if it is.
// A file holding the run twice (official entries twice FLIRT's, the halves
// alike) passes with a note: its offset is the first copy's.
std::string not_same_run(const flirt::OfficialRaw &official, const flirt::RootOutputResult &output,
                         std::vector<std::string> &notes)
{
    if (official.trees.size() != output.eventTrees.size())
        return "it has " + std::to_string(official.trees.size()) + " event trees, this run "
            + std::to_string(output.eventTrees.size());
    for (size_t i = 0; i < output.eventTrees.size(); ++i)
    {
        const auto &name = output.eventTrees[i].first;
        const auto &branches = output.eventTrees[i].second;
        const auto tree = std::find_if(official.trees.begin(), official.trees.end(),
                                       [&](const flirt::OfficialRaw::Tree &t) { return t.name == name; });
        if (tree == official.trees.end())
            return "it has no event tree " + name;
        auto a = tree->branches, b = branches;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (a != b)
        {
            auto list = [](const std::vector<std::string> &v) {
                std::string s;
                for (const auto &x: v)
                    s += (s.empty() ? "" : ", ") + x;
                return s;
            };
            return "its tree " + name + " holds the modules " + list(a) + ", this run " + list(b);
        }
        const auto entries = static_cast<int64_t>(output.eventHits.at(i));
        if (tree->entries == 2 * entries && official.heldTwice && entries > 0)
            notes.push_back("it holds the run twice (replayed twice into it, " + std::to_string(tree->entries)
                            + " entries for " + std::to_string(entries) + " events): the first copy's offset is used");
        else if (tree->entries != entries)
            return "its tree " + name + " has " + std::to_string(tree->entries) + " entries, this run "
                + std::to_string(entries);
    }
    return {};
}

int convert_one(int argc, char *argv[])
{
    install_hangup_handler();

    std::string input, outDir, analysisFile, ditherOffsetFrom;
    bool ditherOffsetGiven = false;
    bool acceptDamagedInput = false;
    bool noWaveforms = false;
    bool defaultFilters = false;
    bool officialWaveformBranches = false;
    bool extractHitFlags = false;
    const flirt::Compression *compression = &flirt::default_compression();
    bool published = false;
    unsigned threads = 1;
    int progressFd = -1;
    flirt::RunInfo runInfo;
    flirt::RootOutputOptions rootOpts;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
            return argv[++i];
        };

        if (arg == "-o" || arg == "--output-dir") outDir = value();
        else if (arg == "--analysis") analysisFile = value();
        else if (arg == "--experiment-name") runInfo.experimentName = value();
        else if (arg == "--experiment-title") runInfo.experimentTitle = value();
        else if (arg == "--dither-offset") { rootOpts.ditherOffset = std::stoull(value()); ditherOffsetGiven = true; }
        else if (arg == "--dither-offset-from") ditherOffsetFrom = value();
        else if (arg == "--threads")
        {
            const auto text = value();
            if (text.empty() || text.size() > 4 || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })
                || std::stoul(text) < 1)
            {
                usage(argv[0]);
                return 2;
            }
            threads = static_cast<unsigned>(std::stoul(text));
        }
        else if (arg == "--progress-fd")
        {
            const auto text = value();
            if (text.empty() || text.size() > 6 || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }))
            {
                usage(argv[0]);
                return 2;
            }
            progressFd = std::stoi(text);
        }
        else if (arg == "--accept-damaged-input") acceptDamagedInput = true;
        else if (arg == "--no-waveforms") noWaveforms = true;
        else if (arg == "--default-filters") defaultFilters = true;
        else if (arg == "--official-waveform-branches") officialWaveformBranches = true;
        else if (arg == "--extract-hit-flags") extractHitFlags = true;
        else if (arg == "--compression")
        {
            compression = flirt::find_compression(value());
            if (!compression)
            {
                usage(argv[0]);
                return 2;
            }
        }
        else if (arg == "-h" || arg == "--help") { usage(argv[0]); return 0; }
        else if (!arg.empty() && arg[0] == '-') { usage(argv[0]); return 2; }
        else if (input.empty()) input = arg;
        else { usage(argv[0]); return 2; }
    }

    if (input.empty() || outDir.empty() || runInfo.experimentName.empty())
    {
        usage(argv[0]);
        return 2;
    }

    if (defaultFilters && !analysisFile.empty())
    {
        std::cerr << "error: --default-filters and --analysis exclude each other\n";
        usage(argv[0]);
        return 2;
    }

    if (ditherOffsetGiven && !ditherOffsetFrom.empty())
    {
        std::cerr << "error: --dither-offset and --dither-offset-from exclude each other\n";
        usage(argv[0]);
        return 2;
    }

    // Checked before any file is opened: a closed fd would be reused by a later
    // open, possibly an output file, and the progress lines written into it.
    if (progressFd >= 0)
    {
        const int flags = fcntl(progressFd, F_GETFL);
        if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY)
        {
            std::cerr << "error: --progress-fd " << progressFd << " is not open for writing\n";
            return 2;
        }
    }

    try
    {
        const auto t0 = Clock::now();

        struct stat st;
        if (stat(outDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            throw std::runtime_error("output directory does not exist: " + outDir);
        outDir = real_path(outDir);

        // I5: identities of the input files before anything reads them.
        const auto inputIdentity = flirt::file_identity(input);
        flirt::FileIdentity analysisIdentity;
        if (!analysisFile.empty())
            analysisIdentity = flirt::file_identity(analysisFile);

        // Absolute: the writer changes the working directory while it runs.
        BackgroundSha256 inputSha256(flirt::absolute_path(input));
        Checks checks;

        // mvme reads module templates from <prefix>/templates (template_system.cc:201-205).
        const auto templatesDir = own_prefix() + "/templates";
        const auto templates = flirt::read_module_templates(QString::fromStdString(templatesDir));
        if (templates.empty())
            throw std::runtime_error("no mvme module templates found in " + templatesDir);

        // I1, and I2 as far as the preamble is concerned.
        flirt::ListfileInfo listfile;
        try
        {
            listfile = flirt::read_listfile_info(input);
        }
        catch (const flirt::integrity_failure &e)
        {
            checks.set(e.check, e.result, e.what());
            return refuse_to_publish(checks, false);
        }

        checks.set(check::Archive, "pass", std::to_string(listfile.archiveEntries)
                   + " entries in the central directory");

        auto vmeConfig = flirt::parse_vme_config(listfile.vmeConfigJson, templates);
        flirt::refuse_several_events(vmeConfig);

        // I3: analysis.analysis is always checked, but only fails the
        // conversion if it is the analysis in use.
        const auto &embedded = listfile.analysisEntry;
        std::string analysisText;
        flirt::DefaultFilters defaultFiltersAnalysis;

        if (!analysisFile.empty())
        {
            analysisText = read_file(analysisFile);

            if (embedded.entry.empty())
                checks.set(check::AnalysisEntry, "pass", "archive has no analysis.analysis; --analysis in use");
            else
                checks.set(check::AnalysisEntry, embedded.result() == "pass" ? "pass" : "warn",
                           embedded.detail() + " (not in use: --analysis)");
        }
        else
        {
            // --default-filters takes the embedded analysis's multi-event
            // settings, so it is in use then too.
            if (embedded.entry.empty())
                throw flirt::unsupported_input(defaultFilters
                    ? "archive contains no analysis.analysis, whose multi-event settings --default-filters needs"
                    : "archive contains no analysis.analysis; use --analysis");

            checks.set(check::AnalysisEntry, embedded.result(), embedded.detail()
                       + (defaultFilters ? " (in use: --default-filters takes its multi-event settings)" : ""));

            if (checks.failed() && !acceptDamagedInput)
                return refuse_to_publish(checks, true);

            if (defaultFilters)
            {
                defaultFiltersAnalysis = flirt::make_default_filters(vmeConfig, templates, listfile.analysisJson);
                analysisText = defaultFiltersAnalysis.analysisJson;
            }
            else
                analysisText = listfile.analysisJson;
        }

        // --extract-hit-flags: a flag source for each amplitude, added to
        // whichever analysis is in use before it is parsed (hit_flags.h).
        // analysisText stays the input, whose sha256 the report records.
        flirt::HitFlagsEdit hitFlagsEdit;
        if (extractHitFlags)
            hitFlagsEdit = flirt::add_hit_flag_sources(analysisText, vmeConfig);

        auto analysis = flirt::parse_analysis(extractHitFlags ? hitFlagsEdit.analysisJson : analysisText, vmeConfig);
        const auto samplingSettings = flirt::sampling_settings(vmeConfig, analysis);

        // The modules with a tree in the waveforms file; they do not depend on
        // the analysis's decoders (docs/design.md, "Waveforms with any analysis").
        if (!noWaveforms)
            analysis.waveformModules = flirt::waveform_tree_modules(vmeConfig, analysis, samplingSettings);
        const bool hasWaveformSources = flirt::has_waveform_sources(analysis);

        // Waveform sources are left out of the ROOT output unless
        // --official-waveform-branches (docs/design.md, "Waveform sources left out").
        es::json omitted = es::json::array();
        for (const auto &sources: analysis.dataSources)
        {
            rootOpts.omitDataSources.emplace_back();
            for (size_t dsIndex = 0; !officialWaveformBranches && dsIndex < sources.size(); ++dsIndex)
            {
                const auto &src = sources[dsIndex];
                if (src.kind != flirt::DataSourceConfig::Kind::MdppSampleDecoder)
                    continue;
                rootOpts.omitDataSources.back().push_back(dsIndex);
                omitted.push_back({ {"class", src.className.toStdString()}, {"name", src.name.toStdString()},
                                    {"eventIndex", src.eventIndex}, {"dataSourceIndex", dsIndex},
                                    {"moduleIndex", src.moduleIndex} });
            }
        }

        // All MDPP sample traces go to <runId>_waveforms.root, one tree per
        // module of analysis.waveformModules (waveform_output.h). Traces are
        // also collected for the analysis decoders' modules without a tree,
        // for the waveform config check.
        const bool writeWaveforms = !noWaveforms && flirt::has_waveform_modules(analysis);
        const bool collectTraces = !noWaveforms && (writeWaveforms || hasWaveformSources);
        std::vector<flirt::WaveformTreeSpec> waveformTrees;
        if (writeWaveforms)
            waveformTrees = flirt::waveform_trees(vmeConfig, analysis);
        const std::string waveformsName = listfile.runId + "_waveforms.root";
        const std::string hitFlagsName = listfile.runId + "_hit_flags.analysis";

        // Output names as the writer derives them from the run ID (root_output.cc).
        const std::string reportPath = outDir + "/" + listfile.runId + ".flirt.json";
        std::vector<std::string> outputs = { outDir + "/" + listfile.runId + "_raw.root",
                                             outDir + "/" + listfile.runId + "_histos.root", reportPath };
        if (writeWaveforms)
            outputs.push_back(outDir + "/" + waveformsName);
        if (extractHitFlags)
            outputs.push_back(outDir + "/" + hitFlagsName);

        // I6: no output path may resolve to an input file (same device and inode).
        {
            std::string clash;
            for (const auto &out: outputs)
            {
                if (same_file(out, inputIdentity))
                    clash = out + " is the input listfile";
                else if (!analysisFile.empty() && same_file(out, analysisIdentity))
                    clash = out + " is the input analysis";
            }

            if (!clash.empty())
            {
                checks.set(check::OutputsNotInputs, "fail", clash);
                return refuse_to_publish(checks, false);
            }
            checks.set(check::OutputsNotInputs, "pass", "no output path resolves to an input file");
        }

        // The report is published like the ROOT files: never over an existing file.
        if (access(reportPath.c_str(), F_OK) == 0)
            throw std::runtime_error("output exists, refusing to overwrite: " + reportPath);

        // --dither-offset-from: the offset of this run's official _raw.root,
        // found by a helper process before anything is written (dither_offset.h).
        flirt::OfficialRaw official;
        std::vector<std::string> officialNotes;
        if (!ditherOffsetFrom.empty())
        {
            official = flirt::official_raw_facts(ditherOffsetFrom, listfile.runId);
            if (official.runInfo && official.runId != listfile.runId)
            {
                if (!flirt::live_run_id(official.runId))
                    throw flirt::unsupported_input("--dither-offset-from: " + ditherOffsetFrom + " is the official file of run "
                                                   + official.runId + ", not of " + listfile.runId);
                officialNotes.push_back("its RunID " + official.runId + " is the start time of a run mvme recorded live, "
                                        "so it is checked by its entries and modules only");
            }
            if (!official.problem.empty() && !official.noHits)
                throw flirt::unsupported_input("--dither-offset-from: no dither offset can be found in "
                                               + ditherOffsetFrom + ": " + official.problem);
            if (official.noHits)
                officialNotes.push_back("it has no hits, so every dither offset gives the same file: 0 is used");
            rootOpts.ditherOffset = official.found ? official.offset : 0;
        }

        runInfo.runId = listfile.runId;
        runInfo.replaySourceFile = input.substr(input.rfind('/') + 1);
        rootOpts.outDir = outDir;
        rootOpts.runId = listfile.runId;
        rootOpts.mvmePrefix = own_prefix();
        rootOpts.implicitMTThreads = threads;
        rootOpts.compression = compression->setting;
        // The waveform writer writes its own file in its own thread.
        rootOpts.threadSafety = writeWaveforms;

        const double openConfigSeconds = seconds_since(t0);
        const auto tWriter = Clock::now();

        flirt::RootWriter writer(rootOpts);
        install_stop_handlers();

        // ServerInfo first, as on a real event server connection.
        es::Message serverInfo;
        serverInfo.type = es::MessageType::ServerInfo;
        auto text = es::json::object().dump();
        serverInfo.contents.assign(text.begin(), text.end());
        writer.handleMessage(serverInfo);

        // Created in the writer's private directory, published with its files.
        // Destroyed before `writer`.
        std::unique_ptr<flirt::WaveformWriter> waveforms;
        if (writeWaveforms)
            waveforms = std::make_unique<flirt::WaveformWriter>(
                writer.addFile(listfile.runId, waveformsName), waveformTrees, compression->setting);

        // The edited analysis, staged and published with the other outputs.
        if (extractHitFlags)
        {
            const auto path = writer.addFile(listfile.runId, hitFlagsName);
            std::ofstream out(path, std::ios::binary);
            out << hitFlagsEdit.analysisJson;
            out.close();
            if (!out)
                throw std::runtime_error("error writing " + path);
        }
        flirt::HitFlagCounter hitFlagCounter(analysis, hitFlagsEdit.sources);
        if (extractHitFlags)
            hitFlagCounter.handleMessage(serverInfo);

        const double writerInitSeconds = seconds_since(tWriter);

        // Producer and consumer in two threads, joined by message batches. The
        // consumer is joined by finish(), or on an exception by its destructor,
        // before `writer` is used again or destroyed.
        flirt::PipelineResult pipeline;
        std::vector<int64_t> waveformEntries(vmeConfig.events.size());
        flirt::WaveformBatch waveformBatch;
        std::map<std::pair<int, int>, uint64_t> otherTraces; // (event, module) without a tree
        {
            flirt::ThreadedSink consumer([&](flirt::MessageBatch &batch)
                                         {
                                             throw_if_stop_requested();
                                             writer.handleBatch(batch);
                                         });

            // The waveform sources and FLIRT's decoders may run in parallel;
            // all other sources stay on the producer thread (pipeline.h).
            const unsigned waveformThreads = hasWaveformSources || writeWaveforms ? threads : 0;
            ProgressWriter progress(progressFd);
            pipeline = flirt::run_pipeline(listfile, vmeConfig, analysis, runInfo,
                                           [&](flirt::MessageBatch &batch)
                                           {
                                               throw_if_stop_requested();
                                               if (extractHitFlags)
                                                   for (size_t i = 0; i < batch.size; ++i)
                                                       hitFlagCounter.handleMessage(batch.messages[i]);
                                               if (collectTraces)
                                                   take_traces(batch, waveformEntries, waveformBatch,
                                                               waveforms.get(), otherTraces);
                                               consumer.push(batch);
                                           },
                                           waveformThreads, collectTraces,
                                           progressFd < 0 ? flirt::ProgressFn() :
                                           [&](uint64_t processed, uint64_t total)
                                           {
                                               progress.progress(processed, total);
                                           });
            progress.finishing();
            consumer.finish();
            if (waveforms)
                waveforms->finish();
        }
        writer.close();

        // Every EventData message is one entry in its event's tree.
        if (waveforms)
        {
            int64_t entries = 0;
            for (auto n: waveformEntries)
                entries += n;
            if (entries != static_cast<int64_t>(pipeline.messagesEmitted))
                throw std::logic_error("waveform entry numbering covers " + std::to_string(entries)
                                       + " of " + std::to_string(pipeline.messagesEmitted) + " events");
        }

        checks.set(check::ListfileEntry, pipeline.listfileEntry.result(), pipeline.listfileEntry.detail());

        const auto [parserResult, parserDetail] = parser_health(pipeline);
        checks.set(check::Parser, parserResult, parserDetail);

        const auto quality = data_quality(pipeline);
        checks.set(check::DataQuality, quality.empty() ? "pass" : "warn",
                   quality.empty() ? "no anomalies recorded" : quality);

        const auto waveformConfig = waveform_config_check(samplingSettings, vmeConfig, analysis, waveforms.get(),
                                                          otherTraces, !noWaveforms);
        checks.set(check::WaveformConfig, waveformConfig.result, waveformConfig.detail);

        // I5 once nothing reads the inputs any more, the hashing included.
        const auto tHashWait = Clock::now();
        const auto inputHash = inputSha256.result();
        const double hashWaitSeconds = seconds_since(tHashWait);
        {
            std::string changed;
            if (!still_unchanged(input, inputIdentity))
                changed = input;
            if (!analysisFile.empty() && !still_unchanged(analysisFile, analysisIdentity))
                changed += (changed.empty() ? "" : ", ") + analysisFile;

            checks.set(check::InputUnchanged, changed.empty() ? "pass" : "fail",
                       changed.empty() ? "size, modification time and inode unchanged"
                                       : "changed during the conversion: " + changed);
        }

        const bool failed = checks.failed();
        if (failed && !acceptDamagedInput)
            return refuse_to_publish(checks, true);

        throw_if_stop_requested();
        // Every output flushed to disk first; the report is made from what
        // will be published, then published last (RootWriter::publish).
        const auto output = writer.sync();
        const auto &rt = writer.timings();

        // --dither-offset-from: the official file must hold this run (its trees,
        // their modules and entries); refused before anything is published.
        if (!ditherOffsetFrom.empty())
        {
            if (const auto why = not_same_run(official, output, officialNotes); !why.empty())
                throw flirt::unsupported_input("--dither-offset-from: " + ditherOffsetFrom
                                               + " is not this run's official file: " + why);
        }

        es::json report;
        // As the batch's build_identity(), which the skip rule compares.
        report["flirt"] = { {"version", FLIRT_GIT_DESCRIBE}, {"mvme", MVME_GIT_DESCRIBE},
                            {"root", flirt::root_version()}, {"platform", flirt::build_platform()},
                            {"executableSha256", flirt::executable_sha256()} };
        report["status"] = failed ? "accepted-with-failed-checks" : "ok";
        report["checks"] = checks.toJson();
        report["input"] = { {"path", real_path(input)}, {"sha256", inputHash},
                            {"listfileEntry", listfile.listfileEntry}, {"runId", listfile.runId},
                            {"size", static_cast<int64_t>(inputIdentity.size)},
                            {"mtimeNs", static_cast<int64_t>(inputIdentity.mtime.tv_sec) * 1000000000
                                        + inputIdentity.mtime.tv_nsec} };
        report["analysis"] = { {"source", !analysisFile.empty() ? real_path(analysisFile)
                                          : defaultFilters ? std::string("default filters")
                                                           : std::string("archive analysis.analysis")},
                               {"sha256", analysisFile.empty() ? es::json(nullptr)
                                                               : es::json(sha256_hex(analysisText))},
                               {"version", analysis.version} };
        std::vector<std::string> skippedModules;
        if (defaultFilters)
        {
            // Object IDs are random, as in mvme, so the analysis has no stable sha256.
            es::json modules = es::json::array();
            for (const auto &m: defaultFiltersAnalysis.modules)
            {
                modules.push_back({ {"event", m.eventIndex}, {"module", m.module}, {"type", m.type},
                                    {"template", m.templateFile.empty() ? es::json(nullptr)
                                                                        : es::json(m.templateFile)} });
                if (m.templateFile.empty())
                    skippedModules.push_back(m.module + " (" + m.type + ")");
            }
            report["analysis"]["templates"] = real_path(templatesDir);
            report["analysis"]["modules"] = modules;
        }
        report["options"] = { {"experimentName", runInfo.experimentName},
                              {"experimentTitle", runInfo.experimentTitle},
                              {"ditherOffset", rootOpts.ditherOffset},
                              {"ditherOffsetFrom", ditherOffsetFrom.empty() ? es::json(nullptr)
                                                                            : es::json(real_path(ditherOffsetFrom))},
                              {"acceptDamagedInput", acceptDamagedInput},
                              {"threads", threads}, {"noWaveforms", noWaveforms},
                              {"defaultFilters", defaultFilters},
                              {"officialWaveformBranches", officialWaveformBranches},
                              {"extractHitFlags", extractHitFlags},
                              {"compression", compression->name},
                              {"compressionSetting", compression->setting} };
        report["output"] = { {"raw", output.rawFile}, {"histos", output.histosFile},
                             {"rawBytes", static_cast<int64_t>(output.rawBytes)},
                             {"histosBytes", static_cast<int64_t>(output.histosBytes)},
                             {"treeEntriesPerEvent", output.eventHits} };
        std::string waveformsFile;
        if (waveforms)
        {
            waveformsFile = output.extraFiles.at(0);
            report["output"]["waveforms"] = waveformsFile;
            report["output"]["waveformsBytes"] = static_cast<int64_t>(output.extraBytes.at(0));

            es::json trees = es::json::array();
            for (size_t i = 0; i < waveforms->trees().size(); ++i)
            {
                const auto &spec = waveforms->trees()[i];
                const auto &st = waveforms->stats()[i];
                trees.push_back({ {"tree", spec.name}, {"module", spec.module},
                                  {"moduleType", spec.moduleType},
                                  {"traces", st.traces}, {"samples", st.samples},
                                  {"tracesOtherLength", st.tracesOtherLength} });
            }
            report["waveforms"] = trees;
        }
        std::string hitFlagsFile;
        report["hitFlags"] = { {"enabled", false} };
        if (extractHitFlags)
        {
            hitFlagsFile = output.extraFiles.back();
            report["output"]["hitFlagsAnalysis"] = hitFlagsFile;
            report["output"]["hitFlagsAnalysisBytes"] = static_cast<int64_t>(output.extraBytes.back());
            report["hitFlags"] = flirt::hit_flags_json(hitFlagsEdit, hitFlagCounter, hitFlagsName);
        }
        report["waveformConfig"] = waveformConfig.modules;

        const auto &pc = pipeline.parserCounters;
        es::json parseResults = es::json::object();
        for (size_t i = 0; i < pc.parseResults.size(); ++i)
            if (pc.parseResults[i])
                parseResults[parser::get_parse_result_name(static_cast<parser::ParseResult>(i))]
                    = pc.parseResults[i];

        report["replay"] = { {"buffersRead", pipeline.buffersRead}, {"bytesRead", pipeline.bytesRead},
                             {"readoutEvents", pipeline.eventsProcessed},
                             {"eventDataMessages", pipeline.messagesEmitted},
                             {"parserExceptions", pipeline.parserExceptions},
                             {"parseResults", parseResults},
                             {"buffersNotParsedOk", pipeline.buffersNotParsedOk},
                             {"buffersWithErrors", pipeline.buffersWithErrors},
                             {"waveformDecoderMessages", pipeline.waveformDecoderMessages},
                             {"parserWarnings", pipeline.parserWarnings.count},
                             {"parserWarningMessages", pipeline.parserWarnings.messages},
                             {"unusedBytes", pc.unusedBytes}, {"emptyStackFrames", pc.emptyStackFrames},
                             {"ethPacketsProcessed", pc.ethPacketsProcessed},
                             {"ethPacketLoss", pc.ethPacketLoss}, {"internalBufferLoss", pc.internalBufferLoss} };

        // report["run"], built as flirt-info builds it (run_info.h).
        const auto runFacts = flirt::run_facts(listfile, pc, pipeline.run);
        report["run"] = flirt::run_json(runFacts, vmeConfig);

        // Module metas and, when used, the multi-event splitter's filters and counters.
        es::json modules = es::json::array();
        for (size_t ei = 0; ei < vmeConfig.events.size(); ++ei)
            for (const auto &m: vmeConfig.events[ei].modules)
            {
                es::json filters = es::json::array();
                for (const auto &f: m.eventHeaderFilters)
                    filters.push_back(f.toStdString());
                modules.push_back({ {"event", ei}, {"name", m.name.toStdString()},
                                    {"type", m.typeName.toStdString()}, {"enabled", m.enabled},
                                    {"moduleMeta", m.metaSource.toStdString()},
                                    {"eventHeaderFilters", filters} });
            }
        report["modules"] = modules;

        const auto &sc = pipeline.splitterCounters;
        report["multiEventSplitting"] = { {"enabledForEvent", analysis.multiEventProcessing} };
        if (pipeline.multiEventSplitting)
            report["multiEventSplitting"]["counters"] = {
                {"inputEvents", sc.inputEvents}, {"outputEvents", sc.outputEvents},
                {"moduleHeaderMismatches", sc.moduleHeaderMismatches},
                {"moduleEventSizeExceedsBuffer", sc.moduleEventSizeExceedsBuffer} };

        // Where the dither offset came from.
        if (!ditherOffsetFrom.empty())
        {
            report["ditherOffsetFrom"] = { {"path", real_path(ditherOffsetFrom)}, {"size", official.size},
                                           {"mtimeNs", official.mtimeNs}, {"runId", official.runId},
                                           {"entries", official.trees.empty() ? 0 : official.trees[0].entries},
                                           {"heldTwice", official.heldTwice}, {"noHits", official.noHits},
                                           {"ditherOffset", rootOpts.ditherOffset}, {"notes", officialNotes} };
        }

        report["omittedSources"] = omitted;
        const bool matchesOfficialLayout = omitted.empty();
        report["matchesOfficialLayout"] = matchesOfficialLayout;
        // Where the waveforms are, for the note and the report's note.
        const std::string waveformsAre = !waveforms
            ? (noWaveforms ? "no waveforms file (--no-waveforms)"
                           : "no module records samples FLIRT can decode, so there is no waveforms file")
            : "all waveforms are in " + waveformsFile;
        if (!matchesOfficialLayout)
            report["note"] = "The analysis's " + std::to_string(omitted.size()) + " waveform (MDPP sample "
                             "decoder) source(s) are not written to _raw.root and _histos.root (official "
                             "mvme writes only their channel 0, cut to 8 bits). Their values still took "
                             "their random draws, so every other value is stored exactly as "
                             "mvme_root_client stores it, but the files lack the waveform branches, "
                             "class members and histograms, and ROOT's basket layout differs as a "
                             "result; " + waveformsAre + "; --official-waveform-branches writes them "
                             "as mvme does.";

        // Seconds per stage. Waveform decoding is part of replayParseExtract;
        // inputHashing runs alongside the other stages.
        const double totalSeconds = seconds_since(t0);
        report["timing"] = { {"openConfig", openConfigSeconds},
                             {"rootSetup", writerInitSeconds + rt.setup},
                             {"replayParseExtract", pipeline.producerSeconds},
                             {"rootFill", rt.fill},
                             {"rootWriteClose", rt.writeClose},
                             {"histograms", rt.histograms},
                             {"publish", rt.publish},
                             {"waveformFill", waveforms ? waveforms->fillSeconds() : 0.0},
                             {"waveformWriteClose", waveforms ? waveforms->writeCloseSeconds() : 0.0},
                             {"inputHashing", inputSha256.seconds()},
                             {"inputHashingWait", hashWaitSeconds},
                             {"total", totalSeconds} };

        // The text, with invalid UTF-8 replaced (common/utf8.h), then the
        // whole set published with the report last (RootWriter::publish): a
        // report that cannot be made or written leaves nothing published.
        {
            std::string contents;
            try
            {
                contents = flirt::dump_valid_utf8(report, 2) + "\n";
            }
            catch (const std::exception &e)
            {
                throw std::runtime_error("cannot make the report " + reportPath + ": " + e.what()
                                         + "; nothing was published");
            }
            throw_if_stop_requested();
            writer.publish(listfile.runId + ".flirt.json", contents);
            published = true;
        }

        std::cerr << "wrote " << output.rawFile << ", " << output.histosFile << ", "
                  << (waveformsFile.empty() ? "" : waveformsFile + ", ")
                  << (hitFlagsFile.empty() ? "" : hitFlagsFile + ", ") << reportPath
                  << " (" << pipeline.messagesEmitted << " events, " << totalSeconds << " s)\n";
        std::cerr << flirt::run_summary_line(runFacts.times, runFacts.events) << "\n";
        if (!ditherOffsetFrom.empty())
        {
            std::cerr << "dither offset " << rootOpts.ditherOffset << ", from " << ditherOffsetFrom << "\n";
            for (const auto &note: officialNotes)
                std::cerr << "note: --dither-offset-from: " << note << "\n";
        }
        if (extractHitFlags)
            std::cerr << flirt::hit_flags_line(report["hitFlags"]) << "\n";
        if (const auto line = flirt::settings_changes_line(runFacts.settings); !line.empty())
            std::cerr << line << "\n";
        if (!skippedModules.empty())
        {
            std::string names;
            for (const auto &m: skippedModules)
                names += (names.empty() ? "" : ", ") + m;
            std::cerr << "note: default filters: no default_filters.analysis template for " << names
                      << "; skipped, as mvme does\n";
        }
        if (!quality.empty())
            std::cerr << "data quality (recorded as the DAQ wrote it, not an error): " << quality << "\n";
        // Loss details, the receive-buffer note (run_info.h) and the rate
        // warnings (run_timeline.h).
        if (const auto loss = flirt::loss_text(runFacts); !loss.empty())
            std::cerr << "lost packets: " << loss << "\n";
        if (const auto note = flirt::receive_buffer_note(runFacts); !note.empty())
            std::cerr << note << "\n";
        for (const auto &line: flirt::rate_warnings(runFacts.rate, "the report lists them all"))
            std::cerr << line << "\n";
        for (const auto &line: waveformConfig.messages)
            std::cerr << line << "\n";
        if (!matchesOfficialLayout)
            std::cerr << "note: the analysis's " << omitted.size() << " waveform source(s) are not written"
                      << " to _raw.root and _histos.root (official mvme writes only their channel 0, cut to"
                      << " 8 bits); every other value is as official; " << waveformsAre
                      << "; --official-waveform-branches writes them as mvme does\n";
        if (failed)
        {
            std::cerr << "warning: published despite failed checks (--accept-damaged-input):\n";
            checks.printFailures(std::cerr);
        }
        return 0;
    }
    catch (const flirt::unsupported_input &e)
    {
        std::cerr << "refused: " << e.what() << "\n";
        return 3;
    }
    catch (const std::exception &e)
    {
        // The RootWriter, and with it its private output directory, is gone.
        if (stopRequested && !published)
        {
            std::cerr << "interrupted: nothing was published\n";
            return 130;
        }
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

}

int main(int argc, char *argv[])
{
    if (argc > 1 && std::string(argv[1]) == "--official-raw-facts")
        return flirt::run_official_raw_facts(argc, argv);
    // A mode like --watch: wherever it stands among the arguments.
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--find-dither-offset")
            return flirt::run_find_dither_offset(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--convert-one")
    {
        std::vector<char *> args{ argv[0] };
        args.insert(args.end(), argv + 2, argv + argc);
        return convert_one(static_cast<int>(args.size()), args.data());
    }
    return flirt::run_batch(argc, argv);
}
