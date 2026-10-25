<p align="center">
  <img src="docs/assets/logo.png" width="320" alt="FLIRT: Fast Listfile Interpretation and ROOT Translation">
</p>

<p align="center"><em>The perfect match for your mvme listfiles: the same ROOT files as mvme, without running mvme.</em></p>

---

![flirt-tui converting a made-up run, then playing its recorded traces](docs/assets/flirt-tui.gif)

Without running mvme, FLIRT turns mvme MVLC listfiles (the `.zip` archives
mvme records) into the ROOT files that official mvme and `mvme_root_client`
would make from them. FLIRT only works with listfiles that mvme has
already recorded and never connects to an MVLC or takes data itself.
FLIRT has four programs:

- `flirt-convert` converts listfiles from the command line;
- `flirt-tui` does the same in a terminal user interface (TUI), and shows
  the waveforms;
- `flirt-info` displays what a listfile recorded about its run, without
  converting it;
- `compare` checks whether two ROOT files hold the same data.

## What FLIRT promises

- **The same output ROOT files as mvme.** For the setups it was [tested
  with](#tested-with), every value, tree, branch and histogram is identical
  to what `mvme_root_client --single-run` writes.
- **It never changes a listfile.** FLIRT only reads listfiles. Before it
  keeps a conversion, it also makes sure that nothing else changed the
  listfile in the meantime.
- **No half-written files.** A run's files appear only when its whole
  conversion has worked, and FLIRT never overwrites a file.
- **It writes only where you tell it to.** Results go only into the
  output folder you choose, and into any extra file you name, such as a
  summary. The only other thing FLIRT saves, the settings of `flirt-tui`,
  stays inside FLIRT's own folder. Nothing is written anywhere
  else on your computer.

[docs/usage.md, "What FLIRT promises"](docs/usage.md#what-flirt-promises)
says exactly what this means, and the few things to know when you compare
with mvme's own files.

## Features

- Convert one run, a folder of runs, a list of runs, or each run as soon
  as mvme has finished recording it (the watch mode).
- Checks every listfile against the checksum its archive records, and
  refuses what it cannot convert exactly as mvme would.
- A short summary for each run: when it ran, its events, network packets
  the DAQ lost, warnings when the event rate fell or changed, and settings
  changed through mvme during the run.
- Every recorded trace (waveform), of every channel, in a separate
  `_waveforms.root` file.
- `flirt-info`: displays what a listfile, or a folder of them, recorded,
  without converting.
- `flirt-tui`: convert in a terminal user interface, and play the
  waveforms event by event.
- `compare`: checks two ROOT files value by value.
- Faster or smaller compression, pile-up and overflow flags for MDPP
  amplitudes, finding the dither offset of mvme's own files, and
  converting with another analysis or mvme's default filters.

## Quick start

These steps take you from downloading FLIRT to a first conversion, checked
against mvme's own files. [docs/building.md](docs/building.md) has every
detail of building.

### 1. What you need

A recent C++ compiler, CMake, make, git, Qt 5 (only its core library),
Boost, zlib and ROOT 6. FLIRT was developed and tested on Rocky
Linux 8 (x86_64) and macOS (Apple silicon), with ROOT 6.36.04.
[Requirements](#requirements) says why each is needed, and
docs/building.md gives the commands to install them. For ROOT, follow
ROOT's own installation instructions, which docs/building.md links to.

### 2. Download FLIRT

```sh
git clone --recurse-submodules https://github.com/TAMU-Pangolins/FLIRT.git
cd FLIRT
```

`--recurse-submodules` is needed to also download the code FLIRT builds on: mvme, and
FTXUI for `flirt-tui`.

### 3. Build

The build needs to find ROOT. If you set up ROOT the usual way that its
installation instructions describe, with its `thisroot.sh` loaded in your
shell's startup file, it is found already: `root-config --version` then prints
ROOT's version. If that command is not found, load ROOT's settings in this
terminal first (`<root>` is the folder ROOT is installed in):

```sh
source <root>/bin/thisroot.sh      # in tcsh: source <root>/bin/thisroot.csh
```

On Linux, build FLIRT and then `compare`:

```sh
cmake -S . -B build/linux
make -C build/linux -j N
cmake -S tools/compare -B build/linux-compare
make -C build/linux-compare -j N
```

`cmake` prepares a build folder, `build/linux`, and checks that ROOT, Qt and
the compiler can be found; if something is missing, it stops and says
what. `make` then compiles in that folder. Replace `N` with how many files
it should compile at once, for example `-j 4`: any number works, and more
is faster up to the number of processor cores your computer has. To see
that number, run `nproc`. The first build takes a few minutes. The last two commands do the same for
`compare`, which is built on its own because it shares no code with the
converter. To use Ninja instead of make, see docs/building.md.

On a Mac, one script does all of this:

```sh
scripts/build_macos.sh
```

It first checks that everything is installed and tells you what is
missing, then builds FLIRT and `compare`.

### 4. Make the programs available by name

Run the one line that matches your shell (`echo $0` prints which shell you
have; most Linux terminals use bash, a Mac uses zsh):

```sh
source build/linux/env.sh          # bash or zsh (on a Mac: source build/macos/env.sh)
source build/linux/env.csh         # tcsh (on a Mac: source build/macos/env.csh)
```

After this you can type `flirt-convert` instead of
`build/linux/prefix/bin/flirt-convert`, in this terminal until you close
it.

To have the programs available in every new terminal, add the same line,
with the full path of your FLIRT folder, at the end of your shell's
startup file (`pwd` prints the full path while you are in the FLIRT
folder). For example, with FLIRT in `/home/you/FLIRT` (on a Mac,
`/Users/you/FLIRT`):

| Your shell | Startup file | Line to add |
|---|---|---|
| bash | `~/.bashrc` | `source /home/you/FLIRT/build/linux/env.sh` |
| zsh (the Mac's default) | `~/.zshrc` | `source /Users/you/FLIRT/build/macos/env.sh` |
| tcsh | `~/.tcshrc`, or `~/.cshrc` if that is the one you have | `source /home/you/FLIRT/build/linux/env.csh` |

Open the file in any text editor and add the line at the end. New
terminals then find FLIRT's programs by name; the one already open does
not, until you run the line there too.

### 5. Convert the example run

FLIRT comes with a small made-up run, `examples/example_run.zip`, and the
ROOT files mvme made from it, in `examples/official/`. Make a practice
folder and convert the run into it:

```sh
mkdir practice
flirt-convert examples/example_run.zip -o practice
```

FLIRT prints a short summary of the run. The folder `practice` now holds
its ROOT files (`_raw.root`, `_histos.root`, and `_waveforms.root` with the
recorded traces) and FLIRT's report (`.flirt.json`).

### 6. Compare with mvme's own files

When mvme replays a listfile, its program `mvme_root_client` writes two
ROOT files: `_raw.root` with the event data and `_histos.root` with the
histograms. `examples/official/` holds the two files it made from the
example run. This step checks that FLIRT makes exactly the same files.

There is one thing to change first. mvme also writes a short, trimmed copy
of the recorded traces into its ROOT files. FLIRT leaves that copy out by
default, because it writes the complete traces to `_waveforms.root`
instead. To get files exactly like mvme's, tell FLIRT to write that copy
too, with `--official-waveform-branches`. Convert into a new folder,
because `practice` already holds the files from step 5, and FLIRT never
overwrites files:

```sh
mkdir practice/as-mvme
flirt-convert examples/example_run.zip -o practice/as-mvme --official-waveform-branches
```

Then give `compare` FLIRT's two files and mvme's two files. It compares
them value by value:

```sh
compare --allow-fused-sumwx2 \
    practice/as-mvme/example_run_raw.root examples/official/example_run_raw.root \
    --histos practice/as-mvme/example_run_histos.root examples/official/example_run_histos.root
```

`compare` prints every check it makes. The last line is the answer:
`IDENTICAL` means the files hold exactly the same data. On a Mac, the last
line reads `IDENTICAL EXCEPT fTsumwx2 fused rounding in 83 histograms`.
That is expected: on Apple computers, ROOT rounds one statistic stored in
some histograms (`fTsumwx2`) very slightly differently, and
`--allow-fused-sumwx2` tells `compare` to accept exactly that difference
and nothing else. On Linux the option changes nothing.

The [tutorial](docs/tutorial.md) goes on from here, step by step.

## Documentation

| Document | What's in it |
|---|---|
| [docs/tutorial.md](docs/tutorial.md) | A hands-on walk through FLIRT with the example run: converting, the files, comparing with mvme, the terminal user interface, what FLIRT refuses. |
| [docs/usage.md](docs/usage.md) | The full reference: what FLIRT promises, every program, option, output file, report field, message and exit code, and what it was tested with. |
| [docs/building.md](docs/building.md) | What to install, and building on Linux and macOS. |
| [docs/design.md](docs/design.md) | The design decisions the code refers to, and why. |
| [tools/compare/README.md](tools/compare/README.md) | What `compare` checks, and how. |
| [examples/README.md](examples/README.md) | The made-up example run and mvme's files for it. |

## Requirements

You supply:

- a C++17 compiler with a recent standard library, because mesytec-mvlc
  (bundled inside mvme) uses the C++ header `<version>`, a part of the C++
  standard library that GCC has only from version 9 on (tested with GCC 13
  on Linux and Apple clang 16 on macOS);
- [CMake](https://cmake.org) 3.18 or newer and make (or Ninja), which
  build FLIRT (mesytec-mvlc needs 3.18);
- `git`, to download FLIRT and the code it builds on;
- Qt 5 (only its core library, Qt5Core), Boost and zlib, which mvme's code
  uses;
- [ROOT](https://root.cern) 6, installed separately, which writes and reads
  the ROOT files. Developed and tested with 6.36.04; with another version,
  check one run with `compare` before trusting it.

Bundled as git submodules, in `external/`, and built from source
unmodified: [mvme](https://github.com/flueke/mvme) 1.21.0 (with
mesytec-mvlc inside it) and [FTXUI](https://github.com/ArthurSonzogni/FTXUI)
6.1.9 (for `flirt-tui`).

## Tested with

FLIRT's files were compared value by value, with `compare`, against the
official files mvme and `mvme_root_client` made from the same runs, and
were identical for:

- recordings made with mvme 1.16, 1.17 and 1.18, over the network (ETH)
  and over USB;
- MDPP-32 modules with SCP and with QDC firmware, with and without
  recorded traces (waveforms), and the MVLC event stamper;
- official files made from the listfile after the run, and files mvme
  wrote live during the run (these differ only in the run's name, which
  `compare --ignore-run-id` leaves out).

All of them had one VME event and no event builder. FLIRT itself runs the
code of mvme 1.21.0 and was tested with ROOT 6.36.04 on Rocky Linux 8
(x86_64) and macOS (Apple silicon). [docs/usage.md, "Tested
with"](docs/usage.md#tested-with) has the details. For any other setup,
convert one run and compare it with mvme's own files before trusting
FLIRT's.

## Project layout

```
src/
├── apps/       the programs: flirt-convert, flirt-info, flirt-waveforms
├── common/     small helpers the programs share
├── flirt/      the conversion: wiring mvme's code, the checks, the run summary
├── ui/         what flirt-tui runs and reads (no interface code)
├── tui/        flirt-tui's terminal user interface
└── macos/      what the Mac build needs in place of USB code
tools/compare/  the checking tool; shares no code with the converter
examples/       the example run and mvme's files for it
docs/           the documentation
external/       mvme and FTXUI, as git submodules
cmake/          build helpers
scripts/        build_macos.sh
```

## Status

This is FLIRT's first public version. It has not yet been used on a DAQ
machine during an experiment, and its watch mode was tested only with a
pretend DAQ: before relying on it, run it for a few runs and check that
mvme's DAQ statistics show no lost buffers or packets.

## How FLIRT was made

FLIRT's code was written by Claude, Anthropic's AI model, directed,
reviewed and tested by Khang Pham. The parts that shape the ROOT files are
mvme's own code, taken from mvme 1.21.0; FLIRT's own code is the rest:
reading the configs, joining those parts, the safety checks and the
programs. Its output was checked against official mvme +
`mvme_root_client` files, as listed under [Tested with](#tested-with).

## License

FLIRT is free software under the GNU General Public License, version 3 or
later (GPL-3.0-or-later), because it compiles mvme's sources, which are
GPL-3.0-or-later; see [`LICENSE`](LICENSE). It uses
[mvme](https://github.com/flueke/mvme) (GPL-3.0-or-later), mesytec-mvlc
inside it (Boost Software License 1.0),
[FTXUI](https://github.com/ArthurSonzogni/FTXUI) (MIT License) and
[ROOT](https://root.cern), installed separately. mvme and mesytec-mvlc
bundle further libraries, each under its own license: see
`external/mvme/LICENSE-3RD-PARTY.TXT` and the folders under
`external/mvme/external/`.
