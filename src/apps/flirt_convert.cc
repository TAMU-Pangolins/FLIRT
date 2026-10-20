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

#include <QCryptographicHash>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "common/names.h"
#include "common/platform.h"
#include "common/utf8.h"
#include "flirt/analysis_config.h"
#include "flirt/integrity.h"
#include "flirt/listfile.h"
#include "flirt/module_templates.h"
#include "flirt/pipeline.h"
#include "flirt/root_output.h"
#include "flirt/threaded_sink.h"
#include "flirt/vme_config.h"
#include "flirt_version.h"
#include "batch.h"

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
              << " [--dither-offset <N>] [--accept-damaged-input]\n"
              << "\n"
              << "  --dither-offset     skip N draws of TRandom3(4357) first; 0 matches a fresh\n"
              << "                      mvme_root_client --single-run (default: 0)\n"
              << "  --accept-damaged-input\n"
              << "                      convert and publish even if integrity checks fail; the\n"
              << "                      report lists the failed checks\n";
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
int convert_one(int argc, char *argv[])
{
    install_hangup_handler();

    std::string input, outDir;
    bool acceptDamagedInput = false;
    bool published = false;
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
        else if (arg == "--dither-offset") rootOpts.ditherOffset = std::stoull(value());
        else if (arg == "--accept-damaged-input") acceptDamagedInput = true;
        else if (arg == "-h" || arg == "--help") { usage(argv[0]); return 0; }
        else if (!arg.empty() && arg[0] == '-') { usage(argv[0]); return 2; }
        else if (input.empty()) input = arg;
        else { usage(argv[0]); return 2; }
    }

    if (input.empty() || outDir.empty())
    {
        usage(argv[0]);
        return 2;
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

        if (embedded.entry.empty())
            throw flirt::unsupported_input("archive contains no analysis.analysis");

        checks.set(check::AnalysisEntry, embedded.result(), embedded.detail());

        if (checks.failed() && !acceptDamagedInput)
            return refuse_to_publish(checks, true);

        analysisText = listfile.analysisJson;

        auto analysis = flirt::parse_analysis(analysisText, vmeConfig);

        // Output names as the writer derives them from the run ID (root_output.cc).
        const std::string reportPath = outDir + "/" + listfile.runId + ".flirt.json";
        std::vector<std::string> outputs = { outDir + "/" + listfile.runId + "_raw.root",
                                             outDir + "/" + listfile.runId + "_histos.root", reportPath };

        // I6: no output path may resolve to an input file (same device and inode).
        {
            std::string clash;
            for (const auto &out: outputs)
            {
                if (same_file(out, inputIdentity))
                    clash = out + " is the input listfile";
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

        runInfo.runId = listfile.runId;
        runInfo.replaySourceFile = input.substr(input.rfind('/') + 1);
        rootOpts.outDir = outDir;
        rootOpts.runId = listfile.runId;
        rootOpts.mvmePrefix = own_prefix();

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

        const double writerInitSeconds = seconds_since(tWriter);

        // Producer and consumer in two threads, joined by message batches. The
        // consumer is joined by finish(), or on an exception by its destructor,
        // before `writer` is used again or destroyed.
        flirt::PipelineResult pipeline;
        {
            flirt::ThreadedSink consumer([&](flirt::MessageBatch &batch)
                                         {
                                             throw_if_stop_requested();
                                             writer.handleBatch(batch);
                                         });

            pipeline = flirt::run_pipeline(listfile, vmeConfig, analysis, runInfo,
                                           [&](flirt::MessageBatch &batch)
                                           {
                                               throw_if_stop_requested();
                                               consumer.push(batch);
                                           });
            consumer.finish();
        }
        writer.close();

        checks.set(check::ListfileEntry, pipeline.listfileEntry.result(), pipeline.listfileEntry.detail());

        const auto [parserResult, parserDetail] = parser_health(pipeline);
        checks.set(check::Parser, parserResult, parserDetail);

        // I5 once nothing reads the inputs any more, the hashing included.
        const auto tHashWait = Clock::now();
        const auto inputHash = inputSha256.result();
        const double hashWaitSeconds = seconds_since(tHashWait);
        {
            std::string changed;
            if (!still_unchanged(input, inputIdentity))
                changed = input;

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

        es::json report;
        report["flirt"] = { {"version", FLIRT_GIT_DESCRIBE}, {"mvme", MVME_GIT_DESCRIBE},
                            {"root", flirt::root_version()} };
        report["status"] = failed ? "accepted-with-failed-checks" : "ok";
        report["checks"] = checks.toJson();
        report["input"] = { {"path", real_path(input)}, {"sha256", inputHash},
                            {"listfileEntry", listfile.listfileEntry}, {"runId", listfile.runId},
                            {"size", static_cast<int64_t>(inputIdentity.size)},
                            {"mtimeNs", static_cast<int64_t>(inputIdentity.mtime.tv_sec) * 1000000000
                                        + inputIdentity.mtime.tv_nsec} };
        report["analysis"] = { {"source", std::string("archive analysis.analysis")},
                               {"sha256", es::json(nullptr)},
                               {"version", analysis.version} };
        report["options"] = { {"ditherOffset", rootOpts.ditherOffset},
                              {"acceptDamagedInput", acceptDamagedInput} };
        report["output"] = { {"raw", output.rawFile}, {"histos", output.histosFile},
                             {"rawBytes", static_cast<int64_t>(output.rawBytes)},
                             {"histosBytes", static_cast<int64_t>(output.histosBytes)},
                             {"treeEntriesPerEvent", output.eventHits} };

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

        // Seconds per stage. inputHashing runs alongside the other stages.
        const double totalSeconds = seconds_since(t0);
        report["timing"] = { {"openConfig", openConfigSeconds},
                             {"rootSetup", writerInitSeconds + rt.setup},
                             {"replayParseExtract", pipeline.producerSeconds},
                             {"rootFill", rt.fill},
                             {"rootWriteClose", rt.writeClose},
                             {"histograms", rt.histograms},
                             {"publish", rt.publish},
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

        std::cerr << "wrote " << output.rawFile << ", " << output.histosFile << ", " << reportPath
                  << " (" << pipeline.messagesEmitted << " events, " << totalSeconds << " s)\n";
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
    if (argc > 1 && std::string(argv[1]) == "--convert-one")
    {
        std::vector<char *> args{ argv[0] };
        args.insert(args.end(), argv + 2, argv + argc);
        return convert_one(static_cast<int>(args.size()), args.data());
    }
    return flirt::run_batch(argc, argv);
}
