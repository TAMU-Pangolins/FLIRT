// flirt-convert: an mvme MVLC listfile archive -> <runId>_raw.root and
// <runId>_histos.root as official mvme + mvme_root_client produce them.
// Inputs are only ever read.

#include <algorithm>
#include <chrono>
#include <climits>
#include <iostream>
#include <sys/stat.h>

#include "common/platform.h"
#include "flirt/analysis_config.h"
#include "flirt/integrity.h"
#include "flirt/listfile.h"
#include "flirt/module_templates.h"
#include "flirt/pipeline.h"
#include "flirt/root_output.h"
#include "flirt/threaded_sink.h"
#include "flirt/vme_config.h"

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

void usage(const char *argv0)
{
    std::cerr << "usage: " << argv0 << " <listfile.zip> -o <outdir>"
              << " [--dither-offset <N>] [--accept-damaged-input]\n"
              << "\n"
              << "  --dither-offset     skip N draws of TRandom3(4357) first; 0 matches a fresh\n"
              << "                      mvme_root_client --single-run (default: 0)\n"
              << "  --accept-damaged-input\n"
              << "                      convert and publish even if integrity checks fail\n";
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

int convert_one(int argc, char *argv[])
{
    std::string input, outDir;
    bool acceptDamagedInput = false;
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
        std::vector<std::string> outputs = { outDir + "/" + listfile.runId + "_raw.root",
                                             outDir + "/" + listfile.runId + "_histos.root" };

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

        runInfo.runId = listfile.runId;
        runInfo.replaySourceFile = input.substr(input.rfind('/') + 1);
        rootOpts.outDir = outDir;
        rootOpts.runId = listfile.runId;
        rootOpts.mvmePrefix = own_prefix();

        flirt::RootWriter writer(rootOpts);

        // ServerInfo first, as on a real event server connection.
        es::Message serverInfo;
        serverInfo.type = es::MessageType::ServerInfo;
        auto text = es::json::object().dump();
        serverInfo.contents.assign(text.begin(), text.end());
        writer.handleMessage(serverInfo);

        // Producer and consumer in two threads, joined by message batches. The
        // consumer is joined by finish(), or on an exception by its destructor,
        // before `writer` is used again or destroyed.
        flirt::PipelineResult pipeline;
        {
            flirt::ThreadedSink consumer([&](flirt::MessageBatch &batch)
                                         {
                                             writer.handleBatch(batch);
                                         });

            pipeline = flirt::run_pipeline(listfile, vmeConfig, analysis, runInfo,
                                           [&](flirt::MessageBatch &batch)
                                           {
                                               consumer.push(batch);
                                           });
            consumer.finish();
        }
        writer.close();

        checks.set(check::ListfileEntry, pipeline.listfileEntry.result(), pipeline.listfileEntry.detail());

        const auto [parserResult, parserDetail] = parser_health(pipeline);
        checks.set(check::Parser, parserResult, parserDetail);

        // I5 once nothing reads the inputs any more.
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

        const auto output = writer.publish();

        const double totalSeconds = seconds_since(t0);
        std::cerr << "wrote " << output.rawFile << ", " << output.histosFile
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
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

}

int main(int argc, char *argv[])
{
    return convert_one(argc, argv);
}
