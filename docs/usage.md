# Using FLIRT: the reference

This page is for looking things up: every program, option, output file,
message and exit code. To learn FLIRT, follow the
[tutorial](tutorial.md) instead. To build it, see
[building.md](building.md). How FLIRT works inside is in
[design.md](design.md).

The examples come from the example run that comes with FLIRT,
`examples/example_run.zip`, converted on a Mac in a FLIRT folder at
`~/FLIRT`. Some things the example run cannot show, such as network
packets lost by the DAQ or a falling event rate. Those examples are
marked as illustrations: their run names (`run042`, `run043`) and numbers
are made up, but the wording is exactly what FLIRT prints.

Words used on this page:

| Word | Meaning |
|---|---|
| DAQ | the data acquisition: the modules, the MVLC (mesytec's VME controller, which reads the modules out) and the computer that runs mvme |
| listfile | the `.zip` archive mvme writes for a run. It holds the recorded data, the VME config (how the modules are set up and read out), the analysis (what mvme extracts from the data) and mvme's log of the run |
| run ID | the listfile's name without `.zip`. FLIRT names a run's files after it |
| official files | the ROOT files that mvme and its program `mvme_root_client` make from the same run |
| report | the `<runId>.flirt.json` file FLIRT writes next to a run's ROOT files: what was done, and every check with its result |
| stderr, stdout | a program's two output channels. Both show in the terminal, but they can be sent to different places (`2> log.txt` sends stderr to a file) |

## Contents

- [What FLIRT promises](#what-flirt-promises)
- [The programs and where they are](#the-programs-and-where-they-are) and [Names](#names)
- [flirt-convert](#flirt-convert)
  - [Inputs](#inputs), [Options](#options), [Outputs](#outputs)
  - [What flirt-convert prints](#what-flirt-convert-prints), [Progress](#progress), [Run summary](#run-summary), [Settings changed during the run](#settings-changed-during-the-run)
  - [Skipping and refusing](#skipping-and-refusing), [Input integrity checks](#input-integrity-checks), [What a failure says](#what-a-failure-says), [Exit codes](#exit-codes), [Stopping (Ctrl-C)](#stopping-ctrl-c)
  - [Watch mode](#watch-mode), [Dither offset](#dither-offset), [Default filters](#default-filters), [Waveforms](#waveforms), [Pile-up and overflow flags](#pile-up-and-overflow-flags), [Compression](#compression), [Events for programs](#events-for-programs)
- [flirt-info](#flirt-info)
- [flirt-tui](#flirt-tui)
- [compare](#compare)
- [Differences from the official output](#differences-from-the-official-output)
- [Verifying against official output](#verifying-against-official-output)
- [Tested with](#tested-with)

## What FLIRT promises

**The same ROOT files as mvme.** For the setups it was tested with (see
[Tested with](#tested-with)), every value, hit mask, tree, branch, class
definition and histogram in FLIRT's `_raw.root` and `_histos.root` is
identical to what `mvme_root_client --single-run` writes. Only these may
differ:

- the file's unique ID (UUID) and its time stamps, which ROOT sets anew
  every time it writes a file, and the compressed sizes it records. None of
  these is data, and `compare` does not compare them;
- on a Mac with Apple silicon only: one histogram statistic, `fTsumwx2` (the
  sum of squares behind a histogram's RMS), can differ in its last bits,
  because ROOT itself rounds it once instead of twice there. `compare
  --allow-fused-sumwx2` recomputes that sum from the raw tree and accepts
  exactly this difference and nothing else.

Three things to know when you compare with mvme's own files:

- **Waveform sources.** If the analysis holds MDPP sample decoders
  (waveform sources), FLIRT leaves them out of `_raw.root` and `_histos.root`
  by default, because mvme keeps only their channel 0, cut to 8 bits. Every
  other value stays identical, and all traces go to a separate
  `<run>_waveforms.root`. `--official-waveform-branches` writes them as mvme
  does.
- **The dither offset.** `mvme_root_client` adds a random number between 0
  and 1 to every value it records ("dither"). The numbers come from one
  fixed list, which goes on from run to run for as long as the client keeps
  running. FLIRT starts at the top of the list for every file. So if the
  client had already converted other runs before this one, its file used
  numbers further down the list, and FLIRT must start at the same place to
  match it bit for bit. `flirt-convert` can find that place from the file
  itself (`--dither-offset-from`). For physics it changes nothing: only the
  random part of each value differs.
- **Files written during the run.** `mvme_root_client` can also write its
  files live, while mvme records the run. Such a file names the run after
  the time it started (for example `260303_141005`); FLIRT names it after
  the listfile. That name, stored in the file as `RunID`, therefore differs.
  To compare such files, add `--ignore-run-id` to `compare`, which then
  leaves that name out.

**It never changes a listfile.** FLIRT only reads listfiles. Before it
keeps a conversion, it also makes sure that no output would replace an
input, and that nothing else changed the listfile in the meantime.

**No half-written files.** FLIRT writes each run's files in a hidden folder
inside the output folder. Only when the whole conversion has worked does it
move them into place, and it never overwrites a file that is already there.
The last file it adds is the report, `<run>.flirt.json`. So if a run has its
report, all of its files are complete. If you stop FLIRT with Ctrl-C, the
run it was converting adds no files (a run already moving its files into
place finishes first).

One rare case: if the computer loses power, or FLIRT is forcibly killed, at
the very moment it moves the files into place, some files can be left
without their report. FLIRT never counts those as converted. When you run
it again, it names them so you can delete them and convert the run again.
A crash or a forced kill can also leave a hidden folder of unfinished files
(`.flirt-…`) in the output folder; it is safe to delete.

**It writes only where you tell it to.** FLIRT writes the output folder you
choose and, if you ask for one, a `--summary` or `--csv` file. Apart from
those, it writes only inside its own folder (where `flirt-tui` saves its
settings). Even its temporary files go into the output folder, never
anywhere else on the computer.

## The programs and where they are

| Program | What it does |
|---|---|
| `flirt-convert` | Converts listfiles into ROOT files, from the command line. Scripts can run it too. |
| `flirt-info` | Displays what listfiles recorded about their runs, without converting them. |
| `flirt-tui` | FLIRT's terminal user interface (TUI), for converting listfiles and viewing their waveforms. It runs `flirt-convert` for you. |
| `flirt-waveforms` | A helper that `flirt-tui` starts by itself to read the waveforms in FLIRT's `_waveforms.root` files. It runs as a separate program so that ROOT's own messages never reach the screen, and a damaged file cannot crash `flirt-tui`. You never need to run it yourself. |
| `compare` | Checks whether two ROOT files hold the same data. |

After building, the first four programs are in `build/linux/prefix/bin/`
(on a Mac, `build/macos/prefix/bin/`), and `compare` is in
`build/linux-compare/` (on a Mac, `build/macos-compare/`). Leave them
there: they need the `include/`, `lib/` and `templates/` folders next to
`bin/`. To
run them by name, see [building.md, "Running the programs by
name"](building.md#running-the-programs-by-name). Each program lists its
options when you run it with `--help`.

The commands on this page are written for bash and zsh. If the answer of
`echo $0` contains `tcsh`, type `bash` first.

The full path of the FLIRT folder itself must not contain `$`, `)` or
`;` ([building.md, "If FLIRT cannot use its
folder"](building.md#if-flirt-cannot-use-its-folder)).

### Names

FLIRT accepts only plain names: letters `A-Z a-z`, digits `0-9`, `_`,
`-`, `.` and spaces. A name must not start with `-` (it would be read as
an option), must not start or end with a space, and must not be only
dots. Everything else is refused, for example `# ? $ ( ) * [ ] { } : ; ~
@ ! % & + = , < > |`, quotes, a backslash, and letters with accents or
from other alphabets. ROOT reads several of these characters as special
signs, and one plain rule is easier to follow than a list of exceptions.

The rule is for the name itself, the last part of the path, of:

- each listfile, and each listfile a watch finds;
- the output folder (`-o`; with `-o .`, the current folder's real name);
- the files given to `--analysis`, `--summary`, `--list`,
  `--dither-offset-from`, `--find-dither-offset`, `flirt-info` and its
  `--csv`;
- what you add, choose or open in `flirt-tui`'s file browser.

The folders above them may have any name.

```
run#1.zip: name not accepted: "#" is not allowed (use letters, digits, _ - . and spaces)
```

A listfile with such a name is refused as `unsupported` (exit code 3), and
the other runs are still converted. For the output folder, or a file
given to an option, it is a usage error (exit code 2), and nothing starts.
`flirt-info` lists such a listfile as `unreadable` (exit code 1).
To convert such a run, rename the listfile, or a copy of it.

## flirt-convert

```sh
flirt-convert <input>... -o <outdir> [options]
```

`<input>...` is one or more listfiles or folders of listfiles, and
`<outdir>` the output folder. Some typical commands:

```sh
flirt-convert run042.zip -o out/                  # one listfile
flirt-convert /data/listfiles -o out/             # every *.zip in a folder
flirt-convert --list runs.txt -o out/             # the listfiles named in a text file
flirt-convert /data/listfiles -o out/ -j 2        # two conversions at a time
flirt-convert --watch /data/listfiles -o out/     # convert each run as mvme finishes it
flirt-convert --find-dither-offset official/run042_raw.root   # print an official file's dither offset
flirt-convert run042.zip -o out/ --dither-offset-from official/run042_raw.root
```

The last two are about the dither offset (see [Dither
offset](#dither-offset)).

### Inputs

An input is a listfile or a folder. A folder means the `*.zip` files
directly in it, in natural order (`run9` before `run10`), without its
subfolders and without hidden files (names that start with `.`). A
`--list` file names one input per line; blank lines and lines that start
with `#` are skipped. The runs are converted in the order given, all with
the same options.

### Options

| Option | What it does |
|---|---|
| `-o, --output-dir <dir>` | The output folder (required). It must exist already. |
| `-j N` | Convert up to N listfiles at the same time (default 1). |
| `--list <file>` | Read the inputs from a text file, one per line. Can be given more than once. |
| `--summary <file.json>` | Also write a summary of the whole batch to this new file: each run's result, the totals and the exit code. |
| `--analysis <file>` | Convert with this mvme `.analysis` file instead of the analysis saved in the listfile. |
| `--default-filters` | Convert with mvme's default filters for every module (see [Default filters](#default-filters)). Not together with `--analysis`. |
| `--experiment-name <name>` | The experiment name stored in the files (default `Experiment`). mvme takes it from its workspace, not from the listfile, so give the one your workspace used. |
| `--experiment-title <title>` | The experiment title, likewise (default empty). |
| `--dither-offset <N>` | Start N numbers down mvme's list of random numbers (default 0; see [Dither offset](#dither-offset)). |
| `--dither-offset-from <file>` | Find the offset of this run's official `_raw.root`, and convert with it. One listfile only. |
| `--find-dither-offset <file>...` | Convert nothing: print the dither offset of each official `_raw.root` given. |
| `--threads <T>` | How many threads each conversion uses to compress and to decode waveforms (default 1). 1, 2, 4 and 8 were tested and give files that `compare` finds identical (the bytes can be in another order). |
| `--no-waveforms` | Do not write `<runId>_waveforms.root` (see [Waveforms](#waveforms)). |
| `--official-waveform-branches` | Write the waveform sources into `_raw.root` and `_histos.root` as mvme does, so the files match mvme's in every branch. |
| `--extract-hit-flags` | Add pile-up and overflow flags (see [Pile-up and overflow flags](#pile-up-and-overflow-flags)). |
| `--compression <name>` | `mvme` (default), `fast` or `small` (see [Compression](#compression)). |
| `--accept-damaged-input` | Write the files even if an input check fails (see [Input integrity checks](#input-integrity-checks)). |
| `--json-events` | Print events for programs on stdout (see [Events for programs](#events-for-programs)). Not together with `--watch`. |
| `--watch <dir>` | Instead of inputs: convert each run mvme finishes in this folder, until stopped (see [Watch mode](#watch-mode)). |
| `--settle <S>` | With `--watch`: wait until a file has not changed for S seconds, 1 to 3600 (default 10). |

**While the DAQ is taking data on the same computer,** keep the defaults
`-j 1 --threads 1` (one run at a time, with one thread), and write the
outputs to another disk than the one mvme records to. Whether even that
disturbs a running DAQ has not been tested yet.

### Outputs

For a listfile `<runId>.zip`, FLIRT writes into the output folder:

| File | What it holds |
|---|---|
| `<runId>_raw.root` | the event tree, as `mvme_root_client` writes it |
| `<runId>_histos.root` | the raw histograms, as `mvme_root_client` makes them |
| `<runId>_waveforms.root` | every trace the MDPP modules recorded, when there are any (see [Waveforms](#waveforms)) |
| `<runId>_hit_flags.analysis` | with `--extract-hit-flags`: the analysis with the flag columns added |
| `<runId>.flirt.json` | the report: the status, every check and its result, the input's path, size, modification time and sha256 checksum, the analysis, the options, the FLIRT build, counters, timings, and the [run summary](#run-summary) |

Each run is converted in a process of its own, and its files are written
to a hidden folder (`.flirt-…`) inside the output folder first. Only when
the whole conversion has worked are they written out to the disk and put
in place under their names, with the report last. FLIRT never overwrites
or deletes a file that is already there. If a conversion is killed while
it puts its files in place, the files without their report are never
counted as converted: when you run FLIRT again, it names them so you can
delete them.

### What flirt-convert prints

Everything flirt-convert writes for people goes to stderr. stdout stays
empty, unless `--json-events` is given. flirt-convert names the output
folder once, then prints a block for each run when it ends, then a
summary. The example:

```
Converting 1 run into ~/FLIRT/practice

✓ example_run   converted in 2.2 s
    run       Mar 3, 2026  14:10:00 → 14:10:22 CST  (22 s)
    events    17,207 (782.1 per second)
    files     _raw.root   _histos.root   _waveforms.root   .flirt.json
    note      the analysis's 3 waveform sources are not in _raw.root and
              _histos.root (official mvme keeps only their channel 0, cut to 8
              bits); every other value is as official. The full traces are in
              the run's _waveforms.root; --official-waveform-branches writes
              them as mvme does.

Done: 1 converted in 2.3 s.
```

The mark in front of the run says what happened:

| Mark | Meaning |
|---|---|
| `✓` | converted (`· 2 warnings` is added when the run has warnings) |
| `–` | skipped: already converted the same way; or not started, because the batch was stopped |
| `!` | not converted, because its files exist or it was stopped; or, in a watch, a recording mvme has not closed |
| `✗` | failed, or refused as unsupported |

The rows under it appear only when they have something to say:

| Row | What it says |
|---|---|
| `run` | the start, the stop and the length (see [Run summary](#run-summary)) |
| `events` | the events and their rate. When mvme's multi-event splitting changes the count, both counts |
| `dither` | the dither offset, with `--dither-offset-from` |
| `files` | the files this run wrote, by their endings |
| `lost` | network packets the DAQ lost |
| `quality` | what the DAQ recorded about the data's quality |
| `flags` | pile-up and overflow counts, with `--extract-hit-flags` |
| `note`, `warning` | things worth knowing about the run |
| `why`, `to do` | for a run that did not convert (see [What a failure says](#what-a-failure-says)) |

The last line counts the results, for example `Done: 6 converted in 2 min
35 s. Nothing needs attention.` Under `Needs attention:` follows one line
for each run that failed, was refused, was not converted or was converted
with warnings. In a batch of several runs, the notes come after that line
instead of in the blocks. A note several runs share is said once, as in
`note      For all 6 runs: ...`, and any other names its runs, as in
`note      For 1 of 6 runs (run042): ...`.

Long rows wrap at the terminal's width, or at 80 columns in a log file.
Colours are used only on a terminal, and not when the setting `NO_COLOR`
is set.

### Progress

While a run converts, flirt-convert shows how far it is, on stderr:

- **On a terminal:** one line at the bottom, redrawn at most once a second,
  with the share of the listfile read and the time left. An illustration:

  ```
  ● run042   converting  ━━━━━━━━━━━━━━━━━━━━━━━━━───────────────  64%   about 5 s left
  ```

  It says `starting` before the first percent, and `finishing` while the
  run puts its files in place. With several runs at once (`-j`), the line
  lists them all. The time left appears once 1 % of the batch is done and
  5 seconds have passed.
- **In a log file** (for example after `2> log.txt`): no live line and no
  percentages. A line `● <runId>   converting` is written when each run
  starts, so the log shows what was running if the computer stops, and the
  run's block follows when it ends. A log names the output folder with its
  full path.

### Run summary

For every run, FLIRT works out when the DAQ run started and stopped, how
long it ran, and its counts. They are in the run's `run` and `events`
rows, and in the report's `run` section:

- **Times** come from time stamps the MVLC readout writes into the
  listfile: BeginRun at the start, a timetick once a second, Pause and
  Resume, and EndRun at the stop, by the DAQ computer's clock. A run
  without BeginRun or EndRun (for example one that mvme's listfile
  recovery saved after a crash) takes the first or last timetick instead.
  Times are shown in your computer's time zone, or in the one the `TZ`
  setting names.
- **Fields:** `start` and `stop` (ISO 8601 times, such as
  `2026-03-03T14:10:00-06:00`), `startUnix` and `stopUnix` (seconds since
  1970), `startFrom` and `stopFrom` (which time stamp they came from),
  `durationSeconds`, `pauses`, `pausedSeconds`, `liveSeconds`; `events`
  (per VME event, the events and their rate) and `modules` (the hits of
  each module); from mvme's log: `packets` (for a recording over the
  network, ETH), `receiveBuffer`, `receiveBufferBytes`, `jumboFrames`,
  `mvme`; `rate` and `minutes` (see [flirt-info](#flirt-info)); and
  `settingsChanges` (see [Settings changed during the
  run](#settings-changed-during-the-run)). An unknown value is null.
- The run notes are left out: mvme keeps the text of its notes editor from
  one run to the next, so the notes saved with a run need not describe it.

When the DAQ lost network packets, or the event rate fell for a while or
changed, the run's block says so. An illustration:

```
    lost      12 of 1,073,420 packets (0.0011 %) in 5 bursts, about 18 events;
              in 3 of 63 minutes, at 14:31 (6), 14:52 (4) and 15:03 (2)
    note      the DAQ PC's receive buffer for the MVLC was 208 kB, below the 10
              MB mesytec-mvlc asks for; mesytec's MVLC notes suggest raising
              net.core.rmem_max on the DAQ PC (e.g. to 104857600)
    warning   low event rate 14:31-14:34, 15:02-15:03 (under 15 % of the run's
              median rate, 320 per second): check whether the beam was off
    warning   the event rate changed at 14:45 (320 → 210 per second)
```

The warnings are hints. FLIRT sees that the rate fell or changed, but not
why: the run's log book or the people on shift can tell. [The rate over
time](#the-rate-over-time) gives the rules.

The `quality` row counts what the DAQ recorded about the data, such as
damaged buffers, unused bytes and the readout parser's warnings. These
describe the recording, not a FLIRT problem. The report keeps their
counts and the first 100 parser warnings. The example with one byte
changed (see the [tutorial](tutorial.md#10-what-flirt-refuses)):

```
    quality   7 damaged buffers, 6,399,648 unused bytes, 1 module header
              mismatch, 4 readout parser warnings
```

### Settings changed during the run

With an MVLC, mvme can run VME scripts, for example to change a module's
settings, while a run is recording. A run can then hold parts taken with
different settings. FLIRT looks for such changes in mvme's log of the run,
`messages.log` in the listfile. If it finds one, the run's block warns.
An illustration:

```
    warning   settings changed during the run: 14:40:00 script "Frontend
              Settings for module mdpp_a" (2 register writes); 14:50:00 VME
              Debug write 0x2000611a -> 0x07d0; see the report
```

The report lists every change under `run.settingsChanges`. A change is a
successful register write from a script or from mvme's VME Debug window
while the DAQ was reading out. FLIRT cannot see:

- scripts run with the VME Debug window's "Run script" button, because
  mvme does not log their results;
- the single writes of a Trigger I/O script, which mvme only sums up;
- anything done outside mvme, such as cables, high voltage or the beam.

If the log is missing or not in a form FLIRT knows, the check is not done,
and FLIRT says so; it never reports "no changes" then. This check has not
yet been tested on a real run with a change. It was tested with log lines
written exactly as mvme writes them.

### Skipping and refusing

Before converting a run, FLIRT looks at what is already in the output
folder:

- **None of the run's files exist:** the run is converted.
- **The run is skipped** if its report says it was converted the same way:
  with status `ok`, by this FLIRT version, mvme version, ROOT version and
  platform, from the same listfile (same size and modification time),
  with the same analysis and options, and with all its files still there
  at the recorded sizes. A development build (an empty version, or one
  that ends in `-dirty`) must also be this very program.
- **Otherwise the run is not converted** (`outputs-exist`), and the message
  names the first thing that differs. FLIRT never overwrites a file: to
  convert the run again, delete its files, or choose another output
  folder.

The example converted a second time:

```
Converting 1 run into ~/FLIRT/practice

– example_run   skipped: already converted with the same input, options and
    FLIRT build

Done: 1 skipped in 0.1 s.
```

Two inputs with the same run ID (for example the same file given twice)
are refused before anything starts, because their files would have the
same names.

### Input integrity checks

Every conversion checks its input. If a check fails, nothing is written
and the run fails:

| Check | What it checks |
|---|---|
| I1 | The archive and its index (the list of the files inside it) can be read. |
| I2 | The recorded data has the length and the CRC-32 checksum that the archive's index records for it, over exactly the bytes the conversion read. |
| I3 | The same for the archive's analysis, when that analysis is used. With another analysis, a failure is only a warning. |
| I4 | The readout parser reported no errors. Only a warning: once I2 has passed, such errors describe the recording itself, and mvme converts such runs as they are. |
| I5 | The input files did not change during the conversion: their size, modification time and identity on the disk are the same at the end as at the start. |
| I6 | No output file would replace an input file. |

I2 cannot see damage that happened while recording, before mvme computed
the checksum, or a change made together with a new checksum.

`--accept-damaged-input` writes the files even when I2, I3 or I5 fail.
The report's status is then `accepted-with-failed-checks`, and the files
hold the part that could be read, as mvme would convert it. Such a run is
never skipped later. The option cannot override I1 or I6, nor an I2
failure where not even the start of the data can be read.

FLIRT refuses what it does not support (`unsupported`, exit code 3),
rather than convert it approximately:

- listfiles not recorded with an MVLC, and LZ4-compressed listfiles;
- one part of a run that mvme split into several listfiles (switch
  listfile splitting off in mvme);
- a VME config with more than one event that reads out data (FLIRT was
  checked with one VME event only), or of a version other than 4;
- a recorded setup that does not match the VME config;
- event building switched on in the analysis;
- analysis versions other than 4 and 5, and data sources other than
  `Extractor`, `ListFilterExtractor` and the MDPP sample decoder.

### What a failure says

A run that did not convert gets a headline, the reason (`why`) and what
to do (`to do`). The example with one byte changed:

```
✗ damaged   failed in 0.9 s: the listfile in the archive is damaged; nothing was
    published
    why       example_run.mvlclst: 13,499,868 bytes read, the archive's index
              says 13,499,864 (check I2-listfile-entry)
    to do     If this file is a copy, copy it again from the DAQ PC. To convert
              what can be read anyway, add --accept-damaged-input.
```

The headlines say no more than the check proves, and FLIRT never guesses
a cause. `flirt-tui` shows the same in the run's details.

| Result | Headline | What to do |
|---|---|---|
| failed (I1) | the archive cannot be read | if mvme is still writing the run, wait; otherwise mvme's listfile recovery may recover it |
| failed (I2) | the listfile in the archive is damaged | copy it again if it is a copy; `--accept-damaged-input` converts what can be read |
| failed (I3) | the analysis file in the archive is damaged | copy it again; or use another analysis (`--default-filters`, `--analysis`) |
| failed (I5) | an input file changed while it was converted | convert it again once nothing writes to the file |
| failed (I6) | an output would replace an input file | choose another output folder |
| failed | the output disk is full | free some space, or choose another output folder |
| unsupported | the listfile's name is not accepted, or the run is of a kind FLIRT does not support | rename the listfile; or convert the run with mvme |
| interrupted | stopped before it finished | run it again: runs already converted are skipped |
| outputs-exist | its files exist | delete them, or choose another output folder |

### Exit codes

A program's exit code tells scripts how it went (`echo $?` prints it
right after the command):

| Code | Meaning |
|---|---|
| 0 | every run was converted or skipped |
| 1 | a run failed, a run's files already exist, or the `--summary` file could not be written |
| 2 | a usage error, such as a wrong option, a missing output folder or a name that is not plain. Nothing was started. |
| 3 | a run was refused as unsupported (and none failed) |
| 130 | stopped (Ctrl-C, or a closed terminal or connection; see [Stopping](#stopping-ctrl-c)) |

A watch ends only when it is stopped, so it never exits with 130: it
exits with 1 if a run failed or its files existed, or its `--summary` file
could not be written, else with 3 if a run was unsupported, else with 0. `--find-dither-offset` exits with 0 if
every offset was found, and with 1 if not.

### Stopping (Ctrl-C)

Ctrl-C stops the batch: no new run starts, the running ones stop, and
nothing is left half-written. A run that is already putting its files in
place finishes first. The runs not yet started are listed as
`not-started`. To go on, run the same command again: the finished runs
are skipped.

Closing the terminal, or losing the ssh connection, stops FLIRT the same
way. Log in again and run the same command. If the network dies without
the connection being closed (a laptop goes to sleep), the other computer
may notice it only after about two hours, and FLIRT keeps working until
then; running it again in the meantime is safe, because FLIRT never
writes over an existing file. `flirt-convert` started over ssh without a
terminal (`ssh host flirt-convert ...`) notices a dropped connection only
when a run starts or ends, so the run being converted finishes first.

After a crash, hidden `.flirt-…` folders may be left in the output
folder. You can delete them.

### Watch mode

A watch runs on the DAQ computer while you take data. It waits for mvme to
finish each run, then converts it, one run at a time (unless `-j` says
more):

```sh
flirt-convert --watch /data/listfiles -o /other-disk/rootfiles
```

`--watch` names the folder mvme records into, and `-o` the output folder,
which must be outside it. The watch keeps running until you stop it with
Ctrl-C. If it was stopped, start it again: it converts the runs it
missed, and skips the others.

- **When a run is finished.** mvme writes a run's archive index last,
  when it closes the run. The watch looks at the folder every 2 seconds,
  and takes a file once it has not changed for `--settle` seconds (10 by
  default) and its index can be read. A recording that crashed never gets
  an index. It is reported once and never converted; mvme's listfile
  recovery may recover it. An illustration:

  ```
  run042: not finished (no ZIP central directory yet): mvme is still writing it or was stopped without closing it; it is converted once mvme closes it
  ```

- **Each run once.** A run is converted once, by the same rules as a batch
  ([Skipping and refusing](#skipping-and-refusing)). A failed run is
  tried again only if its file changes or the watch is restarted.
- **Free space.** Before each conversion, the watch checks that the output
  disk has 3 times the run's listfile data free. If not, it waits, and
  goes on by itself when there is room. A run stopped by a full disk is
  converted again when there is room, up to 3 times.
- **Another disk.** Put the outputs on another disk than the one mvme
  records to, so that they can never fill up the DAQ's disk. The watch
  warns at the start if both are on the same disk.
- **Split runs** (`<prefix>_partNNN.zip`, mvme's listfile splitting) are
  refused as unsupported. Switch listfile splitting off in mvme.

The watch takes the other options of a batch, such as `-j` and
`--summary`. On a terminal, the bottom line shows what it is doing or
waiting for.

**The watch has not been checked on a real DAQ yet.** It was tested for 30
runs in a row with a pretend DAQ. Before relying on it in an experiment,
run it during a few runs, with the defaults `-j 1 --threads 1`, and check
that mvme's DAQ statistics show no lost buffers or packets.

### Dither offset

`mvme_root_client` adds a random number between 0 and 1 to every value it
stores (dither), so that whole-number readings spread evenly over a
histogram's bins. The numbers come from one fixed list. The client starts
at the top of the list when it starts, and goes on down the list from run
to run for as long as it keeps running. FLIRT starts at the top of the
list for every file, as a client does that is started for one run
(`mvme_root_client --single-run`).

A file's *dither offset* is how many numbers the client had used before
that file. It matters only when you compare files bit for bit, never for
the physics. FLIRT can read it from the file itself:

```sh
flirt-convert --find-dither-offset examples/official/example_run_raw.root
```

```
examples/official/example_run_raw.root: dither offset 0
```

The example's official files were made with `--single-run`, so the offset
is 0. For a file that gives, for example, 123456789, `--dither-offset
123456789` converts the run with the same random numbers. Or let
`--dither-offset-from` find the offset and convert with it, in one step:

```sh
flirt-convert run042.zip -o out/ --dither-offset-from official/run042_raw.root
```

- The official file must be the same run's `_raw.root`: the same run name
  (`RunID`; a file mvme wrote live, named after its start time, is
  accepted with a note), the same modules and as many events. Otherwise
  the run is refused, and nothing is written.
- `--dither-offset-from` works with one listfile at a time, because each
  run has its own offset, and not with `--watch`.
- The search takes about a minute at most. In `flirt-tui`, the **From a
  file…** button under the dither offset finds it and fills in the field.
- No offset can be found in a file that was not dithered, a file that is
  not an mvme `_raw.root`, a `_histos.root`, a file with several event
  trees, or a file with no hits. FLIRT says which.
- A file that FLIRT made with `--official-waveform-branches` gives the
  offset it was made with. One made with the default settings gives none
  when the analysis has waveform sources: their values took random
  numbers that the file does not hold, and the message says so.
- A file in which mvme replayed the run twice gives the first copy's
  offset, with a note.

### Default filters

`--default-filters` converts with the analysis that mvme builds when you
start a new analysis and click "Generate default filters" on every
module. The filters come from mvme's templates for each module type; the
multi-event settings (how mvme splits the data of modules that read out
several events at once) come from the listfile's own analysis, which
must be of version 4 or 5. A module type without a template gets no
filters, as in mvme, and a note names it. The files are what mvme 1.18.0
and `mvme_root_client` write with that analysis.

### Waveforms

A trace, or waveform, is the shape of one channel's signal, recorded as a
series of samples. FLIRT writes every trace of the MDPP modules to
`<runId>_waveforms.root`: all channels and every sample, as mvme's own
decoder returns them. This is the only place with the full traces,
because mvme's official files keep only channel 0, cut to 8 bits. The file
does not depend on the analysis. `--no-waveforms` switches it off.

A module gets a tree in the file when it is enabled, its type is one
mvme's decoder supports (`mdpp16_scp`, `mdpp16_qdc`, `mdpp32_scp`,
`mdpp32_qdc`), its sampling is switched on in the VME config, and its
event has at least one data source in the analysis. In the example,
`mdpp_a` has sampling switched off, so the file has trees for `mdpp_b`
and `mdpp_c` only. A run without such a module gets no waveforms file.

Each tree is named after its module, and each entry is one trace:

| Branch | Type | Meaning |
|---|---|---|
| `event` | Long64_t | the event's entry in the event tree of `_raw.root` |
| `channel` | Int_t | the module's channel |
| `timestamp` | ULong64_t | the module event's timestamp (0 if none) |
| `config` | Int_t | from the trace header: the sampling configuration (register 0x614A) |
| `phase` | Int_t | from the trace header: the phase correction, 0 to 511 |
| `length` | Int_t | from the trace header: the trace length in words (normally 2 samples per word) |
| `nsamples` | Int_t | the number of samples |
| `samples[nsamples]` | Short_t | the samples, 14-bit signed, as decoded (no dither added) |

The time between samples is 12.5 ns. `event` links a trace to its event:
`physics_event->GetEntry(event)` gives the same event's amplitudes. A
channel hit twice in one event has two traces. Reading the file in ROOT:

```cpp
TFile f("practice/example_run_waveforms.root");
auto t = f.Get<TTree>("mdpp_c");
t->Draw("samples:Iteration$", "channel==0", "l");  // all channel-0 traces on top of each other
t->Draw("samples:Iteration$", "", "l", 1, 42);     // one trace: entry 42
```

With uproot, in Python:

```python
import numpy as np
import uproot
t = uproot.open("practice/example_run_waveforms.root")["mdpp_c"]
a = t.arrays(["event", "channel", "samples"], library="np")
trace = a["samples"][0]          # numpy int16 array of the first trace
t_ns = 12.5 * np.arange(len(trace))
```

`flirt-tui` plays them event by event in its Waveforms tab (see
[flirt-tui](#flirt-tui)).

**The waveform config check.** FLIRT also reads each MDPP module's setup
in the VME config, to know whether sampling is on and how many samples a
trace has, and compares that with the traces it found. It only warns
(`warning: waveforms: <module>: ...`), for example when sampling is on
but no traces were found. The report's `waveformConfig` lists the result
for each module. For the example's `mdpp_c`: `sampling on, 200 samples;
19583 of 19586 traces have 200 samples`.

A few traces have a different number of samples than their header says;
the report counts them as `tracesOtherLength`. They come from mvme's
decoder as it is ([design.md](design.md), "Waveforms with any analysis").
Writing the waveforms file makes a conversion about a tenth longer (with
`--default-filters`, about twice as long, because FLIRT then decodes the
traces itself).

### Pile-up and overflow flags

Each amplitude an MDPP module records comes with flags from the hardware:
pile-up (two pulses too close together) and over/underflow (the pulse went
out of range) on SCP modules, overflow on QDC modules. mvme's default
analysis does not extract these flags. `--extract-hit-flags` adds them, as
a column next to each module's amplitudes, the way mvme's manual describes
editing the analysis by hand. In `flirt-tui`, it is the tick "Add
pile-up/overflow flags".

The output folder must exist first (`mkdir -p` makes it, without an
error if it exists already):

```sh
mkdir -p practice/flags
flirt-convert examples/example_run.zip -o practice/flags --extract-hit-flags
```

The run's block then lists the extra file and a `flags` row:

```
    files     _raw.root   _histos.root   _waveforms.root   _hit_flags.analysis
              .flirt.json
    flags     0 pile-up, 0 over/underflow, 0 both (of 88,989 hits)
```

The example's data were made up without pile-up or overflow, so its
counts are 0. The new column, for example `mdpp_c_amplitude_flags[32]`
next to `mdpp_c_amplitude[32]`, holds for each hit:

| Value | SCP modules | QDC modules |
|---|---|---|
| 0 | clean | clean |
| 1 | over/underflow | overflow |
| 2 | pile-up | — |
| 3 | pile-up and over/underflow | — |

Like every value, a flag has a random number between 0 and 1 added, so a
cut must test a range. These commands draw the clean amplitudes of
channel 1, and the piled-up ones:

```cpp
physics_event->Draw("mdpp_c_amplitude[1]", "mdpp_c_amplitude_flags[1] < 1");
physics_event->Draw("mdpp_c_amplitude[1]", "int(mdpp_c_amplitude_flags[1]) == 2");
```

Write `int(x)`, not `(int)x`, which ROOT's `TTree::Draw` does not
understand. A channel without a hit holds NaN (not a number) in both
columns, which no such cut selects. For clean hits, test `< 1`, never
`int(x) == 0`: on a Mac with Apple silicon, `int` of NaN is 0, so that cut
would also select every event without a hit. The flags' histograms are
`physics_event/mdpp_c_mdpp_c_amplitude_flags[i]`, with 4 bins from 0 to 4.

- `<runId>_hit_flags.analysis` is the edited analysis. Loaded into mvme,
  it makes the same files.
- Every value gets a different random part than without the option,
  because the new column takes its own random numbers. The whole numbers
  stay the same. So compare such files with official files made from
  `<runId>_hit_flags.analysis`.
- The report's `hitFlags` holds the counts for each channel, and lists
  what got no flag column and why.

### Compression

`--compression` sets how the ROOT files are compressed. The values are
the same whatever you choose; only the way they are packed differs.

| Choice | ROOT setting | What it gives |
|---|---|---|
| `mvme` (default) | ZLIB level 1 (101), as `mvme_root_client` | files identical to the official ones |
| `fast` | ZSTD level 1 (501) | about 40 % less time for a run without waveforms, and files a little bigger |
| `small` | ZSTD level 9 (509) | files a sixth to a third smaller, but 6 to 17 times slower to write: for archiving |

A run with waveforms spends most of its time decoding the traces, so
`fast` gains it little. Only `mvme` gives files identical to the official
ones. With the others, the values are the same but the packing differs,
so compare them with `compare --data-only` (on a Mac, also
`--allow-fused-sumwx2`):

```
SAME DATA (layout not compared) EXCEPT fTsumwx2 fused rounding in 83 histograms (1.0 s)
```

(the example converted with `--official-waveform-branches --compression
fast`, on a Mac). ROOT reads every compression by itself, so the same
macros work. ZSTD needs ROOT 6.20 or newer, and uproot needs the
`zstandard` package (`pip install zstandard`). The choice is one of the
options a report records, so a run is not skipped when you convert it
again with another choice. The measurements are in
[design.md](design.md), "Compression".

### Events for programs

`--json-events` is for a program that runs `flirt-convert`, as
`flirt-tui` does. stdout then carries only events: one JSON object per
line, written the moment it happens, with its kind in `"event"`.
Everything else (stderr, the files, the report, `--summary`, the exit
code) stays the same, except that stderr's rows are not wrapped. Text that
is not valid UTF-8 appears with `?` for each invalid byte.

| Event | Fields | When |
|---|---|---|
| `start` | `flirt` (the build), `options` | once, when the arguments are accepted |
| `plan` | `runs`: `[{runId, input, bytes, action}]`, action `convert`, `skipped`, `outputs-exist` or `unsupported` | once, before any conversion, in input order |
| `progress` | `runId`, `phase` (`starting`, `converting`, `finishing`), `percent` (0–100 while converting, else null) | when the phase or the whole percent changes, at most once a second per run |
| `message` | `runId`, `text` | each line of the detailed messages, in order (run ID null for the batch's own lines) |
| `result` | the run, as in the summary's `results` | once per run |
| `end` | `summary`: what `--summary` writes | last; the exit code is `summary.exitCode` |
| `usage-error` | `message` | a usage error: the only event |

Programs may rely on the results and the check IDs. The headlines and
to-dos are written for people and may be reworded. Some of the example's
events:

```
{"event":"plan","runs":[{"action":"convert","bytes":9763504,"input":"examples/example_run.zip","runId":"example_run"}]}
{"event":"progress","percent":null,"phase":"starting","runId":"example_run"}
{"event":"progress","percent":7,"phase":"converting","runId":"example_run"}
{"event":"progress","percent":null,"phase":"finishing","runId":"example_run"}
{"event":"message","runId":"example_run","text":"run 2026-03-03 14:10:00 CST to 14:10:22 CST (22 s), 17207 events (782.1/s)"}
```

**`flirt-convert --find-dither-offset --json-events <file>...`** gives
these events:

| Event | Fields | When |
|---|---|---|
| `progress` | `done`, `total`: the random numbers searched so far, out of 4,294,967,296 | at the start, then at most 4 times a second |
| `dither-offset` | `file`, `offset` (or null), `heldTwice` (the run is in the file twice), `problem` (why there is no offset, or null) | one per file, after the search |
| `end` | `exitCode` | last |
| `usage-error` | `message` | a usage error |

```
{"done":0,"event":"progress","total":4294967296}
{"event":"dither-offset","file":"examples/official/example_run_raw.root","heldTwice":false,"offset":0,"problem":null}
{"event":"end","exitCode":0}
```

**`flirt-waveforms`** takes no arguments. It reads one JSON request per
line on stdin and writes one JSON answer per line on stdout, and it
never writes a file. Each request names its file by its full path (a
name that is not valid UTF-8 is also sent as hex bytes, in `fileBytes`):

- `{"id": 1, "op": "trees", "file": "/out/run_waveforms.root"}` answers
  `{"id", "file", "trees": [{"name", "title", "traces", "typical"}]}`.
  `typical` is `{"traces", "low", "high", "samples"}`, from the tree's
  first 20,000 traces with samples: `low` is the 1st percentile of their
  lowest values, `high` the 99th percentile of their highest values, and
  `samples` their most common length (null when no trace has samples).
- `{"id": 2, "op": "frame", "file": ..., "tree": "mdpp_c", "channel":
  null, "position": 0}` answers `{"id", "file", "tree", "channel",
  "channels", "frames", "position", "depth", "event", "timestamp",
  "traces": [{"channel", "samples", "age"}]}`. A frame is one event's
  traces. The request may give `"event": E` instead of `position`, a
  channel or a list of channels instead of `null`, and `"depth": N` (1 to
  100) for N frames back. `age` is 0 for the frame at `position` and k
  for the frame k before it.

An error is answered with `{"id", "error": "<sentence>"}`, and the helper
goes on.

## flirt-info

`flirt-info` displays what listfiles recorded about their runs, without
converting them: the start, stop and length, the events and their rate,
lost network packets, the rate over time, the mvme version and the DAQ
computer's system, the MVLC connection, the modules, and settings changed
during the run. It does not need ROOT. On a Mac it reads about 5.5
seconds per gigabyte of listfiles, and several files at once on up to
half of the computer's processor cores.

```sh
flirt-info run042.zip                          # one listfile: its details
flirt-info /data/listfiles                     # a folder: one row per run
flirt-info /data/listfiles --csv runs.csv      # the same, and every field into a CSV file
flirt-info --json /data/listfiles              # one JSON object per listfile, for programs
```

A folder means its `*.zip` files, as for `flirt-convert` (see
[Inputs](#inputs)). One listfile gives its details. The example:

```
example_run (examples/example_run.zip, 9.3 MB)
  run           2026-03-03 14:10:00 CST to 14:10:22 CST (22 s), 17207 events (782.1/s)
  lost packets  not recorded (USB)
  rate          not checked (no minute with 30 s of recording)
  recorded by   mvme 1.18.0 on Rocky Linux 8.10 (Green Obsidian) (x86_64), MVLC over USB, jumbo frames unknown, receive buffer unknown
  modules       physics_event: mdpp_a (mdpp32_scp), mdpp_b (mdpp32_scp), mdpp_c (mdpp32_scp), mvlc_event_stamper (mvlc_event_stamper)
```

| Line | What it says |
|---|---|
| `run` | the start, the stop, the length, the events and their rate |
| `lost packets` | over the network (ETH): the packets the DAQ lost, about how many events that cost, and in which minutes. Over USB there are no packet numbers, so `not recorded` |
| `rate` | the median event rate and the pauses (see [The rate over time](#the-rate-over-time)) |
| `recorded by` | the mvme version and the DAQ computer's system, the MVLC connection, and the network receive buffer, as mvme logged them |
| `modules` | each event's modules, with their types |
| `settings` | only if settings were changed during the run, or the check could not be done (see [Settings changed during the run](#settings-changed-during-the-run)) |
| `parser` | only if the readout parser warned about the recording |
| `damaged` | only if the file is damaged, and why |

A low rate or a rate change gets a `warning:` line under the details, and
a small network receive buffer on a run that lost packets a `note:` line,
as in [Run summary](#run-summary).

Several listfiles give one row per run, and a summary line. The example
and its copy with one byte changed:

```
run          start             length              events  rate/s  lost packets    flags
example_run  2026-03-03 14:10  22 s                 17207   782.1  n/a (USB)
damaged      2026-03-03 14:10  10 s                  8960   896.0  n/a (USB)       damaged: example_run.mvlclst: 13,499,868 bytes read, the archive's index says 13,499,864
2 listfiles: 2 read, 0 unreadable
```

- `--csv <file>` also writes every field of every run to a new CSV file,
  which a spreadsheet can open. `--csv -` writes it to stdout instead of
  the table. Its columns: `listfile`, `run`, `status` (`ok`, `damaged` or
  `unreadable`), `problem`, `start`, `stop`, `duration_s`, `paused_s`,
  `live_s`, `events`, `rate_hz`, `median_rate_hz`, `eth_packets`,
  `lost_packets`, `lost_share`, `lost_events_estimate`, `loss_bursts`,
  `loss_minutes`, `flags`, `mvme_version`, `system`, `connection`,
  `jumbo_frames`, `receive_buffer_bytes`, `modules`, `settings_changes`,
  `parser_warnings`. An empty field means unknown.
- `--json` writes one JSON object per listfile: `{listfile, runId,
  status, problem, bytes, connection, modules, run, details, warnings,
  note, parserWarnings}`. `run` is the same as a conversion report's
  `run` section, and `details` holds the lines above as `[{label,
  text}]`.

flirt-info only reads. It writes nothing but the `--csv` file, which it
creates new and never writes over an existing file. A listfile whose
recording `flirt-convert` would refuse (because of its name, its format or
its VME setup) is listed as `unreadable`, with the reason, and the others
are still read. flirt-info does not read the analysis, so it does not see
refusals that come from the analysis. A damaged listfile is read as far as it
goes and marked `damaged`. flirt-info exits with 0 if every listfile was
read (damaged ones included), with 1 if some could not be read, and with
2 for a usage error.

#### Lost packets

Over the network (ETH), the MVLC numbers its packets, so the gaps in the
numbers are the packets the DAQ computer lost. A burst is one such loss.
The lost events are estimated from the lost packets and the events per
packet; this estimate was never checked against a known count. The
receive-buffer note appears when packets were lost and mvme's log says
that the DAQ computer gave the MVLC's connection less than the 10 MB
mesytec-mvlc asks for. mesytec's MVLC notes suggest raising the system
setting `net.core.rmem_max`.

#### The rate over time

mvme writes a time mark (a timetick) about once a second. From them,
FLIRT counts the events of each minute and takes the median rate over the
minutes with at least 30 seconds of recording. Then it looks for:

- **pauses**: from each Pause to its Resume;
- **low rate**: minutes whose rate is below 15 % of the median. FLIRT does
  not say the beam was off: a low rate can have other causes (a detector,
  the trigger, the target), so the warning asks you to check;
- **rate changes**: a lasting step of at least 25 % in the rate, in the
  middle of the run about 7 minutes long and at its end about 3 minutes.
  Short excursions and slow drifts are not steps.

A run without a clock minute that holds 30 seconds of recording, like the
example (22 s), is not checked. In a slow
run (about one event per second), chance alone can make a minute look
low, so read its warnings with that in mind. The exact rules are in
[design.md](design.md), "Run information".

## flirt-tui

`flirt-tui` is FLIRT's terminal user interface (TUI): pick the
listfiles, press Convert, and watch the result. It converts exactly as
`flirt-convert` does, because it runs `flirt-convert` for you. It needs a
terminal window of at least 80 columns and 24 rows that can show UTF-8
characters, and it works over ssh. It is checked by automated tests on
Linux and macOS, and was used by hand in one terminal on a Mac. It has not
yet been used on a DAQ computer. The [tutorial](tutorial.md), step 9,
shows it in two animations.

```sh
flirt-tui                           # start with an empty list
flirt-tui /data/listfiles -o out/   # every .zip in a folder, and where the ROOT files go
flirt-tui run042.zip run043.zip -o out/   # two listfiles, and where the ROOT files go
flirt-tui --no-mouse                # leave the mouse to the terminal, for example to select text
```

These arguments only fill in the screen: nothing starts until you press
**Convert**. A listfile whose name is not plain (see [Names](#names)) is
listed, and refused when you convert.

**The screen** has three boxes and a bar:

- **Box 1** (top left) lists the runs, with a mark for how each went: `○`
  waiting, `●` converting, `✓` converted (in yellow: look at its
  details), `–` already converted, `!` stopped or not converted, `✗`
  failed. Its second tab, **Waveforms**, lists the waveforms files.
- **Box 2** (bottom left) holds the options; in the Waveforms tab, the
  plot options.
- **Box 3** (right) shows what the chosen box on the left holds: a run's
  **Details** (the same facts `flirt-info` displays, with a small graph of
  the rate over the run, and after a conversion its status and files) and
  the **Log** (what `flirt-convert` printed), or the options, or the plot.
- **The bottom box** says "Ready" with **Convert N runs**, or shows the
  conversion's progress with **Stop**. The line under it lists the keys
  you can press right now.

The box you are in has a heavy cyan edge. To convert a run marked `!
exists` again, first delete its files in the output folder (its details
name them), then press Convert.

**Keys:**

| Key | What it does |
|---|---|
| Tab, Shift-Tab, `1` `2` `3` | the next box, the previous one, box 1, 2 or 3 |
| ↑ ↓ | choose a run, a file or an option; scroll the details |
| PgUp, PgDn, Home, End | box 1: 10 runs up or down, the first or last; box 3: scroll |
| `[` `]` | switch the tabs of the box you are in |
| Enter | go into the details or the options, press a button |
| Space or Enter | in the options: act on the row, as the bottom line says |
| Del, Backspace | take a run off the list |
| `a`, `o`, `s` | add listfiles, choose the output folder, convert or stop |
| `c`, `l`, `r` | show the terminal command, the Log, a run's report |
| Esc | back to the box on the left, or close a view |
| `?` | show all keys |
| `q`, Ctrl-C | quit (during a conversion it asks first) |

**The mouse** works too: click a box, a tab or a row to go there,
double-click a run for its details, click a button or a key on the bottom
line. The wheel scrolls. To select text, hold Shift while you drag (⌥
Option in iTerm2), or start `flirt-tui --no-mouse`.

**Quitting** (`q`) during a conversion asks first, and stops it the same
way as Ctrl-C does for `flirt-convert`. On leaving after a conversion,
`flirt-tui` prints the `flirt-convert` command that does the same
conversion, with full paths.
Closing the terminal or the ssh session stops it in the same way.

**What it remembers:** the output folder, the analysis choice, the
waveforms and flags ticks, and the last folders you browsed. Not the list
of listfiles, and not the Advanced options, because those could change
your output without you noticing. `-o` replaces the remembered output
folder. The settings are in `settings/flirt-tui.json` in FLIRT's folder,
next to `bin/` (for example `build/macos/prefix/settings/flirt-tui.json`),
and everyone who runs this copy of FLIRT shares them. To keep them in a
file of your own, start it as `env FLIRT_TUI_SETTINGS=/full/path/flirt-tui.json
flirt-tui`.

### Looking at waveforms

Box 1's **Waveforms** tab lists the waveforms file of every run converted
since `flirt-tui` started, and the files you open with `o`. Box 3 plots
the chosen file one frame at a time: one event of one module.

- **The grid** draws one panel per channel, in rows of 4, each channel
  always in the same place. Channels with traces have their names in
  bold. When not all rows fit, PgDn and PgUp turn the pages. `z` shows
  the plot on the whole screen, and `z` or Esc goes back.
- **Overlaid** draws all the traces in one plot, one colour per channel.
- **The y range** is the same for every channel and every frame, so frames
  can be compared by eye. **Typical** (the default) is taken from the
  module's first 20,000 traces; **own** is a range you type, such as `0 to
  8000`. A trace beyond the range runs along its edge, with its real
  highest or lowest value in red, for example `▲2325`.
- **Box 2, Plot options:** Module (the next module), Channels (pick which
  ones), Show (grid or overlaid), Y range (typical or own), and Traces
  (how many past events to draw, faded, as mvme does).
- **Over ssh**, `flirt-tui` draws at most 100 traces at once and plays at
  most 10 events a second, to keep the connection light.

| Key | What it does |
|---|---|
| ← → | the previous, the next frame |
| Space | play or pause; in box 2, change the plot option |
| `-` `+` | slower, faster play (1 to 50 events a second) |
| PgUp, PgDn | grid: the previous, the next page; overlaid: 100 frames back or forward |
| Home, End | the first, the last frame |
| `g` | go to an event: type its number, then Enter |
| `z` | the full window, and back |
| `o` | open a waveforms file |
| Esc | back to box 1, or from the full window to the boxes |

## compare

`compare` checks whether two ROOT files hold the same data, value by
value. Give it FLIRT's file first and mvme's second. It needs only ROOT,
and shares no code with FLIRT's converter.
[tools/compare/README.md](../tools/compare/README.md) explains its
options, what it checks and how to read its result.
[Verifying against official output](#verifying-against-official-output)
says which options to use for which of mvme's files.

## Differences from the official output

For the setups FLIRT was tested with (see [Tested with](#tested-with)),
its files are identical to those of `mvme_root_client --single-run`, apart
from each file's unique ID (UUID), its time stamps, the compressed byte
counts, and these:

- **Waveform sources** (the parts of an analysis that read the recorded
  traces) are left out of `_raw.root` and `_histos.root` by default,
  because mvme writes only their channel 0, cut to 8 bits. Every other
  value is identical, and all the traces are in `<runId>_waveforms.root`.
  The report says `matchesOfficialLayout: false`.
  `--official-waveform-branches` writes the sources as mvme does.
- **The dither offset:** FLIRT starts mvme's list of random numbers afresh
  for each file, as `mvme_root_client --single-run` does. A file made by a
  client that had already converted other runs needs `--dither-offset`
  (see [Dither offset](#dither-offset)).
- **Files mvme wrote live**, during the run: mvme names such a run after
  its start time, and FLIRT after the listfile, so the run name stored in
  the file (`RunID`) differs.
- **On a Mac with Apple silicon, `fTsumwx2` of some histograms** (the sum
  of squares ROOT keeps for a histogram's RMS) differs from the Linux
  value in its last digits. The cause is ROOT itself: on these processors,
  it computes each step of that sum with one rounding instead of two.
  Everything else is identical ([design.md](design.md), "macOS (Apple
  silicon)").

## Verifying against official output

To check FLIRT's files against mvme's, `compare` them. The example run,
converted with `--official-waveform-branches` into `practice/as-mvme`:

```sh
compare --allow-fused-sumwx2 \
    practice/as-mvme/example_run_raw.root examples/official/example_run_raw.root \
    --histos practice/as-mvme/example_run_histos.root examples/official/example_run_histos.root
```

It prints each check, and the verdict last. On a Mac (the first lines,
which name the files and the options, are left out here):

```
...
PASS  raw: keys
PASS  raw: streamer info
PASS  raw: MVMERunInfo
PASS  raw: tree layout
PASS  raw: tree values (bitwise)
      info: physics_event;2: compared 17207 entries x 3293 slots in 35 leaf branches
      info: physics_event;1: compared 0 entries x 3293 slots in 35 leaf branches
PASS  histos: keys
PASS  histos: streamer info
PASS  histos: contents (bitwise)
...
IDENTICAL EXCEPT fTsumwx2 fused rounding in 83 histograms (1.0 s)
```

On Linux the verdict is `IDENTICAL`. Which options to add:

| Case | Option | Verdict |
|---|---|---|
| FLIRT's default files, without the waveform sources | `--ignore-branch-regex _samples` (or convert with `--official-waveform-branches`) | `IDENTICAL EXCEPT IGNORED '_samples' (basket layout not compared)` |
| FLIRT's files made on a Mac with Apple silicon | `--allow-fused-sumwx2` | `IDENTICAL EXCEPT fTsumwx2 fused rounding in N histograms` |
| mvme's files written live, during the run | `--ignore-run-id` | `IDENTICAL EXCEPT RunID` |
| FLIRT's files made with `--compression fast` or `small` | `--data-only` | `SAME DATA (layout not compared)` |
| mvme's file made by a client that had converted other runs | none: convert with `--dither-offset-from <its _raw.root>` | |

A file mvme wrote live may also need its dither offset, unless the
client was started fresh for that run. For anything FLIRT was not tested
with, compare one run against the files of `mvme_root_client
--single-run` before you trust FLIRT's.

## Tested with

FLIRT runs the code of mvme 1.21.0 with ROOT 6.36.04. Its files were
compared with `compare` against official files made by older mvme versions
(1.16 to 1.18). Nothing was compared with files of mvme 1.19 to 1.21.

What was compared:

- **Recordings over the network (ETH) with MDPP-32 SCP modules**, made
  with mvme 1.16, against the files `mvme_root_client` made from them at
  the time: identical.
- **Recordings over the network with MDPP-32 QDC modules**, made with
  mvme 1.17, against files made for the check with mvme 1.18: identical
  with `--official-waveform-branches`. Against the files mvme wrote live
  during those runs: identical except the run's name (`RunID`).
- **Recordings over USB with MDPP-32 SCP modules and recorded traces
  (waveforms)**, made with mvme 1.18, against files made for the check
  with mvme 1.18: identical with `--official-waveform-branches`.

All of them had one VME event and no event builder. Between them they
used the modules `mdpp32_scp`, `mdpp32_qdc` and `mvlc_event_stamper`.
They were converted with the analysis stored in the listfile, in mvme's
analysis file formats 4 and 5. `--default-filters` was checked against
files made by mvme 1.18 itself.

- **Platforms.** Rocky Linux 8 x86_64 with ROOT 6.36.04 and gcc-toolset-13,
  run emulated on a Mac (not yet on a physical Linux machine): everything
  above. macOS 15 on Apple silicon with ROOT 6.36.04 and Apple clang 16: a
  smaller selection, with some of the network and USB recordings with SCP
  modules and none with QDC modules.
- **Threads.** `--threads` 1, 2, 4 and 8 give files that `compare` finds
  `IDENTICAL` (the bytes on disk may be in another order).
- **Compression.** Only the default, `--compression mvme`, gives files
  identical to official ones. `fast` and `small` gave the same values
  (`compare --data-only` reports `SAME DATA`) for the recordings above on
  Linux and for the selection checked on macOS. These files can be read by
  mvme's `mvme_root_client --replay`, by plain ROOT macros, and by uproot
  (with its `zstandard` package installed).

Not tested yet:

- reading FLIRT's files with ROOT versions other than 6.36.04;
- the watch mode on a real DAQ computer (it was tested only with a
  pretend DAQ);
- a real run whose settings were changed through mvme during the run (it
  was tested with mvme's log lines inserted into a real log);
- other modules, other mvme versions, other setups.

For anything not listed, convert one run and compare it with the files of
`mvme_root_client --single-run` before you trust it (see [Verifying
against official output](#verifying-against-official-output)).
