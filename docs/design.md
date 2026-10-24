# FLIRT's design

This page is for programmers who read FLIRT's code. The code refers to its
sections as `(docs/design.md, "<section>")`. Each section says what was
decided, why, and where the decision stops working. What the programs do
is in [usage.md](usage.md). mvme's sources are cited as `file:line` in
mvme 1.21.0, which is in `external/mvme`.

## The rule that shapes everything

FLIRT must write the same ROOT files as official mvme followed by
`mvme_root_client --single-run`. They must be identical in every value,
hit mask (the record of which elements of an array had a hit), tree and
branch layout, class definition and histogram. Only three things may
differ, because ROOT writes them anew in every file: the file's UUID, its
time stamps and the compressed byte counts. On Apple silicon, one
histogram statistic may also differ (see "macOS (Apple silicon)").

So everything that shapes the output is mvme's own code. It is compiled
from the sources at the versions in `external/`, and never modified:

- mesytec-mvlc, which reads the ZIP archive, replays the recording and
  parses the readout data;
- mvme's `src/multi_event_splitter.cc`;
- mvme's `src/analysis/a2/` (liba2, which runs the analysis's extractors)
  and `src/mdpp-sampling/mdpp_decode.cc` (the decoder for the samples of
  MDPP traces);
- `mvme_root_client`'s templates, event classes and the code that fills
  the trees and histograms.

FLIRT's own code only joins these parts. It reads the configs, calls the
parts in the order mvme calls them, and adds the command line, the checks
and the programs around them. Where FLIRT's code repeats something mvme
does, its comment cites the mvme line it copies. FLIRT does not link
libmvme, mvme's main library, because it pulls in Qt Widgets and other
graphical libraries. The only part of Qt that FLIRT uses is Qt5Core, which
liba2 and the config readers need.

FLIRT also reproduces two steps of the official pipeline:

- mvme rounds each extracted value down to an integer
  (`event_server.cc:570`).
- `mvme_root_client` then adds a random number between 0 and 1,
  `gRandom->Uniform()`, to every value with a hit
  (`mvme_root_client.cc:1020`). This is called dithering. The random
  numbers come from `TRandom3(4357)`, ROOT's random generator started from
  the seed 4357, and the sequence continues across all the runs that one
  `mvme_root_client` process converts.

FLIRT starts the sequence afresh for every file, as `--single-run` does.
`--dither-offset N` skips the first N random numbers, to remake a file
that was converted later in a client's sequence (see "Finding a file's
dither offset").

**How identity is checked.** Not by reading the code, but with
`tools/compare`, which shares no code with the converter. It compares two
files key by key and class by class, the layout of every tree, every leaf
of every entry bit for bit, and the bins and statistics of every
histogram ([tools/compare/README.md](../tools/compare/README.md)). What was
compared is listed in [usage.md, "Tested with"](usage.md#tested-with):
recordings made over the network and over USB, with MDPP-32 modules with
SCP and QDC firmware, made with mvme 1.16 to 1.18, and files that mvme
wrote live during a run. Before FLIRT's files are trusted for any other
setup, that setup should be checked the same way.

## Input integrity, strict by default

Every conversion checks its input. If a check fails, FLIRT publishes
nothing and exits with code 1. The checks are:

- I1: the archive and its central directory (the ZIP archive's table of
  contents) can be read.
- I2: the CRC-32 checksum and the length of the listfile entry equal what
  the central directory records, and there was no read error. FLIRT
  computes them over exactly the bytes the replay read, in its single pass
  over the file.
- I3: the same for `analysis.analysis`, when that is the analysis in use.
  This includes `--default-filters`, which takes its multi-event settings
  from it. When another analysis is in use, a failed I3 is only a warning.
- I4: the readout parser reported no exception and no error. This is only
  a **warning**. Once I2 has shown that the bytes are the ones recorded,
  parser errors describe the recording itself, and mvme exports such runs
  as they are. Runs with such buffers were among those tested, and
  FLIRT's files for them are identical to mvme's.
- I5: the size, modification time (mtime), device and inode number of
  every input are the same at the end of the conversion as at the start.
- I6: no output path leads to an input.

A check that could not be done (`unavailable`) counts as failed, because
the data could not be compared with the archive's own record.
`--accept-damaged-input` publishes the outputs despite a failed I2, I3 or
I5, with the status `accepted-with-failed-checks`. After a read error,
the outputs hold the part that was read, as mvme would export it. The
option cannot override I1, I6, a preamble that cannot be read, or an input
that is refused.

Listfiles compressed with LZ4 are refused. In them, the ZIP entry's CRC-32
covers the LZ4-compressed data, not the data the reader delivers, so I2
cannot check them at the ZIP level. Counters that describe the recording,
such as lost packets, buffers with errors or empty stack frames, are
warnings about the data quality, never failures.

Limits: I2 cannot see damage done before mvme computed the CRC-32, a
change made together with a new CRC-32, or a damaged entry whose CRC-32
still matches by chance (1 in 2^32).

## Several VME events

`refuse_several_events()` (in `vme_config.cc`, called right after the VME
config is read) counts the VME events that have at least one enabled
module. If there is more than one, the run is refused as `unsupported`,
with the reason "the run has several VME events". Every run FLIRT was
tested with has exactly one such event, and no conversion of a setup with several events
was ever compared with mvme's files, so FLIRT refuses instead of guessing.
An event without an enabled module, for example one left over in the
config, does not count. flirt-info still reads such runs.

## Publishing, the report last

Each run is written into a private folder named `.flirt-*` inside the
output folder. After the files are closed, every output is flushed to the
disk: the raw and histos files, the waveforms file and the analysis that
`--extract-hit-flags` writes. This is done by `flush_to_disk` in
`root_output.cc`, with `fsync`, and on macOS with `F_FULLFSYNC`, because
`fsync` there can leave the data in the drive's cache.

Then the report is made from the outputs' final sizes and written with
`O_EXCL`, which fails if the file exists already. Next, the outputs are
linked into the output folder with `link()`, which never replaces an
existing file, and the folder is flushed. The report is linked **last**,
and the folder is flushed again. So if a report is in the output folder,
the whole set of files is there and on the disk, even after `kill -9`, a
kill by the system when memory ran out, or a power loss. If any link
fails, the links already made are removed again.

A stop request is followed until publishing starts. Once publishing has
started, the run finishes. If FLIRT is killed while it publishes, outputs
can remain without a report. The next conversion then refuses them
(`outputs-exist`, "they have no report ...") and names the files to
remove.

ROOT reports a failed write only by printing a message. So FLIRT's
writers check `TFile::kWriteError` after writing and after closing each
file. A full disk is therefore a failure, never a cut-off file with the
status `ok`.

## The batch

When `flirt-convert` gets several inputs, it acts as a parent process that
starts one child process per listfile. The child is the same program,
started with `--convert-one`. So each conversion starts with a fresh ROOT,
with nothing left over from the last run (the compiled classes,
`gRandom`), and a crash affects only its own run.

The inputs can be listfiles, folders and `--list` files. From a folder,
FLIRT takes the `*.zip` files directly inside it (not in its subfolders),
in natural order, so that `run2` comes before `run10`. Before the first
child starts, every run is sorted into one of four groups: convert, skip,
outputs exist, or refused. Two inputs with the same run ID are refused
before anything starts, because their outputs would have the same names.
`-j N` runs at most N children at once. The default is 1, so that only one
run is converted at a time next to a running DAQ (the data acquisition
system).

**The skip rule.** A run is skipped only if all of the following hold.
Otherwise it is refused as `outputs-exist`, with the first difference
named, and nothing is overwritten.

- Its report says `ok`.
- The report's build equals this one: `flirt.version` (from
  `git describe`), the mvme and ROOT versions, and the platform,
  `linux-x86_64` or `macos-arm64`. The platform is included because the
  two platforms can differ in `fTsumwx2`.
- For a development build, also the sha256 checksum of the program
  itself. A development build has an empty version or one that ends in
  `-dirty`, and every uncommitted change of one commit has that same
  version.
- The input's size and mtime.
- The analysis, which for an `--analysis` file is its sha256 checksum.
- Every option that changes the output.
- The outputs are present, with the sizes the report records.

The input's sha256 checksum is not compared, because that would read
every input again at each new run of the command.

**Progress and output.** The child writes lines `progress <bytes> <total>`
to a file it gets as its file descriptor 3 (`--progress-fd`). The parent
reads that file with `pread` and, on a terminal, draws one line that it
keeps updating, with an estimate of the time left in which each run
counts by its input size. A file is used instead of a pipe so that a
parent that died never blocks a child or makes it get a signal.

Text for people goes to stderr: one block per run, when the run ends. The
parent makes it from the report and from its own data, never from the
child's text. stdout carries only the `--json-events` lines. Their
`result` and check IDs are what programs may rely on, while the plain
headlines may be reworded. The exit codes are:

- 0: every run was converted or skipped;
- 1: a run failed, or its outputs exist;
- 2: a usage error;
- 3: a run is unsupported;
- 130: the batch was stopped.

flirt-convert and flirt-tui take a failure's headline and what to do from
one table, `src/common/failures.cc`, so both say the same in the same
words. I4 is only ever a warning.

## Stop signals

SIGINT, SIGTERM, SIGHUP and SIGPIPE stop a batch. Nothing new starts, and
the running children are stopped and publish nothing. The parent always
sends the children SIGTERM, because ROOT ignores SIGINT while it writes.

SIGHUP comes when the terminal or the ssh connection is closed. SIGPIPE
covers `ssh host flirt-convert` run without a terminal: it gets no SIGHUP,
only a failed write when it next prints. Both are followed only if they
were not ignored when FLIRT started. A batch that lost its terminal would
go on unseen and could run at the same time as a second run of the same
command. So a dropped connection stops it: log in again and run the same
command, and the runs already converted are skipped. FLIRT prints a line
only when a run starts or ends, so after a SIGPIPE, the run that was
converting when the connection dropped still finishes and is published
first.

## Nothing written outside the FLIRT folder

FLIRT writes only into its own folder (flirt-tui's settings) and what the
user names: the output folder, `--summary` and `--csv`. The child's log
and progress files are temporary files in the output folder
(`temp_file_in`, in `batch.cc`). Where the file system supports it, they
are made with `O_TMPFILE`, so they never have a name. Elsewhere, such as
on macOS and some bind mounts, they are made by `mkstemp` as
`.flirt-tmp-XXXXXX` and their name is removed at once. They never go to
`/tmp`. If they cannot be made, the run fails and says why. A hard kill
can leave an empty `.flirt-tmp-*` file behind.

## Standard names

A name FLIRT handles may use only ASCII letters, digits, `_`, `-`, `.` and
the space. It must not be empty or consist only of dots. It must not
start with `-`, because it would read as an option, and it must not start
or end with a space, because ROOT and the shell remove such spaces. Any
other name is refused, and the message names the first character that is
not allowed (`nonstandard_name_reason` and `name_refusal`, in
`common/names.h`). ROOT reads `#`, `?`, `$`, `~` and others as parts of a
URL or as variables (see "Names ROOT reads as a URL or expands"), and one
plain rule is easier to follow than a list of exceptions.

Only the name of the path itself (its last part) is checked, never the
folders above it, so that iCloud's folder `com~apple~CloudDocs` keeps
working. These names are checked:

- each listfile, given on the command line, in a folder, in a `--list`
  file or found by `--watch`;
- the output folder: the last part of its real path, so that `-o .`
  checks the folder's real name;
- the files given to options;
- flirt-info's inputs and its `--csv` file;
- flirt-tui's `-o` and the names in its file browser.

Names inside an archive and the files FLIRT writes are not checked. A
wrong name for an output folder or an option's file is a usage error
(exit code 2). A wrong listfile name refuses that run as `unsupported`, in
`classify()`, before its outputs are looked at, and the batch goes on. A
watch reports it once for each version of the file.

## Names ROOT reads as a URL or expands

ROOT 6.36 reads the name given to `new TFile(name, "recreate")` as a URL.
It removes leading and trailing spaces and some protocol prefixes, and it
cuts the name at `?` and at `#`. `gSystem->ExpandPathName` then replaces
`$VAR`, `${VAR}`, `$$` and a leading `~`, and removes every `)` when the
name holds any of `~*[]{}?$`. `TCling::AddIncludePath`, which tells ROOT's
interpreter where to find headers, does the same. Before this design,
`run#42.zip` wrote both its files to `<staging>/run`, a `$` broke the
compiling of the classes, and an output folder `out#1` made the waveforms
file replace an unrelated file `out` next to it.

Decision: ROOT and its interpreter never get a user's file or folder name.

- Each ROOT file is created from inside the private folder
  (`ScopedChdir`), under the name `root_safe_name(name)`
  (`flirt/root_paths.h`). It turns `# ? : ~ * [ ] { } $` and leading or
  trailing spaces into `_`. `publish()` then links the file to its real
  name, and every message uses the real name.
- The generated header is found through the working directory, not
  through an include path.
- Files are read through `/dev/fd/<fd>`, where `<fd>` is a file descriptor
  that FLIRT opened itself. This is done by the dither offset finder and
  by flirt-waveforms.

Since "Standard names" refuses such names, this design remains as a
second line of defence, for the folders above. The one path ROOT still
gets is the install folder's `include/`. If ROOT's expansion would change
that path, FLIRT refuses, with a message to install FLIRT elsewhere.

## Text that is not valid UTF-8

The JSON library FLIRT uses, nlohmann json, throws an exception when it
writes text that is not valid UTF-8. So every JSON that FLIRT writes or
sends goes through `dump_valid_utf8` (`common/utf8.h`): the report,
`--summary`, `--json-events` and flirt-tui's settings. It replaces each
invalid byte by `?` at the moment of writing.

Files keep their real names. The input is opened by the bytes of its
name, with POSIX calls. `QString::fromStdString` is not used, because it
would turn such a name into a different one. The run ID comes from the
real path. When flirt-tui shows a terminal command, it writes a word that
holds a control character or invalid UTF-8 as `$'...'`, with `\xHH` for
those bytes, so that the command, pasted back into a shell, has the real
bytes. Since "Standard names" refuses such file names, this applies
mainly to titles and to the folders above.

## Threads

`--threads T` (default 1) gives the same files for any T. This was
checked with 1, 2, 4 and 8. When T is more than 1, FLIRT turns on ROOT's own
threads with `ROOT::EnableImplicitMT(T)`. ROOT then compresses the baskets
(the blocks in which a branch's data is stored) in parallel, with the
same settings. Only the order of the baskets in the file changes, not the
layout that `compare` checks.

When a waveforms file is written or the analysis has waveform sources, T
worker threads decode the MDPP samples, one readout buffer per job. Each
worker has its own a2, which holds only those sources. Their results are
merged back by data source index, in the order of the buffers.

Every other source runs on the producer thread (the thread that parses
the buffers the replay reads, and extracts), event by event, because a value written
can depend on its source's random sequence. a2 adds a random number from
each source's own generator, a pcg32 (`a2.h:133,144,203`), and for a
large value, adding it can round the sum up before the value is rounded
down to an integer. Which waveform values are valid, and so which dither
numbers they take, does not depend on that random number
(`a2.cc:862-895`). Only the consumer thread, which writes the ROOT files,
uses ROOT's `gRandom` and ROOT's interpreter.

The default stays 1 so that a conversion uses little of a computer that
is also running a DAQ. The effect of more threads on a real DAQ has not
been checked yet.

**Processor use.** Each conversion runs a thread that reads the listfile,
one that computes its checksum, one that converts, one that writes the
ROOT files, one more for the waveforms file when there is one, and T
threads for compression and waveform decoding. Measured on Linux
emulated on a Mac, one conversion used 1.2 to 2.1 processor cores on
average with `--threads 1`, and 1.4 to 5.3 with `--threads 4` (the CPU
time divided by the time the conversion took). The highest use at any
one moment was not measured.

## Waveforms with any analysis

The waveforms file (`<runId>_waveforms.root`) is made from the listfile.
The analysis's decoders never decide what it holds. A module gets a tree
in it if all of these hold:

- it is enabled;
- mvme's `get_decoder_function(typeName)` supports its type
  (`mdpp_decode.cc:352-384`; FLIRT calls it instead of keeping a copied
  list);
- its sampling is on, or unknown, in the VME config;
- its event has a data source in the analysis. Otherwise mvme sends no
  data for the event (`event_server.cc:500-507`), and a trace would have
  no entry to belong to.

Each module is decoded once. If one of the analysis's sample decoders of
the module's type reads it, FLIRT reuses that decoded event, since it is
the same function on the same data. Otherwise FLIRT calls mvme's decoder
itself, as a2 would (`a2.cc:779, 869`). FLIRT's own calls are not data
sources: they send no data to the event server, take no dither numbers
and make no branches. So the waveforms file never changes `_raw.root` or
`_histos.root`, whatever the analysis. The traces are the decoder's
output before a2 adds its random number, so they do not depend on the
number of threads. A separate writer thread fills the trees, because
ROOT compresses the `samples` branch on one thread only.

Limits: there is no tree for a module type that mvme's decoder does not
support (for example `mdpp16_csi`), for a disabled module, or for an event
without data sources. Only channel 0 has an official counterpart to
compare with.

**Cost.** Measured on a large test run with traces in most events, on
Linux emulated on a Mac, whose waveforms file was nearly twice the size
of its raw file: writing the waveforms file made the conversion about
8 % longer at `--threads 1` and about 11 % longer at `--threads 4`, with
about 185 MB and 270 MB more memory. With `--default-filters`, where
FLIRT decodes the samples itself, the conversion took about 2.6 times as
long at `--threads 1` and about twice as long at `--threads 4`.
`--official-waveform-branches` made it about 27 % longer.

**Traces whose length differs from their header.** Some traces have
`nsamples` not equal to 2 × `length`; the report counts them per tree as
`tracesOtherLength`. They come straight from mvme's decoder. Two kinds
were found in test recordings:

- *Two or more hits of one channel in one event.* The decoder starts a new
  trace only when the channel number changes (`mdpp_decode.cc:250-265`),
  so a second hit of the same channel is appended to the first, and its
  header word is decoded as two samples: only a trace's first sample word
  is taken as its header (`:275`), later ones are samples (`:297-312`).
  Such a trace has `nsamples = h × 2·length + (h − 1) × 2` for h hits.
- *Traces cut short.* These had fewer samples than the header says, were
  the last trace of their module event, and had `timestamp` 0: the
  module's data for that event ended inside the trace. In each case, the
  next event had no trace of that module, and the run had exactly as many
  decoder error messages (`waveformDecoderMessages`) as such traces. This
  points to the rest of the trace arriving at the start of the next
  event's data, which the decoder rejects as a whole
  (`mdpp_decode.cc:269-273`). The raw data words were not inspected.

FLIRT writes these traces as mvme's decoder returns them. Only channel 0
could be compared with mvme's own output, the only channel mvme exports:
for MDPP-32 SCP recordings it showed no difference. The traces of MDPP-32
QDC modules were not compared with anything, and no MDPP-16 run was ever
converted.

## Waveform sources left out

By default, FLIRT leaves the analysis's MDPP sample decoder sources out of
`_raw.root` and `_histos.root`. Official mvme writes only channel 0 of
them, cut to 8 bits, while the waveforms file has all the traces. They are
still decoded and sent through the event server stream as mvme does, and
each of their values takes its dither number, so every other value stays
bit-identical.

The files then lack those branches, class members and histograms, and the
basket layout differs, because ROOT sizes the baskets from all branches
together. So `compare` needs `--ignore-branch-regex _samples` for them.
The report lists the sources left out and says
`matchesOfficialLayout: false`. `--official-waveform-branches` writes
them as mvme does.

## Waveform config check

This is a second, independent check of where waveforms are expected. It
gives warnings only and changes no output.

For each MDPP module, FLIRT works out what its init scripts set, as mvme
runs them at the DAQ start: a reset, then the init scripts in order
(`vmeconfig_to_crateconfig.cc:415-440`), each with fresh variables
(`vme_config_scripts.cc:40-81`). FLIRT understands a part of mvme's script
language: `set`, `write <amod> <dwidth> <addr> <value>` and
`<addr> <value>`, with mvme's comments, `${}` variables and number
formats. Sampling is bit 4 of the last value written to register 0x6044,
and off if 0x6044 is never written. The number of samples is the value
written to 0x6148 (unknown, and not compared, if different values are
written). A value that needs `$(...)` or an undefined variable is
unknown, and a note gives the reason.

Then, for each module, FLIRT warns when:

- W1: sampling is on but the module has no tree, with the reason;
- W2: sampling is on but no traces were recorded;
- W3: an analysis decoder found traces although sampling is off;
- W4: fewer than half of the traces have the configured length.

Limits: other commands, `writeabs` and blocks are ignored, and addresses
must match exactly (the module's base address is not taken into account).

## Run information

This is new code: mvme never looks back at a run. One builder (`run_json`,
in `run_info.h`) makes the report's `run` section for both flirt-convert
and flirt-info. Both count in the same way, with `RunRecorder` on the
parser's callbacks, during the same replay.

Times come from the sections of the listfile that carry a time stamp:
BeginRun, a timetick about once a second, Pause, Resume and EndRun.
Without BeginRun or EndRun, the first or last timetick is used. Times
before 1970 or later than 2^40 seconds come from a damaged section and are
ignored, so the summary never makes a conversion fail. The run notes are
left out, because mvme keeps the editor's text from one run to the next.

- *Minutes* (`run_timeline.h`): for each minute, the seconds the DAQ was
  running, the events and the lost packets. What is counted between two
  sections belongs to the second of the earlier one. The median rate is
  taken over the minutes with at least 30 running seconds.
- *Low rate*: minutes with at least 10 running seconds and a rate below
  15 % of the median. Low minutes in a row form a period, which needs at least one such
  minute with 30 running seconds. FLIRT warns, and never says that the
  beam was off, because it cannot know why the rate was low.
- *Rate change*: the median of up to 12 minutes before a minute and the
  median of up to 12 minutes from it differ by at least 25 %. At least 6
  usable minutes are needed before, so that the settling after the start
  is not taken for a change, and 3 after. The change is placed at a
  minute that, with the next two, is past halfway to the new rate, while
  the minute before it is not; among such minutes, at the one with the
  largest change. A 12-minute median moves only once 7 minutes have the new
  rate. Short excursions and slow drifts do not count as changes. The
  parameters are in `rate_rules`.
- *Loss*: lost packets are the gaps in the packet numbers that the parser
  sees. A burst is a loss between two delivered events. The number of
  events lost is estimated as lost packets × events / packets received +
  bursts, because each gap also drops the event that was being assembled.
  This is an estimate, never checked against a count.
- *messages.log*: mvme's version and system, jumbo frames and the network
  receive buffer, from their first appearance in the log. When packets
  were lost and the receive buffer is smaller than the 10 MiB that
  mesytec-mvlc asks for (`mvlc_impl_eth.cc:83`), the summary says so.
- *Warnings*: at most two lines. Each names at most three times, then
  `and N more` and where the full list is. The full lists are in the
  report. Pauses are facts, not warnings. mesytec-mvlc's parser warnings
  go to a log that only counts them (`library_log.*`), never to stdout.
  The report has the count and the first 100 texts.

## Settings changes

`messages.log` is mvme's log from the DAQ start to the stop, at most
100,000 lines. FLIRT reads it with its CRC-32 checked, as for
`analysis.analysis`. Between mvme's lines `Entering readout loop` and
`Leaving readout loop`, each of these is a settings change:

- a script run whose result lines show a successful write, in the exact
  form of mvme's `format_result` (`vme_script_exec.cc:460-611`);
- a script run that mvme only sums up, as the Trigger I/O editor's are
  (the number of commands, or an error, with the writes not listed);
- a line `VME Debug: write ..., write ok`.

Error and warning lines never count. These patterns are the same in mvme
1.16.2, 1.18.0 and 1.21.0.

FLIRT checks that it can trust the result. If the log is not valid
UTF-8, lacks either of the two lines, or shows no successful write before
the readout loop (every DAQ start writes the module settings), the result
is "not checked", with the reason, and never "no changes".

FLIRT cannot see the "Run script" of mvme's VME Debug window, the single
writes of the script runs that mvme sums up, or anything done outside
mvme. This has not yet been seen on a real run with a change. It was
tested with mvme's log lines inserted into a real log.

The report's `run.settingsChanges` holds `checked`, `reason`,
`mvmeVersion` and `changes`. Each entry of `changes` has the log's
`time` (by the DAQ computer's clock), its `source` (`script "<title>"`,
`script` or `VME Debug`), the `writes` (`address` and `value`, as the
log's hex text), `writesListed`, `commands` and `error`. The run's status
stays `ok`, because the physics outputs do not depend on this check.

## Reading several listfiles at once

flirt-info with several inputs, and flirt-tui when it reads its rows
ahead, read several listfiles at once. The number at once is
min(floor(cores / 2), floor(idle cores)), and never less than 1
(`common/cores.*`). Cores are the processors this process may use. Idle
cores are cores × the share of processor time that was idle during a
short measurement (from `/proc/stat` on Linux, `host_statistics` on
macOS), with FLIRT's own running reads added back.

The load average is used only when the processor time cannot be read:
on macOS it read 8 to 10 on 10 cores that were 60 % idle. So FLIRT uses at most half the machine, and less when the
machine is busy, as a DAQ computer is during a run.

The reads are separate processes, not threads. mesytec-mvlc's parser
takes a lock on a shared logger for every network packet, which would
make threads wait for each other, and its loggers exist once per process.
flirt-info prints the children's results in the order the inputs were
given, so its output is byte for byte the same as reading them one after
another. In flirt-tui, the row under the cursor never waits. While a
conversion is running, no new reading ahead starts.

## Watch mode

mvme writes a run's ZIP archive with the central directory last, when it
closes the run (`mvlc_listfile_zip.cc:424-433`). So the watch
(`watch.cc`) looks at the folder every 2 seconds, using only `readdir`
and `stat`. A file is ready when its inode, size and mtime have not
changed for `--settle` seconds and its central directory can be read. A
recording that is still running, or that crashed, has no central
directory and is reported once.

A ready file goes through the same skip and refuse rules as in a batch.
Each file is converted once, and again only if it changes. If a file
that was converted changes, the new attempt meets `outputs-exist`,
because its outputs are there already. A failed file is not tried
again until it changes or the watch starts again. The watch keeps no
state on disk and writes nothing in the watched folder. An output folder
(`-o`) inside the watched folder is a usage error. FLIRT compares the
real paths to find out.

Before it starts each conversion, the output disk must have 3 × the
listfile's data free. In tests, the outputs were at most about 1.7 × the
data, more for tiny runs, and mvme's next run
may be written to the same disk. If the space is not there, the watch
pauses. A run stopped by a full disk is tried again up to 3 times.

A stop is the normal end of a watch. Its exit code is 1, 3 or 0, from
each file's last result, never 130. Runs split into several files are
refused, because mvme replays the parts as one run. Limit: the watch was
tested with a pretend DAQ only, not on a real DAQ computer.

In the `--summary` JSON, `options.watch` and `options.settleSeconds`
record the watch (null in a batch), and `totals.unfinished` counts the
files mvme had not finished when the watch was stopped. Those are listed
as `unfinished`, and the runs still waiting as `not-started`.

## Finding a file's dither offset

The dither offset is the number of random numbers that `mvme_root_client`
had used before it converted the file. A value's fraction × 2^32 is the
TRandom3 number that `mvme_root_client` added, because `Rndm()` returns
y × 2^-32 for a 32-bit integer y. This is exact when the value's absolute
size is below 2^21.

FLIRT reads the first hits of the file's one event tree in the order in
which the client filled them: every element that is not NaN, of every
`Double_t` leaf, depth first (`mvme_root_client.cc:974-1016`). It takes up
to 24 of these values that have a fraction, called anchors. It looks for
the first three anchors (96 bits together) in one pass over the numbers
of `TRandom3(4357)`: a quick test on the third, and a ring of the most
recent numbers for the first two. Then it confirms a match with the other
anchors. The offset is the position of the first hit's number in the
sequence. All the files given are searched in the same pass, over up to
2^32 numbers, which takes about a minute at most. Offsets met in
practice are far below this limit.

When no offset can be found, FLIRT gives a plain reason: no event tree,
no `MVMERunInfo`, several event trees (the file does not record the order
in which they were filled), no hits, no value below 2^21, no fractions
(the file was not dithered), fractions that are not TRandom3's, too few
anchors, or not found. A file that FLIRT made gives the offset it was
made with, if it holds every source. One whose waveform sources were left
out (see "Waveform sources left out") gives no offset, because their
values took dither numbers that the file does not show. A file that holds the same run twice gives the offset of the
first copy, with a note. ROOT opens the file only as `/dev/fd/<fd>`.

`--dither-offset-from` reads the official file in a helper process
(`--official-raw-facts`). This is needed because reading the official file
creates classes from the file's own descriptions, with the same names as
the classes the conversion compiles. The file must be of the same run:
its `RunID` must be the listfile's (or, for a file written live, the
run's start time, with a note). After the conversion, before publishing,
its trees, modules and entries must match. Otherwise the run is refused
and nothing is published. The skip rule then also compares the official
file's path, size and mtime.

**Speed.** The search goes through the list at about 75 million numbers a
second on Linux emulated on a Mac, and 165 million on a Mac with Apple
silicon. For a file whose offset is about 200 million, it takes about 5 s
and 2 s in all, with the start-up; for one whose offset is not in the list, about 56 s and 26 s,
because it goes through all 2^32 numbers.

## Hit flags

mvme's filters can match only bits fixed to `0` or `1`
(mesytec-mvlc's `util/data_filter.cc:46-55`), so the analysis never
extracts the pile-up and over/underflow bits of an MDPP amplitude or
integral word. mvme's manual describes getting at them by editing the
analysis's filters (`doc/sphinx/source/analysis.rst:536-571`).
`--extract-hit-flags` makes that edit on the analysis JSON in use, before
it is parsed. The rest of the conversion is unchanged.

For each extractor of an `mdpp32_scp`, `mdpp16_scp`, `mdpp32_qdc` or
`mdpp16_qdc` module whose filter has the fixed bits of mvme's template,
FLIRT appends a copy named `<name>_flags`. The copy:

- extracts only the flag bits: for SCP the value
  2 × pile-up + over/underflow, for QDC the overflow bit;
- gets a UUIDv5 id derived from the original's id, so the same input
  always gives the same bytes;
- has no parameter names, because copied names would clash in the
  generated class;
- comes last among its module's extractors, by a2's stable sort.

The edited analysis is published as `<runId>_hit_flags.analysis`. Official
mvme, given that analysis, writes identical files.

Consequence: every extra source takes one `gRandom->Uniform()` number per
hit. So the stored values of all branches differ from those of a file made
without the option, while their integer parts and the hit masks stay
equal. mvme's event server has no way to avoid this for one source only.
Compare such files with official files made from the saved analysis.

## Compression

`--compression` sets the compression of the three ROOT files from one
table (`common/compression.cc`):

- `mvme` = 101, ZLIB at level 1. This is TFile's default, which
  `mvme_root_client` gets; a `static_assert` makes sure it stays so.
- `fast` = 501, ZSTD at level 1.
- `small` = 509, ZSTD at level 9.

It is set on each TFile before any tree or histogram exists, because a
branch takes the file's setting when it is created.

The candidates were measured. ZSTD 1 was as fast as LZ4 1, with much
smaller files. ZSTD 3 to 8 gave files no smaller than ZLIB 1 on a typical
run, whose values carry a random fraction and hardly compress. ZSTD 9 was
the only clearly smaller one, at 6 to 17 times the write time. LZMA was
far slower to write and to read.

Only `mvme` gives identical files in every case. In a larger run, the
others change when ROOT writes a basket and how large the baskets are,
because ROOT derives both from the compressed bytes when it first flushes
a tree (the small example is too small to be flushed, so its layout is the
same either way). So those files are checked with
`compare --data-only`,
which compares every value and histogram bit for bit, but not the layout.
The choice is part of the skip rule. ZSTD file sizes differ slightly
between Linux and macOS, which have different builds of the libzstd
library.

**Measurements.** On two test runs, a medium-sized one without waveforms
and a large one with waveforms, natively on a Mac (Apple M1 Max, internal
SSD) and on Linux emulated on that Mac, each figure the middle one of 3
tries, compared with `mvme`:

| | `fast` | `small` |
|---|---|---|
| conversion time, the run without waveforms (`--threads 1`) | 38-48 % less | 6 to 9 times as long |
| `_raw.root`, the run without waveforms | 14-16 % bigger | 17-27 % smaller |
| conversion time, the run with waveforms (`--threads 4`) | 4-10 % less | about 17 times as long |
| `_raw.root` + `_waveforms.root`, the run with waveforms (Mac) | about the same | 29 % smaller |
| reading every entry of both files, the run with waveforms (Mac) | 5 % faster | 11 % faster |

A run with a waveforms file spends most of its time decoding the traces,
so `fast` gains it little time, but it uses 10-15 % less CPU time. On the
small example, `fast` made the raw file 12 % bigger with the default
options (3.6 MB instead of 3.2 MB), but smaller with
`--official-waveform-branches` (7.5 MB instead of 9.5 MB).

## macOS (Apple silicon)

FLIRT builds natively on macOS on Apple silicon (arm64), from the same
sources, unmodified:

- liba2's `util/bits.h` includes `<x86intrin.h>`, a header that exists
  only for x86 processors, without a guard, but uses it only when
  `__BMI2__` is defined. On other processors, an empty stand-in
  (`cmake/non_x86_include/`) comes first on the include path.
- mesytec-mvlc has no USB code for macOS. Its Linux USB file is compiled
  with stand-ins for the 9 FTDI functions it calls, which always answer
  "no device" (`src/macos/ftd3xx_stubs.cc`). FLIRT only replays listfiles,
  so it never needs a device.
- Calls that exist only on Linux are wrapped in `common/platform.*`: the
  program's own path, waiting for signals (through kqueue on macOS),
  `st_mtimespec`, and `pipe` + `FD_CLOEXEC`. That last one is not atomic.
  A program started from another thread between `pipe()` and `fcntl()`
  inherited the pipe. This once kept a helper's stdout open and made
  flirt-tui hang when it quit. So on macOS, `posix_spawn` gets
  `POSIX_SPAWN_CLOEXEC_DEFAULT` (`src/ui/job.cc`, `src/ui/spawn.cc`).

**Floating point.** On x86-64 without FMA (fused multiply-add), `a*b + c`
rounds `a*b` before adding `c`. Clang for arm64 fuses such expressions by
default into one instruction that rounds only once. FLIRT's CMake adds
`-ffp-contract=off` on processors other than x86 for everything it builds,
and the raw trees are then bit-identical to Linux.

What remains is inside ROOT. Its `TH1::Fill` does `fTsumwx2 += x*x`,
and the arm64 build of ROOT does that with one fused multiply-add. So in
some histograms, `fTsumwx2` (the sum behind the RMS) differs by one or a
few units in the last place (ulp). Everything else is identical. This is
accepted, but proven for each histogram: `compare --allow-fused-sumwx2`
recomputes both sums from the candidate's raw tree in the order of
filling (`fused_sumwx2.cc`, itself
built with `-ffp-contract=off`). It accepts a histogram only if one file
holds exactly the fused sum and the other exactly the separate one. No
tolerance is allowed. The platform is part of the skip rule for the same
reason.

## TUI

`flirt-tui` is FLIRT's terminal user interface (TUI), built with FTXUI
6.1.9. It runs `<its own folder>/flirt-convert --json-events` as a user
would (`src/ui/job.*`) and shows the run states that `job.cc` derives from
the events. It decides nothing about a run itself, so it converts exactly
as the command line does.

Helper programs keep ROOT and mvme out of flirt-tui's own process:

- `flirt-info --json` for a run's details;
- `flirt-waveforms` for the waveform view;
- `flirt-convert --find-dither-offset --json-events` for "From a file…".

Each runs in its own process group, with stderr on `/dev/null`, and
stdin too except for flirt-waveforms, which reads its requests there
(`spawn_helper`), so a damaged file cannot make flirt-tui crash.
`flirt-convert` does not depend on any of this.

Drawing is kept apart from the handling of input. `view.cc` turns the
state (`Model`) into what is drawn, and does nothing else. `input.cc`
turns a key into a change of the state. FLIRT uses FTXUI's plain
elements, not its components. The focus (a box, and in the options form
an item) is kept in the model, and text fields are edited in the model:
Enter or Tab keeps the text, Esc cancels. Other threads send work to the
screen's loop only through a gate that the loop closes before it exits,
because in FTXUI 6.1.9, `Post` can run at the same time as `ExitNow()`
and fail. Text for users never names a terminal multiplexer,
so that none looks required.

### Layout

W and H are the terminal's width and height. The layout is:

- a left column L = clamp(W × 2/5, 42, 72) wide, that is W × 2/5 but at
  least 42 and at most 72. It holds box 1 (H − 12 rows, with the tabs
  Listfiles and Waveforms) over box 2 (8 rows: Options, or Plot options
  in the Waveforms tab);
- box 3 on the right, W − L wide and H − 4 high, which shows what the
  chosen left box holds;
- the bottom box (3 rows) with the conversion;
- the key bar last.

The layout is computed at every draw from the terminal's size
(`layout()`, shared with the input code). 80x24 is the smallest size it
was designed for.

The box with the focus has a heavy cyan border, with its number on a cyan
badge. The left box that box 3 shows is magenta, and the others are dim.
The colours are the terminal's ANSI colours, so the user's colour scheme
decides the shades. Box 3's title names what it shows
(`[3] Listfiles → Details ─ Log`).

The key bar (`key_bar_text`, `fit_items`) shows the keys of the current
context first, then optional ones while they fit, and `? help  q quit`
last. In the details, the Log and the report, `s` stays and
`Tab next box` is the first to give way. While a line is edited, only
Enter and Esc are shown. Every run listed is converted, so there are no
checkboxes. The full window (`z`) shows the plot on the whole screen.

### Remembered settings

The settings are in `<prefix>/settings/flirt-tui.json`, where `<prefix>`
is the folder above the `bin/` folder that holds the running flirt-tui
(found from its real path). Everyone who runs that copy shares them.
`FLIRT_TUI_SETTINGS=<absolute file>` sets another file. This keeps FLIRT
from writing outside its own folder (see "Nothing written outside the
FLIRT folder").

flirt-tui remembers the output folder, the analysis choice, the
waveforms, the hit flags, and the last folder for each mode of the file
browser. It writes the file when one of them changes, atomically: first
to a temporary file, then renamed. It does not remember the listfiles or
any Advanced option, because a remembered dither offset, accept-damaged
setting or experiment name would silently change the output or the
safety of the next session. A file that cannot be read or is broken gives
the defaults, and flirt-tui says so once. If the file cannot be written,
flirt-tui works on without remembering, and says so once.

### Waveform view

`flirt-waveforms` takes no arguments and talks in JSON lines on stdin and
stdout. It reads the waveforms file with ROOT in its own process, opened
as `/dev/fd/<fd>`, so flirt-tui links no ROOT. For each tree, it sends a
`typical` range, from the first 20,000 traces: the 1st percentile of their
minima, the 99th percentile of their maxima, and the most common length.
It also sends frames. A frame is the run of entries with the same
`event`, plus up to N − 1 earlier frames for the Traces setting.
flirt-tui only rounds and draws.

One y range holds for every channel and frame: the typical one, or the
user's own. Stepping or playing never changes it, so frames can be
compared by eye. A trace beyond the range runs along its edge, with its
real extreme value in red.

The grid has one panel per channel, from 0 to the highest channel with
traces + 1, rounded up to a multiple of 4. The number comes from the
traces, not from the module type, because the module type says what the
module can do, not what the setup uses. The panels are
in rows of 4, with one colour per row. The pages are spread evenly over
the rows that fit, with panels at least 3 rows high and 8 columns wide.
Where not even one row fits, the box draws the traces on top of each
other. Traces are drawn as lines of braille characters, with older events
dim, as mvme fades them.

Over ssh (detected by `SSH_CONNECTION`), flirt-tui keeps the traffic
small: at most 100 traces are drawn at once, the number of past events is
lowered for all channels on screen together, and play shows at most 10
events a second. Each file keeps its module, channels, frame, own ranges
and page for the session. None of this is saved.

### Mouse

The mouse is on by default, for people not used to terminal programs.
`--no-mouse` turns it off. A press of the left button acts. Releases and
other buttons do nothing. The wheel scrolls the box under the pointer, or
steps through frames over the plot.

Every part that can be clicked is drawn with `ftxui::reflect` into a box
(`Hits`). At each press, `mouse.cc` draws the current state off-screen,
so the positions are always those of the current layout, and the mouse
handling can be tested without a terminal. An entry of the key bar does
what its key does, through the same code.

A double-click is a second press on the same target within 500 ms. A
press within 500 ms of one that changed the screen, at the same cell, is
ignored, so that a double-click never clicks what appears under the
pointer after the first click.

FTXUI turns on the terminal's "any motion" mouse tracking (mode 1003),
which redraws at every move of the mouse and is heavy over ssh. So FLIRT
writes `\e[?1002h` before each frame instead. That mode reports presses,
releases, the wheel, and motion only while a button is held, which is
needed to drag the play bar. FTXUI itself is not patched.
