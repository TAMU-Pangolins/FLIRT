// flirt-convert: an mvme MVLC listfile archive -> <runId>_raw.root and
// <runId>_histos.root as official mvme + mvme_root_client produce them.
// Inputs are only ever read.

#include <chrono>
#include <climits>
#include <iostream>
#include <sys/stat.h>

#include "common/platform.h"
#include "flirt/analysis_config.h"
#include "flirt/listfile.h"
#include "flirt/module_templates.h"
#include "flirt/pipeline.h"
#include "flirt/root_output.h"
#include "flirt/threaded_sink.h"
#include "flirt/vme_config.h"

namespace es = mvme::event_server;

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
              << " [--dither-offset <N>]\n"
              << "\n"
              << "  --dither-offset     skip N draws of TRandom3(4357) first; 0 matches a fresh\n"
              << "                      mvme_root_client --single-run (default: 0)\n";
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

int convert_one(int argc, char *argv[])
{
    std::string input, outDir;
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

        // mvme reads module templates from <prefix>/templates (template_system.cc:201-205).
        const auto templatesDir = own_prefix() + "/templates";
        const auto templates = flirt::read_module_templates(QString::fromStdString(templatesDir));
        if (templates.empty())
            throw std::runtime_error("no mvme module templates found in " + templatesDir);

        auto listfile = flirt::read_listfile_info(input);
        auto vmeConfig = flirt::parse_vme_config(listfile.vmeConfigJson, templates);

        if (listfile.analysisJson.empty())
            throw flirt::unsupported_input("archive contains no analysis.analysis");

        auto analysis = flirt::parse_analysis(listfile.analysisJson, vmeConfig);

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

        const auto output = writer.publish();

        const double totalSeconds = seconds_since(t0);
        std::cerr << "wrote " << output.rawFile << ", " << output.histosFile
                  << " (" << pipeline.messagesEmitted << " events, " << totalSeconds << " s)\n";
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
