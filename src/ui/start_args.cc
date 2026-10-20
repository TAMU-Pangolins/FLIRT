#include "start_args.h"

#include <algorithm>
#include <iterator>

#include "common/compression.h"
#include "common/names.h"

namespace flirt
{

using json = nlohmann::json;

namespace
{

bool starts_with(const std::string &s, const std::string &prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

}

std::vector<std::string> start_arguments(const json &body, std::string &error)
{
    auto fail = [&](const std::string &message) { error = message; return std::vector<std::string>(); };
    if (!body.is_object())
        return fail("the body must be a JSON object");

    static const char *const known[] = { "inputs", "outputDir", "analysis", "waveforms", "extractHitFlags",
                                         "officialWaveformBranches",
                                         "jobs", "threads", "acceptDamagedInput", "experimentName",
                                         "experimentTitle", "ditherOffset", "compression" };
    for (auto it = body.begin(); it != body.end(); ++it)
        if (std::find_if(std::begin(known), std::end(known), [&](const char *k) { return it.key() == k; })
            == std::end(known))
            return fail("unknown field \"" + it.key() + "\"");

    auto absolute = [](const json &j) { return j.is_string() && starts_with(j.get<std::string>(), "/"); };

    std::vector<std::string> args;
    const auto inputs = body.value("inputs", json());
    if (!inputs.is_array() || inputs.empty())
        return fail("inputs must be a list of absolute paths");
    for (const auto &input: inputs)
    {
        if (!absolute(input))
            return fail("inputs must be absolute paths");
        args.push_back(input.get<std::string>());
    }

    const auto outputDir = body.value("outputDir", json());
    if (!absolute(outputDir))
        return fail("outputDir must be an absolute path");
    // Only standard names (common/names.h) for the output folder and the
    // analysis file; a listfile's name is refused per run by flirt-convert.
    if (const auto refusal = name_refusal(own_name(outputDir.get<std::string>()));
        !refusal.empty() && outputDir.get<std::string>().find_first_not_of('/') != std::string::npos)
        return fail("output folder " + refusal);
    args.insert(args.end(), { "-o", outputDir.get<std::string>() });

    auto boolean = [&](const char *name, bool def, bool &value) {
        const auto j = body.value(name, json(def));
        if (!j.is_boolean())
            return false;
        value = j.get<bool>();
        return true;
    };
    auto integer = [&](const char *name, int64_t def, int64_t &value) {
        const auto j = body.value(name, json(def));
        if (!j.is_number_integer())
            return false;
        value = j.get<int64_t>();
        return true;
    };
    auto string = [&](const char *name, const std::string &def, std::string &value) {
        const auto j = body.value(name, json(def));
        if (!j.is_string())
            return false;
        value = j.get<std::string>();
        return true;
    };

    const auto analysis = body.value("analysis", json({ {"mode", "archive"} }));
    if (!analysis.is_object() || !analysis.value("mode", json()).is_string())
        return fail("analysis must be {\"mode\": \"archive\" | \"default-filters\" | \"file\", \"path\": ...}");
    const auto mode = analysis["mode"].get<std::string>();
    if (mode == "default-filters")
        args.push_back("--default-filters");
    else if (mode == "file")
    {
        if (!absolute(analysis.value("path", json())))
            return fail("analysis mode \"file\" needs an absolute path");
        if (const auto refusal = name_refusal(own_name(analysis["path"].get<std::string>())); !refusal.empty())
            return fail("analysis file " + refusal);
        args.insert(args.end(), { "--analysis", analysis["path"].get<std::string>() });
    }
    else if (mode != "archive")
        return fail("analysis mode must be \"archive\", \"default-filters\" or \"file\"");

    bool waveforms, hitFlags, official, acceptDamaged;
    int64_t jobs, threads, ditherOffset;
    std::string name, title, compression;
    if (!boolean("waveforms", true, waveforms) || !boolean("extractHitFlags", false, hitFlags)
        || !boolean("officialWaveformBranches", false, official) || !boolean("acceptDamagedInput", false, acceptDamaged))
        return fail("waveforms, extractHitFlags, officialWaveformBranches and acceptDamagedInput must be true or false");
    if (!integer("jobs", 1, jobs) || !integer("threads", 1, threads) || !integer("ditherOffset", 0, ditherOffset))
        return fail("jobs, threads and ditherOffset must be integers");
    if (!string("experimentName", "Experiment", name) || !string("experimentTitle", "", title)
        || !string("compression", default_compression().name, compression))
        return fail("experimentName, experimentTitle and compression must be strings");

    // Values out of range are flirt-convert's to refuse (its usage-error event).
    if (!waveforms)
        args.push_back("--no-waveforms");
    if (hitFlags)
        args.push_back("--extract-hit-flags");
    if (official)
        args.push_back("--official-waveform-branches");
    if (acceptDamaged)
        args.push_back("--accept-damaged-input");
    if (jobs != 1)
        args.insert(args.end(), { "-j", std::to_string(jobs) });
    if (threads != 1)
        args.insert(args.end(), { "--threads", std::to_string(threads) });
    if (ditherOffset != 0)
        args.insert(args.end(), { "--dither-offset", std::to_string(ditherOffset) });
    if (name != "Experiment")
        args.insert(args.end(), { "--experiment-name", name });
    if (!title.empty())
        args.insert(args.end(), { "--experiment-title", title });
    if (compression != default_compression().name)
        args.insert(args.end(), { "--compression", compression });
    return args;
}

}
