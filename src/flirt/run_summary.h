#ifndef FLIRT_RUN_SUMMARY_H
#define FLIRT_RUN_SUMMARY_H

// Run summary: when the DAQ run started and stopped, from the timestamped
// system sections mesytec-mvlc writes into the listfile.
// mvme itself never computes run times from them (mvlc_stream_worker.cc:298),
// so this is FLIRT's own glue. Plain functions, no ROOT, no Qt.

#include <cstdint>
#include <string>

namespace flirt
{

// 1 h 02 min 47 s, 22 min 05 s or 43 s.
std::string format_duration(int64_t seconds);

}

#endif
