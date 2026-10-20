// mvme_root_client's output, produced in-process from event-server Messages.
//
// The client's templates and event base classes are used unmodified. The
// ClientContext logic is ported below in the same order of operations; line
// numbers refer to external/mvme/src/event_server/clients/mvme_root_client/
// mvme_root_client.cc. Left out on purpose, as none of it affects the output
// files: the TCP client loop, writing the generated code and build files into
// the working directory (:603-735), the ROOT pre-make hook (:494-502), and
// loading and calling the user's analysis.so (:536-567, :791-818, :917-922,
// :1062-1067).
// The one replacement is how ROOT learns the generated classes: they are
// declared to ROOT's interpreter instead of built with make (:504-524).

#include "flirt/root_output.h"
#include "flirt/root_paths.h"

#include <TFile.h>
#include <TH1D.h>
#include <TInterpreter.h>
#include <TROOT.h>
#include <TRandom3.h>
#include <TSystem.h>
#include <TTree.h>

#include <Mustache/mustache.hpp>
#include "mvme_root_event_objects.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace mu = kainjow::mustache;
namespace es = mvme::event_server;

namespace flirt
{

namespace
{

// :88-100 (the templates for the build and analysis skeleton files are not needed)
static const char *exportHeaderTemplate =
#include "templates/user_objects.h.mustache"
;

static const char *exportImplTemplate =
#include "templates/user_objects.cxx.mustache"
;

static const char *exportLinkDefTemplate =
#include "templates/user_objects_LinkDef.h.mustache"
;

// :137-206
struct MakeRawHistosResult
{
    std::vector<std::vector<TH1D *>> histos;
    size_t histoCount;
    size_t histoMem;
};

static MakeRawHistosResult make_raw_histograms(const MVMEExperiment *exp, TFile *histoOutFile)
{
    MakeRawHistosResult result = {};

    for (const auto &event: exp->GetEvents())
    {
        auto dir = histoOutFile->mkdir(event->GetName());
        assert(dir);
        dir->cd();
        std::vector<TH1D *> eventHistos;

        for (const auto &module: event->GetModules())
        {
            for (const auto &userStorage: module->GetDataStorages())
            {
                // cap the bin count at 16 bits
                unsigned bins = 1u << std::min(userStorage.bits, 16u);

                for (size_t paramIndex = 0; paramIndex < userStorage.size; paramIndex++)
                {
                    std::string histoName;

                    if (paramIndex < userStorage.paramNames.size())
                    {
                        histoName = module->GetName() + std::string("_")
                            + userStorage.paramNames[paramIndex];
                    }
                    else
                    {
                        histoName = module->GetName() + std::string("_") + userStorage.name
                            + "[" + std::to_string(paramIndex) + "]";
                    }

                    auto histo = new TH1D(
                        histoName.c_str(),
                        histoName.c_str(),
                        bins,
                        0.0,
                        std::pow(2.0, userStorage.bits)
                        );

                    eventHistos.emplace_back(histo);

                    result.histoMem += bins * sizeof(double);
                    ++result.histoCount;
                }
            }
        }

        result.histos.emplace_back(std::move(eventHistos));
        histoOutFile->cd();
    }

    return result;
}

// :299-371
static mu::data build_event_template_data(const es::StreamInfo &streamInfo)
{
    mu::data mu_vmeEvents = mu::data::type::list;

    for (const auto &event: streamInfo.vmeTree.events)
    {
        mu::data mu_vmeModules = mu::data::type::list;

        for (const auto &module: event.modules)
        {
            mu::data mu_moduleDataMembers = mu::data::type::list;
            mu::data mu_moduleRefMembers = mu::data::type::list;

            for (const auto &edd: streamInfo.eventDataDescriptions)
            {
                if (edd.eventIndex != event.eventIndex) continue;

                for (const auto &dsd: edd.dataSources)
                {
                    if (dsd.moduleIndex != module.moduleIndex) continue;

                    mu::data mu_dataMember = mu::data::type::object;
                    mu_dataMember["name"] = dsd.name;
                    mu_dataMember["size"] = std::to_string(dsd.size);
                    mu_dataMember["bits"] = std::to_string(dsd.bits);
                    mu::data mu_paramNames = mu::data::type::list;

                    size_t paramCount = std::min(dsd.paramNames.size(),
                                                 static_cast<size_t>(dsd.size));

                    for (size_t paramIndex = 0; paramIndex < paramCount; paramIndex++)
                    {
                        mu::data mu_refMember = mu::data::type::object;
                        mu_refMember["name"] = dsd.paramNames[paramIndex];
                        mu_refMember["index"] = std::to_string(paramIndex);
                        mu_refMember["target"] = dsd.name;

                        mu_moduleRefMembers.push_back(mu_refMember);

                        mu_paramNames.push_back(dsd.paramNames[paramIndex]);
                    }

                    mu_dataMember["param_names"] = mu::data{mu_paramNames};
                    mu_moduleDataMembers.push_back(mu_dataMember);
                }
            }

            mu::data mu_module = mu::data::type::object;
            mu_module["struct_name"] = "Module_" + module.name;
            mu_module["name"] = module.name;
            mu_module["title"] = "Module " + module.name;
            mu_module["var_name"] = module.name;
            mu_module["data_members"] = mu::data{mu_moduleDataMembers};
            mu_module["ref_members"] = mu::data{mu_moduleRefMembers};
            mu_module["event_name"] = event.name;
            mu_vmeModules.push_back(mu_module);
        }

        mu::data mu_event = mu::data::type::object;
        mu_event["struct_name"] = "Event_" + event.name;
        mu_event["title"] = "Storage for event '" + event.name + "'";
        mu_event["name"] = event.name;
        mu_event["var_name"] = event.name;
        mu_event["modules"] = mu::data{mu_vmeModules};
        mu_vmeEvents.push_back(mu_event);
    }

    return mu_vmeEvents;
}

// render_template lambda of generate_code_file (:429-435)
std::string render_template(const char *contents, const mu::data &data)
{
    auto no_escape = [](const auto &s) { return s; };
    mu::mustache tmpl(contents);
    tmpl.set_custom_escape(no_escape);
    return tmpl.render(data);
}

bool write_file(const std::string &contents, const std::string &filename)
{
    std::ofstream out(filename);
    out << contents;
    out.close();
    return static_cast<bool>(out);
}

bool file_exists(const std::string &filename)
{
    struct stat buffer;
    return stat(filename.c_str(), &buffer) == 0;
}

// Template data of ClientContext::beginRun (:667-677).
mu::data make_template_data(const es::StreamInfo &streamInfo)
{
    std::string expName = streamInfo.infoJson["ExperimentName"];
    std::string expTitle = streamInfo.infoJson["ExperimentTitle"];

    mu::data mu_vmeEvents = build_event_template_data(streamInfo);
    mu::data mu_data;
    mu_data["vme_events"] = mu::data{mu_vmeEvents};
    mu_data["exp_name"] = expName;
    std::string expStructName = expName;
    mu_data["exp_struct_name"] = expStructName;
    mu_data["exp_title"] = expTitle;
    mu_data["header_guard"] = expName;
    mu_data["header_filename"] = expName + "_mvme.h";
    mu_data["impl_filename"] = expName + "_mvme.cxx";
    return mu_data;
}

// :569-575
inline double make_quiet_nan()
{
    static const double result = std::numeric_limits<double>::quiet_NaN();
    return result;
}

using MVMERunInfo = std::map<std::string, std::string>;

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// A failed write (e.g. a full disk) sets TFile::kWriteError, and ROOT only
// prints it; such a file must not be published. `name` is the real name, not
// the staging name ROOT knows the file by.
void throw_if_write_error(TFile *file, const std::string &name)
{
    if (file->TestBit(TFile::kWriteError))
        throw std::runtime_error("error writing " + name);
}

} // anon namespace

struct RootWriter::Private: public es::Client
{
    RootOutputOptions opts;
    RootOutputResult result;

    std::unique_ptr<MVMEExperiment> exp;
    std::vector<TTree *> eventTrees;
    TFile *treeOutFile = nullptr;           // the client leaves it to ROOT to close
    std::unique_ptr<TFile> histoOutFile;
    MakeRawHistosResult rawHistos;

    // Stream description without the omitted data sources, and for every
    // transmitted data source its storage index in the generated event
    // class, or -1 when omitted.
    es::StreamInfo storedStreamInfo;
    std::vector<std::vector<int>> storageIndexes;

    std::string tmpDir;                     // private directory inside outDir
    std::string tmpBase;                    // the run ID it was made for
    std::vector<std::string> tmpFiles;      // names created inside tmpDir (root_safe_name())
    std::string rawName, histosName;        // real names, as published
    std::vector<std::string> extraNames;    // addFile() real names, in order
    bool endRunSeen = false;
    bool synced = false;                    // sync() done; publish() may follow
    RootTimings timings;

    void serverInfo(const es::Message &, const es::json &) override {}
    void beginRun(const es::Message &msg, const es::StreamInfo &streamInfo) override;
    void eventData(const es::Message &msg, int eventIndex,
                   const std::vector<es::DataSourceContents> &contents) override;
    void endRun(const es::Message &msg, const es::json &info) override;
    void error(const es::Message &, const std::exception &) override {}

    void loadExperimentClasses(const es::StreamInfo &streamInfo);
    void selectStoredDataSources(const es::StreamInfo &streamInfo);
    void makeTmpDir(const std::string &base);
    void cleanup();
};

void RootWriter::Private::makeTmpDir(const std::string &base)
{
    if (!tmpDir.empty())
    {
        if (base != tmpBase)
            throw std::runtime_error("run ID " + base + " differs from the one the outputs were prepared for, "
                                     + tmpBase);
        return;
    }

    std::string tmpl = opts.outDir + "/.flirt-" + base + "-XXXXXX";
    std::vector<char> tmplBuf(tmpl.begin(), tmpl.end());
    tmplBuf.push_back('\0');
    if (!mkdtemp(tmplBuf.data()))
        throw std::runtime_error("mkdtemp " + tmpl + ": " + std::strerror(errno));
    tmpDir = tmplBuf.data();
    tmpBase = base;
}

void RootWriter::Private::selectStoredDataSources(const es::StreamInfo &streamInfo)
{
    storedStreamInfo = streamInfo;
    storageIndexes.clear();

    for (size_t ei = 0; ei < streamInfo.eventDataDescriptions.size(); ++ei)
    {
        const auto &sources = streamInfo.eventDataDescriptions[ei].dataSources;
        const auto omit = ei < opts.omitDataSources.size() ? opts.omitDataSources[ei]
                                                            : std::vector<size_t>{};
        auto &stored = storedStreamInfo.eventDataDescriptions[ei].dataSources;
        stored.clear();
        std::vector<int> indexes;

        for (size_t dsIndex = 0; dsIndex < sources.size(); ++dsIndex)
        {
            if (std::find(omit.begin(), omit.end(), dsIndex) != omit.end())
                indexes.push_back(-1);
            else
            {
                indexes.push_back(static_cast<int>(stored.size()));
                stored.push_back(sources[dsIndex]);
            }
        }

        for (auto dsIndex: omit)
            if (dsIndex >= sources.size())
                throw std::runtime_error("data source to omit out of range: event "
                                         + std::to_string(ei) + ", index " + std::to_string(dsIndex));

        storageIndexes.push_back(indexes);
    }
}

// :577-929
void RootWriter::Private::beginRun(const es::Message &, const es::StreamInfo &streamInfo)
{
    const auto t0 = std::chrono::steady_clock::now();

    if (streamInfo.infoJson.count("runInProgress")
        && static_cast<bool>(streamInfo.infoJson["runInProgress"]))
        throw std::runtime_error("run already in progress"); // :588-598

    std::string expStructName = streamInfo.infoJson["ExperimentName"];

    // Output names (:829-839, :891-896) must not exist yet: FLIRT never replaces files.
    // From the options, not streamInfo.runId: BeginRun's JSON is cleaned of
    // invalid UTF-8, the file names and RunID keep the real bytes.
    const std::string base = opts.runId.empty() ? "unknown_run" : opts.runId;
    rawName = base + "_raw.root";
    histosName = base + "_histos.root";

    for (const auto &name: { rawName, histosName })
        if (file_exists(opts.outDir + "/" + name))
            throw std::runtime_error("output exists, refusing to overwrite: " + opts.outDir + "/" + name);

    makeTmpDir(base);

    selectStoredDataSources(streamInfo);
    loadExperimentClasses(storedStreamInfo);

    if (!exp)
        throw std::runtime_error("error creating an instance of the experiment class '"
                                 + expStructName + "'"); // :739-746

    // :748-776
    if (storedStreamInfo.eventDataDescriptions.size() != exp->GetNumberOfEvents())
        throw std::runtime_error("StreamInfo and generated Experiment disagree on the event count");

    for (size_t eventIndex = 0; eventIndex < exp->GetNumberOfEvents(); eventIndex++)
    {
        auto &edd = storedStreamInfo.eventDataDescriptions.at(eventIndex);
        if (edd.dataSources.size() != exp->GetEvent(eventIndex)->GetDataSourceStorages().size())
            throw std::runtime_error("StreamInfo and generated Event class disagree on data sources");
    }

    {
        // The client writes into its working directory; FLIRT's is the private
        // tmpDir. ROOT gets the staging name; publish() links the real one.
        ScopedChdir cd(tmpDir);

        // :841-885. The client gives TFile no compression, so it gets ROOT's
        // compiled default.
        const auto rawStaging = root_safe_name(rawName);
        treeOutFile = new TFile(rawStaging.c_str(), "recreate");
        tmpFiles.push_back(rawStaging);

        if (treeOutFile->IsZombie() || !treeOutFile->IsOpen())
            throw std::runtime_error("error opening output file " + rawName + " for writing: "
                                     + std::strerror(treeOutFile->GetErrno()));

        eventTrees = exp->MakeTrees();

        for (auto &tree: eventTrees)
        {
            tree->SetMaxTreeSize(100000000000LL); // m_rootMaxTreeSize default, :286
        }

        MVMERunInfo info;
        info["ExperimentName"] = exp->GetName();
        info["RunID"] = opts.runId;
        treeOutFile->WriteObject(&info, "MVMERunInfo");
        treeOutFile->Write();

        // :890-915
        const auto histosStaging = root_safe_name(histosName);
        histoOutFile = std::make_unique<TFile>(histosStaging.c_str(), "recreate");
        tmpFiles.push_back(histosStaging);

        if (histoOutFile->IsZombie() || !histoOutFile->IsOpen())
            throw std::runtime_error("error opening histo output file " + histosName
                                     + " for writing: " + std::strerror(histoOutFile->GetErrno()));

        rawHistos = make_raw_histograms(exp.get(), histoOutFile.get());
    }

    result.eventHits = std::vector<size_t>(streamInfo.eventDataDescriptions.size());
    result.totalDataBytes = 0;

    // Dither: the client uses ROOT's default gRandom, TRandom3 seeded with 4357
    // in a fresh process. Reset it explicitly, then skip the requested draws.
    if (!gRandom || gRandom->IsA() != TRandom3::Class())
        throw std::runtime_error("gRandom is not a TRandom3");
    gRandom->SetSeed(4357);
    for (uint64_t i = 0; i < opts.ditherOffset; ++i)
        gRandom->Rndm();

    timings.setup += seconds_since(t0);
}

// Replaces compile_and_load_experiment_library (:488-534). The rendered header
// and implementation are declared to ROOT's interpreter instead of being built
// with make; the header lives in the private tmpDir while it is needed.
void RootWriter::Private::loadExperimentClasses(const es::StreamInfo &streamInfo)
{
    auto code = render_experiment_code(streamInfo);

    if (!write_file(code.header, tmpDir + "/" + code.headerFilename))
        throw std::runtime_error("error writing " + tmpDir + "/" + code.headerFilename);
    tmpFiles.push_back(code.headerFilename);

    // AddIncludePath runs gSystem->ExpandPathName on the path.
    const std::string inc = opts.mvmePrefix + "/include";
    TString p(inc.c_str());
    if (gSystem->ExpandPathName(p) || p != inc.c_str())
        throw std::runtime_error("ROOT cannot use FLIRT's install folder " + opts.mvmePrefix
                                 + ": its path holds characters ROOT reads as variables or patterns"
                                 " ($ ~ * ? [ ] { } ( )); install FLIRT in another folder");
    gInterpreter->AddIncludePath(inc.c_str());

    // The base classes come from the compiled mvme_root_event dictionary;
    // declare their header first so the macro below cannot touch them.
    // Plain ClassDef leaves Class()/IsA()/Streamer() to a compiled dictionary,
    // which interpreted classes never get; ClassDefInline defines them inline
    // with the same class version and streamer layout.
    // The header is found through the working directory (the private one), so
    // ROOT never sees the output folder's or the run's name.
    const std::string decl =
        "#include <mvme/mvme_root_event_objects.h>\n"
        "#pragma push_macro(\"ClassDef\")\n"
        "#undef ClassDef\n"
        "#define ClassDef(name, id) ClassDefInline(name, id)\n"
        + code.impl +
        "\n#pragma pop_macro(\"ClassDef\")\n";

    ScopedChdir cd(tmpDir);

    if (!gInterpreter->Declare(decl.c_str()))
        throw std::runtime_error("ROOT could not compile the generated experiment classes");

    std::string cmd = "new " + code.structName + "();";

    exp = std::unique_ptr<MVMEExperiment>(
        reinterpret_cast<MVMEExperiment *>(gROOT->ProcessLineSync(cmd.c_str())));
}

// :931-1068
void RootWriter::Private::eventData(const es::Message &, int eventIndex,
                                    const std::vector<es::DataSourceContents> &contents)
{
    auto &streamInfo = getStreamInfo();

    if (eventIndex < 0 || static_cast<size_t>(eventIndex) >= eventTrees.size())
        throw std::runtime_error("eventIndex " + std::to_string(eventIndex) + " out of range");

    auto event = exp->GetEvent(eventIndex);
    auto &edd = streamInfo.eventDataDescriptions.at(eventIndex);

    result.eventHits[eventIndex]++;

    for (size_t dsIndex = 0; dsIndex < contents.size(); dsIndex++)
    {
        const es::DataSourceContents &dsc = contents.at(dsIndex);
        const uint8_t *dscEnd = es::get_end_pointer(dsc);
        const int storageIndex = storageIndexes.at(eventIndex).at(dsIndex);

        // Omitted data source: its values take their draws (:1017-1021) and
        // are dropped. userStorage.size would have been the described size.
        Storage userStorage = {};
        if (storageIndex >= 0)
        {
            userStorage = event->GetDataSourceStorage(storageIndex);
            assert(userStorage.ptr);
            assert(userStorage.size == edd.dataSources.at(dsIndex).size);
            std::fill(userStorage.ptr, userStorage.ptr + userStorage.size, make_quiet_nan());
        }
        else
            userStorage.size = edd.dataSources.at(dsIndex).size;

        const size_t entrySize = es::get_entry_size(dsc);
        const size_t indexSize = es::get_storage_type_size(dsc.indexType);

        for (auto entryIndex = 0; entryIndex < dsc.count; entryIndex++)
        {
            const uint8_t *indexPtr = dsc.firstIndex + entryIndex * entrySize;
            const uint8_t *valuePtr = indexPtr + indexSize;

            if (indexPtr >= dscEnd || valuePtr >= dscEnd)
                throw std::runtime_error("incoming data source contents are inconsistent");

            uint32_t index = es::read_storage<uint32_t>(dsc.indexType, indexPtr);
            double value = es::read_storage<double>(dsc.valueType, valuePtr);

            if (index < userStorage.size)
            {
                // Add a random in [0, 1) to avoid binning issues (:1017-1021).
                value += gRandom->Uniform();
                if (userStorage.ptr)
                    userStorage.ptr[index] = value;
            }
            else
                throw std::runtime_error("param index value " + std::to_string(index)
                                         + " out of range");
        }

        result.totalDataBytes += es::get_entry_size(dsc) * dsc.count;
    }

    // Fill the raw histograms (:1044-1057).
    size_t histoIndex = 0;

    for (const auto &userStorage: event->GetDataSourceStorages())
    {
        for (size_t paramIndex = 0; paramIndex < userStorage.size; paramIndex++)
        {
            double paramValue = userStorage.ptr[paramIndex];

            if (!std::isnan(paramValue))
                rawHistos.histos[eventIndex][histoIndex + paramIndex]->Fill(paramValue);
        }
        histoIndex += userStorage.size;
    }

    eventTrees.at(eventIndex)->Fill();
}

// :1070-1091
void RootWriter::Private::endRun(const es::Message &, const es::json &)
{
    auto t0 = std::chrono::steady_clock::now();
    auto file = eventTrees.front()->GetCurrentFile();

    if (file && !file->IsZombie() && file->IsOpen())
    {
        file->Write();
        throw_if_write_error(file, rawName);
    }

    timings.writeClose += seconds_since(t0);
    t0 = std::chrono::steady_clock::now();

    if (histoOutFile)
    {
        histoOutFile->Write();
        // The client leaves the close to the destructor, which calls Close();
        // closing first writes the same records and shows a failed write.
        histoOutFile->Close();
        throw_if_write_error(histoOutFile.get(), histosName);
        histoOutFile = {};
    }
    rawHistos = {};
    endRunSeen = true;
    timings.histograms += seconds_since(t0);
}

void RootWriter::Private::cleanup()
{
    histoOutFile = {};
    if (treeOutFile)
    {
        treeOutFile->Close();
        delete treeOutFile;
        treeOutFile = nullptr;
    }
    // Remove only what this writer created.
    if (!tmpDir.empty())
    {
        for (const auto &name: tmpFiles)
            unlink((tmpDir + "/" + name).c_str());
        rmdir(tmpDir.c_str());
        tmpDir.clear();
    }
}

RootWriter::RootWriter(const RootOutputOptions &options)
    : d(std::make_unique<Private>())
{
    d->opts = options;
    // First, before any other ROOT use (TROOT.h).
    if (options.threadSafety)
        ROOT::EnableThreadSafety();
    // Before any ROOT file exists. The compression settings are unchanged; the
    // layout was identical with 2, 4 and 8 threads (docs/design.md, "Threads").
    if (options.implicitMTThreads > 1)
        ROOT::EnableImplicitMT(options.implicitMTThreads);
    gROOT->SetBatch(true);
}

RootWriter::~RootWriter()
{
    d->cleanup();
}

void RootWriter::handleMessage(const es::Message &msg)
{
    d->handleMessage(msg);
}

void RootWriter::handleBatch(const MessageBatch &batch)
{
    const auto t0 = std::chrono::steady_clock::now();

    for (size_t i = 0; i < batch.size; ++i)
        d->handleMessage(batch.messages[i]);

    // BeginRun and EndRun come alone and time themselves.
    if (batch.size && batch.messages[0].type == es::MessageType::EventData)
        d->timings.fill += seconds_since(t0);
}

void RootWriter::close()
{
    if (!d->endRunSeen)
        throw std::runtime_error("close() before EndRun");

    // The client never closes the tree file itself; ROOT closes it at process
    // exit (single-run mode). Closing it here writes the same final records.
    const auto t0 = std::chrono::steady_clock::now();
    d->treeOutFile->Close();
    const bool writeError = d->treeOutFile->TestBit(TFile::kWriteError);
    delete d->treeOutFile;
    d->treeOutFile = nullptr;
    if (writeError)
        throw std::runtime_error("error writing " + d->rawName);
    d->timings.writeClose += seconds_since(t0);
}

std::string RootWriter::addFile(const std::string &runId, const std::string &name)
{
    if (d->exp)
        throw std::runtime_error("addFile() after BeginRun");
    // ROOT gets the staging name inside tmpDir; publish() links the real one.
    const auto staging = root_safe_name(name);
    auto taken = [this](const std::string &n) {
        return std::find(d->tmpFiles.begin(), d->tmpFiles.end(), n) != d->tmpFiles.end()
            || std::find(d->extraNames.begin(), d->extraNames.end(), n) != d->extraNames.end();
    };
    if (name.empty() || name.find('/') != std::string::npos || taken(name) || taken(staging))
        throw std::runtime_error("addFile(): bad or repeated name '" + name + "'");
    if (file_exists(d->opts.outDir + "/" + name))
        throw std::runtime_error("output exists, refusing to overwrite: " + d->opts.outDir + "/" + name);

    d->makeTmpDir(runId.empty() ? "unknown_run" : runId);
    d->tmpFiles.push_back(staging);
    d->extraNames.push_back(name);
    return d->tmpDir + "/" + staging;
}

namespace
{

// Flushes fd to the disk itself. On macOS fsync() leaves the data in the
// drive's own cache; F_FULLFSYNC flushes that too (fsync where the file
// system refuses it). Elsewhere fsync().
bool flush_to_disk(int fd)
{
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return true;
#endif
    return fsync(fd) == 0;
}

// flush_to_disk() of a file or directory; throws naming `what` on failure.
void sync_path(const std::string &path, const std::string &what)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("cannot open " + what + " to flush it: " + std::strerror(errno));
    const bool ok = flush_to_disk(fd);
    const int err = errno;
    close(fd);
    if (!ok)
        throw std::runtime_error("error writing " + what + " (fsync): " + std::strerror(err));
}

uint64_t size_of(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        throw std::runtime_error("cannot stat " + path + ": " + std::strerror(errno));
    return static_cast<uint64_t>(st.st_size);
}

}

RootOutputResult RootWriter::sync()
{
    if (d->treeOutFile || d->tmpDir.empty())
        throw std::runtime_error("sync() before close()");

    const auto t0 = std::chrono::steady_clock::now();
    auto staged = [this](const std::string &name) { return d->tmpDir + "/" + root_safe_name(name); };
    std::vector<std::string> names = { d->rawName, d->histosName };
    names.insert(names.end(), d->extraNames.begin(), d->extraNames.end());
    for (const auto &name: names)
        sync_path(staged(name), d->opts.outDir + "/" + name);

    d->result.rawFile = d->opts.outDir + "/" + d->rawName;
    d->result.histosFile = d->opts.outDir + "/" + d->histosName;
    d->result.rawBytes = size_of(staged(d->rawName));
    d->result.histosBytes = size_of(staged(d->histosName));
    d->result.extraFiles.clear();
    d->result.extraBytes.clear();
    for (const auto &name: d->extraNames)
    {
        d->result.extraFiles.push_back(d->opts.outDir + "/" + name);
        d->result.extraBytes.push_back(size_of(staged(name)));
    }
    d->synced = true;
    d->timings.publish += seconds_since(t0);
    return d->result;
}

void RootWriter::publish(const std::string &reportName, const std::string &reportContents)
{
    if (!d->synced || d->tmpDir.empty())
        throw std::runtime_error("publish() before sync()");
    const std::string nothing = "; nothing was published";
    const auto &outDir = d->opts.outDir;

    // The report, written and flushed in the private directory first.
    const auto reportStaging = root_safe_name(reportName);
    if (reportName.empty() || reportName.find('/') != std::string::npos
        || std::find(d->tmpFiles.begin(), d->tmpFiles.end(), reportStaging) != d->tmpFiles.end())
        throw std::runtime_error("publish(): bad report name '" + reportName + "'");
    {
        const auto path = d->tmpDir + "/" + reportStaging;
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0)
            throw std::runtime_error("cannot create the report " + outDir + "/" + reportName + ": "
                                     + std::strerror(errno) + nothing);
        d->tmpFiles.push_back(reportStaging);
        std::string error;
        for (size_t done = 0; error.empty() && done < reportContents.size();)
        {
            const auto n = write(fd, reportContents.data() + done, reportContents.size() - done);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                error = n < 0 ? std::strerror(errno) : "short write";
            else
                done += static_cast<size_t>(n);
        }
        if (error.empty() && !flush_to_disk(fd))
            error = std::string(std::strerror(errno)) + " (fsync)";
        if (::close(fd) != 0 && error.empty())
            error = std::strerror(errno);
        if (!error.empty())
            throw std::runtime_error("error writing " + outDir + "/" + reportName + ": " + error + nothing);
    }

    // Test only: a kill -9 at a chosen step.
    long killAfter = -1;
    if (const char *text = std::getenv("FLIRT_TEST_KILL_AFTER_LINKS"))
        killAfter = std::strtol(text, nullptr, 10);
    auto maybe_killed = [killAfter](size_t links) {
        if (killAfter >= 0 && static_cast<size_t>(killAfter) == links)
            raise(SIGKILL);
    };

    // Publish without replacing: link() fails if the destination exists. If a
    // file cannot be published, the ones before it are taken back.
    std::vector<std::string> names = { d->rawName, d->histosName };
    names.insert(names.end(), d->extraNames.begin(), d->extraNames.end());
    names.push_back(reportName);
    std::vector<std::string> published;
    auto take_back = [&](const std::string &error) {
        for (const auto &p: published)
            unlink(p.c_str());
        throw std::runtime_error(error + nothing);
    };
    maybe_killed(0);
    for (const auto &name: names)
    {
        // The outputs' names are on disk before the report's appears.
        if (name == reportName)
        {
            try { sync_path(outDir, outDir); }
            catch (const std::exception &e) { take_back(e.what()); }
        }
        auto src = d->tmpDir + "/" + root_safe_name(name);
        auto dst = outDir + "/" + name;
        if (link(src.c_str(), dst.c_str()) != 0)
            take_back("publishing " + dst + ": " + std::strerror(errno));
        published.push_back(dst);
        maybe_killed(published.size());
    }
    // The report's name on disk too. Too late to take anything back: the set
    // is complete and published.
    try { sync_path(outDir, outDir); }
    catch (const std::exception &) { }

    d->cleanup();
}

const RootTimings &RootWriter::timings() const
{
    return d->timings;
}

ExperimentCode render_experiment_code(const es::StreamInfo &streamInfo)
{
    auto mu_data = make_template_data(streamInfo);
    std::string expName = streamInfo.infoJson["ExperimentName"];

    ExperimentCode code;
    code.structName = expName;
    code.headerFilename = expName + "_mvme.h";
    code.header = render_template(exportHeaderTemplate, mu_data);
    code.impl = render_template(exportImplTemplate, mu_data);
    code.linkDef = render_template(exportLinkDefTemplate, mu_data);
    return code;
}

std::string root_version()
{
    return gROOT->GetVersion();
}

}
