#ifndef FLIRT_APPS_BATCH_H
#define FLIRT_APPS_BATCH_H

#include <string>

namespace flirt
{

// The batch parent of flirt-convert: `flirt-convert <input>... -o <outdir> ...`.
// Checks the arguments, then converts each listfile in a child process
// (`flirt-convert --convert-one ...`), at most -j at a time. Returns the exit
// code. Never initializes ROOT.
int run_batch(int argc, char *argv[]);

}

#endif
