#ifndef FLIRT_LISTFILE_SCAN_H
#define FLIRT_LISTFILE_SCAN_H

#include <mesytec-mvlc/mvlc_readout_parser.h>

#include "flirt/listfile.h"
#include "flirt/vme_config.h"

namespace flirt
{

// The readout parser for the recorded crate config, after mvme's
// sanitize_readout_stacks, checked against the VME config; throws
// unsupported_input on a mismatch (pipeline.cc, mvlc_stream_worker.cc:549-562).
mesytec::mvlc::readout_parser::ReadoutParserState make_listfile_parser(const ListfileInfo &listfile,
                                                                        const VMEConfig &vmeConfig);

}

#endif
