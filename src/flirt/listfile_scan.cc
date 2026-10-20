#include "flirt/listfile_scan.h"

#include <mesytec-mvlc/mvlc_command_builders.h>
#include <mesytec-mvlc/mvlc_readout_config.h>

namespace mvlc = mesytec::mvlc;
namespace parser = mesytec::mvlc::readout_parser;

namespace flirt
{

namespace
{

// mvme_mvlc::sanitize_readout_stacks (mvlc_daq.cc:455-474): drop command
// groups that produce no output so parser and mvme module indexes line up.
std::vector<mvlc::StackCommandBuilder> sanitize_readout_stacks(
    const std::vector<mvlc::StackCommandBuilder> &inputStacks)
{
    std::vector<mvlc::StackCommandBuilder> result;

    for (const auto &srcStack: inputStacks)
    {
        mvlc::StackCommandBuilder dstStack;
        dstStack.setName(srcStack.getName());

        for (const auto &srcGroup: srcStack.getGroups())
            if (mvlc::produces_output(srcGroup))
                dstStack.addGroup(srcGroup);

        result.emplace_back(dstStack);
    }

    return result;
}

}

parser::ReadoutParserState make_listfile_parser(const ListfileInfo &listfile, const VMEConfig &vmeConfig)
{
    // The readout structure comes from the recorded crate config; mvme derives
    // the same stacks from the VME config (mvlc_stream_worker.cc:549-562).
    auto crateConfig = mvlc::crate_config_from_yaml(listfile.crateConfigYaml);

    if (crateConfig.stacks.size() != vmeConfig.events.size())
        throw unsupported_input("crate config has " + std::to_string(crateConfig.stacks.size())
                                + " readout stacks but the VME config has "
                                + std::to_string(vmeConfig.events.size()) + " events");

    auto parserState = parser::make_readout_parser(sanitize_readout_stacks(crateConfig.stacks));

    for (size_t ei = 0; ei < vmeConfig.events.size(); ++ei)
    {
        auto enabled = make_module_index_map(vmeConfig.events[ei]).size();
        if (parserState.readoutStructure.at(ei).size() != enabled)
            throw unsupported_input("event '" + vmeConfig.events[ei].name.toStdString()
                                    + "': readout stack has "
                                    + std::to_string(parserState.readoutStructure.at(ei).size())
                                    + " output groups but " + std::to_string(enabled)
                                    + " modules are enabled");
    }

    return parserState;
}

}
