#include "flirt/waveform_output.h"

#include <TDirectory.h>
#include <TFile.h>
#include <TTree.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>

#include "flirt/listfile.h"
#include "flirt/root_paths.h"
#include "flirt/threaded_sink.h"
#include "util/variablify.h"

namespace flirt
{

std::vector<WaveformTreeSpec> waveform_trees(const VMEConfig &vmeConfig, const AnalysisConfig &analysis)
{
    std::vector<WaveformTreeSpec> result;

    for (size_t ei = 0; ei < vmeConfig.events.size() && ei < analysis.waveformModules.size(); ++ei)
    {
        const auto &modules = vmeConfig.events[ei].modules;

        for (int mi: analysis.waveformModules[ei])
        {
            WaveformTreeSpec spec;
            spec.eventIndex = static_cast<int>(ei);
            spec.moduleIndex = mi;
            // The module's name in physics_event (make_vme_tree_description,
            // event_server_util.cc:62-91, as extract.cc builds it).
            spec.name = variablify(modules.at(mi).name).toStdString();
            spec.module = modules[mi].name.toStdString();
            spec.moduleType = modules[mi].typeName.toStdString();
            spec.title = spec.moduleType + " sample traces, one entry per channel trace";

            for (const auto &other: result)
                if (other.name == spec.name)
                    throw unsupported_input("modules '" + other.module + "' and '" + spec.module
                                            + "' would both get the waveform tree '" + spec.name
                                            + "'; use --no-waveforms");
            result.push_back(spec);
        }
    }

    return result;
}

struct WaveformWriter::Private
{
    std::unique_ptr<TFile> file;
    std::string path;               // as given; ROOT knows the file by its base name
    std::vector<WaveformTreeSpec> specs;
    std::vector<TTree *> trees;     // owned by the file
    std::map<std::pair<int, int>, size_t> treeIndexes; // (eventIndex, moduleIndex)
    std::vector<WaveformTreeStats> stats;
    double fillSeconds = 0, writeCloseSeconds = 0;

    // The branch buffers, shared by all trees.
    Long64_t event = 0;
    Int_t channel = 0;
    ULong64_t timestamp = 0;
    Int_t config = 0, phase = 0, length = 0, nsamples = 0;
    std::vector<Short_t> samples = std::vector<Short_t>(1024);

    std::unique_ptr<BasicThreadedSink<WaveformBatch>> sink; // last: its thread uses the above

    void fill(const WaveformBatch &batch);
};

WaveformWriter::WaveformWriter(const std::string &path, const std::vector<WaveformTreeSpec> &trees,
                               int compression, size_t capacity)
    : d(std::make_unique<Private>())
{
    if (path.empty() || path[0] != '/')
        throw std::logic_error("WaveformWriter: path must be absolute: " + path);

    // ROOT gets only the base name, from inside its folder (root_paths.h).
    const auto slash = path.rfind('/');
    const std::string dir = slash == 0 ? "/" : path.substr(0, slash);
    const std::string base = path.substr(slash + 1);
    if (root_safe_name(base) != base)
        throw std::logic_error("WaveformWriter: base name ROOT would read as a URL or expand: " + base);

    d->path = path;
    d->specs = trees;
    d->stats.resize(trees.size());

    {
        // The file makes itself the current directory; restore this thread's.
        TDirectory::TContext context;

        // As the raw file: by default the TFile default compression (root_output.cc).
        {
            ScopedChdir cd(dir);
            d->file = std::make_unique<TFile>(base.c_str(), "recreate", "", compression);
        }
        if (d->file->IsZombie() || !d->file->IsOpen())
            throw std::runtime_error("error opening output file " + path + " for writing: "
                                     + std::strerror(d->file->GetErrno()));

        for (size_t i = 0; i < trees.size(); ++i)
        {
            const auto &spec = trees[i];
            auto tree = new TTree(spec.name.c_str(), spec.title.c_str(), 99, d->file.get());
            tree->SetDirectory(d->file.get());
            tree->Branch("event", &d->event, "event/L");
            tree->Branch("channel", &d->channel, "channel/I");
            tree->Branch("timestamp", &d->timestamp, "timestamp/l");
            tree->Branch("config", &d->config, "config/I");
            tree->Branch("phase", &d->phase, "phase/I");
            tree->Branch("length", &d->length, "length/I");
            tree->Branch("nsamples", &d->nsamples, "nsamples/I");
            tree->Branch("samples", d->samples.data(), "samples[nsamples]/S");
            d->trees.push_back(tree);
            d->treeIndexes[{ spec.eventIndex, spec.moduleIndex }] = i;
        }
    }

    auto self = d.get();
    d->sink = std::make_unique<BasicThreadedSink<WaveformBatch>>(
        [self](WaveformBatch &batch) { self->fill(batch); }, capacity);
}

WaveformWriter::~WaveformWriter()
{
    d->sink.reset(); // stops and joins the thread
    if (d->file)
    {
        TDirectory::TContext context;
        d->file->Close();
    }
}

void WaveformWriter::Private::fill(const WaveformBatch &batch)
{
    const auto t0 = std::chrono::steady_clock::now();

    for (const auto &ev: batch.events)
    {
        auto it = treeIndexes.find({ ev.eventIndex, ev.moduleIndex });
        if (it == treeIndexes.end())
            throw std::logic_error("waveform traces for event " + std::to_string(ev.eventIndex)
                                   + ", module " + std::to_string(ev.moduleIndex) + " without a tree");
        TTree *tree = trees[it->second];
        auto &st = stats[it->second];

        event = ev.entry;
        timestamp = ev.decoded.timestamp;

        // In decoder order: every trace, every sample (mdpp_decode.h ChannelTrace).
        for (const auto &trace: ev.decoded.traces)
        {
            const auto n = static_cast<size_t>(trace.samples.size());

            if (n > samples.size())
            {
                samples.resize(std::max(n, 2 * samples.size()));
                for (auto t: trees)
                    t->SetBranchAddress("samples", samples.data());
            }

            channel = trace.channel;
            config = trace.traceHeader.parts.config;
            phase = trace.traceHeader.parts.phase;
            length = trace.traceHeader.parts.length;
            nsamples = static_cast<Int_t>(n);
            std::copy(trace.samples.begin(), trace.samples.end(), samples.begin());

            if (tree->Fill() < 0)
                throw std::runtime_error(std::string("error filling waveform tree ") + tree->GetName());

            ++st.traces;
            st.samples += n;
            if (n != 2u * static_cast<size_t>(trace.traceHeader.parts.length))
                ++st.tracesOtherLength;
            if (n >= st.tracesPerLength.size())
                st.tracesPerLength.resize(n + 1);
            ++st.tracesPerLength[n];
        }
    }

    fillSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void WaveformWriter::push(WaveformBatch &batch)
{
    d->sink->push(batch);
}

void WaveformWriter::finish()
{
    d->sink->finish();

    const auto t0 = std::chrono::steady_clock::now();
    {
        TDirectory::TContext context;
        if (d->file->Write() < 0)
            throw std::runtime_error("error writing " + d->path);
        d->file->Close();
        if (d->file->TestBit(TFile::kWriteError))
            throw std::runtime_error("error writing " + d->path);
    }
    d->trees.clear();
    d->writeCloseSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

const std::vector<WaveformTreeSpec> &WaveformWriter::trees() const { return d->specs; }
const std::vector<WaveformTreeStats> &WaveformWriter::stats() const { return d->stats; }
double WaveformWriter::fillSeconds() const { return d->fillSeconds; }
double WaveformWriter::writeCloseSeconds() const { return d->writeCloseSeconds; }

}
