# Building FLIRT

FLIRT is built from its source code on your own computer. It was developed
and tested on Rocky Linux 8 (x86_64) and macOS (Apple silicon). This page
gives every step for both:

- [Linux](#linux): install the packages FLIRT needs, download FLIRT, then
  build it with `cmake` and `make`.
- [macOS](#macos): install Apple's command line tools and a few packages
  from Homebrew, download FLIRT, then build it with one script.

On both, FLIRT also needs [ROOT](#root), which you install first.

After building, see [Running the programs by
name](#running-the-programs-by-name), and later [Updating
FLIRT](#updating-flirt) or [Removing FLIRT](#removing-flirt).

The build puts everything it makes into a folder named `build` inside the
FLIRT folder. It installs nothing anywhere else on your computer.

## The programs

The build makes five programs:

| Program | What it does |
|---|---|
| `flirt-convert` | Converts listfiles into ROOT files, from the command line. |
| `flirt-tui` | A terminal user interface (TUI) for converting listfiles and viewing their waveforms. |
| `flirt-info` | Displays what a listfile recorded about its run, without converting it. |
| `flirt-waveforms` | A helper that `flirt-tui` starts by itself to read the waveforms in FLIRT's `_waveforms.root` files. It runs as a separate program so that a damaged file cannot crash `flirt-tui`. You never need to run it yourself. |
| `compare` | Checks whether two ROOT files hold the same data. |

The first four programs are built together and end up in one folder:
`build/linux/prefix/bin` on Linux, `build/macos/prefix/bin` on a Mac.
`compare` is built on its own, into `build/linux-compare` or
`build/macos-compare`, because it shares no code with the converter: it
checks FLIRT's files with code of its own.

Leave the programs where the build puts them. They need the folders next
to them (`prefix/lib`, `prefix/include` and `prefix/templates`) and do not
work without them. To run them by name from any folder, see [Running the
programs by name](#running-the-programs-by-name).

## ROOT

FLIRT needs ROOT 6, installed separately before you build FLIRT. To see
whether ROOT is installed and found, run:

```sh
root-config --version
```

If this prints a version number, such as `6.36.04`, ROOT is ready. If ROOT
is not installed, install it as ROOT's [installation
instructions](https://root.cern/install/) describe.

If ROOT is installed but `root-config` is not found, load ROOT's settings
into this terminal (`<root>` is the folder ROOT is installed in):

```sh
source <root>/bin/thisroot.sh      # in tcsh: source <root>/bin/thisroot.csh
```

ROOT's settings are needed only in the terminal where you build FLIRT.
Once FLIRT is built, its programs find ROOT by themselves.

FLIRT was tested with ROOT 6.36.04: on Linux with ROOT's ready-made
release, on a Mac with ROOT built from source. With another version, or
ROOT installed another way, convert the example run and compare it with
mvme's own files before you trust FLIRT's files, as steps 5 and 6 of the
[Quick start](../README.md#quick-start) show.

## Linux

These are the steps FLIRT was tested with, on Rocky Linux 8. On another
Linux distribution, install the same tools with its own package manager.
FLIRT has not been tested there, so before you trust its files, convert
the example run and compare it with mvme's own files, as steps 5 and 6 of
the [Quick start](../README.md#quick-start) show.

The commands are written for bash, the usual shell on Linux. To see which
shell you have, run `echo $0`. If its answer contains `tcsh`, type `bash`
first. You then work in bash until you type `exit`. This matters because
the `source` line in step 3 works only in bash.

### 1. Install the packages

Installing packages needs administrator rights. If you cannot use `sudo`,
ask your system administrator to install these packages for you:

```sh
sudo dnf -y install gcc-toolset-13-gcc-c++ make cmake git \
    qt5-qtbase-devel boost-devel zlib-devel xz-devel
```

`dnf` is Rocky Linux's package manager, and `-y` answers "yes" to its
question before it installs. The packages are:

- `gcc-toolset-13-gcc-c++`: the C++ compiler GCC 13, which is installed
  next to the system's own compiler without replacing it. FLIRT needs GCC
  9 or newer, because mesytec-mvlc (part of mvme) uses the C++ header
  `<version>`, a part of the C++ standard library that GCC has only from
  version 9 on.
- `make`, `cmake` and `git`: the tools that download and build FLIRT.
- `qt5-qtbase-devel` and `boost-devel`: libraries that mvme's code uses.
  FLIRT uses only Qt's core library, Qt5Core.
- `zlib-devel` and `xz-devel`: compression libraries, which mesytec-mvlc
  uses to read the listfiles' ZIP archives.

To see which of these packages are installed already, run:

```sh
rpm -q gcc-toolset-13-gcc-c++ make cmake git qt5-qtbase-devel boost-devel zlib-devel xz-devel
```

`rpm -q` lists each package with its version, or says that it `is not
installed`. Only the missing ones need installing.

### 2. Download FLIRT

Go to the folder where you want FLIRT, for example a folder named
`software` in your home folder, and download it:

```sh
mkdir -p ~/software
cd ~/software
git clone --recurse-submodules https://github.com/TAMU-Pangolins/FLIRT.git
cd FLIRT
```

`mkdir -p ~/software` makes the folder (`-p`: without an error if it
exists already), and `cd ~/software` goes into it. `git clone` makes a
folder named `FLIRT` there, which holds FLIRT's code.
`--recurse-submodules` also downloads the code FLIRT builds on into
`FLIRT/external`: mvme 1.21.0 (with mesytec-mvlc inside it) and FTXUI
6.1.9, which `flirt-tui` uses. FLIRT uses this code unmodified, at exactly
these versions. If you cloned without `--recurse-submodules`, download that
code now, from the FLIRT folder:

```sh
git submodule update --init --recursive
```

### 3. Build

From the FLIRT folder:

```sh
source /opt/rh/gcc-toolset-13/enable
cmake -S . -B build/linux
make -C build/linux -j N
cmake -S tools/compare -B build/linux-compare
make -C build/linux-compare -j N
```

- `source /opt/rh/gcc-toolset-13/enable` makes this terminal use GCC 13
  from step 1, until you close it. Only the build needs it:
  FLIRT's programs run without it.
- `cmake -S . -B build/linux` prepares the build folder `build/linux`
  (`-B`) for the code in the folder you are in (`-S .`). It checks that
  ROOT, Qt and the compiler can be found; if something is missing, it
  stops and says what.
- `make -C build/linux -j N` compiles FLIRT in that folder (`-C`). Replace
  `N` with how many files it should compile at once, for example `-j 4`:
  any number works, and more is faster up to the number of processor cores
  your computer has. To see that number, run `nproc`. The first build takes
  a few minutes.
- The last two commands do the same for `compare`.

The programs are now in `build/linux/prefix/bin`, and `compare` is in
`build/linux-compare`. Next, see [Running the programs by
name](#running-the-programs-by-name).

During the build, ROOT may print this warning:

```
Warning in cling::IncrementalParser::CheckABICompatibility():
  Possible C++ standard library mismatch, ...
```

You can ignore it. It appears when ROOT was built with a different
compiler from the one that builds FLIRT, as with ROOT's ready-made release
and GCC 13. FLIRT's files made this way were checked and are identical to
mvme's.

### Faster builds with Ninja (optional)

Ninja is a build tool like make that is usually faster, mostly when it
builds again after a small change. On Rocky Linux 8, Ninja is in an extra
package collection named PowerTools, which the first two commands turn on:

```sh
sudo dnf -y install dnf-plugins-core
sudo dnf config-manager --set-enabled powertools
sudo dnf -y install ninja-build
```

Then build with Ninja instead of make, from the FLIRT folder:

```sh
source /opt/rh/gcc-toolset-13/enable
cmake -S . -B build/linux -G Ninja
ninja -C build/linux
cmake -S tools/compare -B build/linux-compare -G Ninja
ninja -C build/linux-compare
```

`-G Ninja` tells CMake to prepare the build folder for Ninja instead of
make. Ninja uses all the processor cores by itself, so it needs no `-j`.

A build folder stays with the tool it was first prepared for. To switch
folders you already built with make, delete them first, from the FLIRT
folder: `rm -r build/linux build/linux-compare`. This also deletes
`flirt-tui`'s saved settings, which are kept in the build folder.

## macOS

FLIRT was tested on macOS 15.4, on a Mac with Apple silicon, with Apple
clang 16, CMake 4.1, Ninja 1.13, Qt 5.15.17, Boost 1.89 and ROOT 6.36.04.
The commands work in zsh, the Mac's usual shell, and in bash.

### 1. Install Apple's command line tools

```sh
xcode-select --install
```

This installs Apple's C++ compiler (Apple clang), `git` and other tools for
the command line. A window asks you to confirm. If the tools are installed
already, the command says so.

### 2. Install the packages with Homebrew

Homebrew is a package manager for macOS. To see whether you have it, run
`brew --version`. If the command is not found, install Homebrew first, as
[brew.sh](https://brew.sh) describes. Then:

```sh
brew install qt@5 boost cmake ninja
```

- `qt@5` and `boost`: libraries that mvme's code uses. FLIRT uses only
  Qt's core library, Qt5Core.
- `cmake`: prepares the build.
- `ninja`: compiles FLIRT when you build with the script. Ninja is a
  build tool like make, but faster.

### 3. Download FLIRT

Go to the folder where you want FLIRT, for example a folder named
`software` in your home folder, and download it:

```sh
mkdir -p ~/software
cd ~/software
git clone --recurse-submodules https://github.com/TAMU-Pangolins/FLIRT.git
cd FLIRT
```

`mkdir -p ~/software` makes the folder (`-p`: without an error if it
exists already), and `cd ~/software` goes into it. `git clone` makes a
folder named `FLIRT` there, which holds FLIRT's code.
`--recurse-submodules` also downloads the code FLIRT builds on into
`FLIRT/external`: mvme 1.21.0 (with mesytec-mvlc inside it) and FTXUI
6.1.9, which `flirt-tui` uses. If you cloned without
`--recurse-submodules`, download that code now, from the FLIRT folder:

```sh
git submodule update --init --recursive
```

### 4. Build

From the FLIRT folder:

```sh
scripts/build_macos.sh
```

The script first checks that the command line tools, Homebrew's packages
and ROOT are there. If something is missing, it stops and says what to
install; it does not install anything itself. Then it builds FLIRT and
`compare` with CMake and Ninja. The first build takes a few minutes.

The script uses only these tools, so that other software on your Mac
cannot get mixed into the build. Anaconda, for example, brings its
own copy of Boost. Anything you type after the script's name is passed on
to CMake (see [Building without flirt-tui](#building-without-flirt-tui)).

The programs are now in `build/macos/prefix/bin`, and `compare` is in
`build/macos-compare`. Next, see [Running the programs by
name](#running-the-programs-by-name).

On a Mac, FLIRT's files are identical to mvme's except for one statistic in
some histograms (`fTsumwx2`), which ROOT rounds very slightly differently
on Apple computers. Step 6 of the [Quick start](../README.md#quick-start)
explains this.

### Building with make instead of the script (optional)

You can also build on a Mac with `cmake` and `make`, as on Linux. Run these
commands from the FLIRT folder, instead of the script:

```sh
cmake -S . -B build/macos -DCMAKE_PREFIX_PATH="$(brew --prefix qt@5);$(brew --prefix boost)"
make -C build/macos -j N
cmake -S tools/compare -B build/macos-compare
make -C build/macos-compare -j N
```

- `cmake -S . -B build/macos` prepares the build folder `build/macos`
  (`-B`) for the code in the folder you are in (`-S .`). It checks that
  ROOT, Qt and the compiler can be found; if something is missing, it
  stops and says what.
- `-DCMAKE_PREFIX_PATH=...` tells CMake to use Homebrew's Qt 5 and Boost.
  `brew --prefix qt@5` and `brew --prefix boost` print the folders
  Homebrew installed them in. Without this setting, CMake may find other
  copies first: on a Mac with Anaconda, for example, it took Anaconda's
  older Boost.
- `make -C build/macos -j N` compiles FLIRT in that folder (`-C`). Replace
  `N` with how many files it should compile at once: more is faster, up to
  the number of processor cores your Mac has, which `sysctl -n hw.ncpu`
  prints.
- The last two commands do the same for `compare`.

`make` comes with Apple's command line tools, so this way does not need
Homebrew's `ninja`. The programs end up in the same folders as with the
script: `build/macos/prefix/bin`, and `build/macos-compare` for `compare`.
One difference from the script: these commands use the tools they find
first on your `PATH`, whichever program put them there. The script uses
only ROOT and the tools from steps 1 and 2.

## Running the programs by name

The build also writes two small files into the build folder, `env.sh` and
`env.csh`. From the FLIRT folder, run the one line that matches your shell
(`echo $0` prints which shell you have; most Linux terminals use bash, a
Mac uses zsh):

```sh
source build/linux/env.sh          # bash or zsh (on a Mac: source build/macos/env.sh)
source build/linux/env.csh         # tcsh (on a Mac: source build/macos/env.csh)
```

After this you can type `flirt-convert` instead of
`build/linux/prefix/bin/flirt-convert`, in this terminal until you close
it. The line adds FLIRT's program folder, and `compare`'s folder if you
have built `compare`, to `PATH`, the list of folders in which your terminal
looks for programs. It changes nothing else, and running it twice does no
harm.

To have the programs available in every new terminal, add the same line,
with the full path of your FLIRT folder, at the end of your shell's
startup file (`pwd` prints the full path while you are in the FLIRT
folder). For example, with FLIRT in `/home/you/software/FLIRT` (on a Mac,
`/Users/you/software/FLIRT`):

| Your shell | Startup file | Line to add |
|---|---|---|
| bash | `~/.bashrc` | `source /home/you/software/FLIRT/build/linux/env.sh` |
| zsh (the Mac's default) | `~/.zshrc` | `source /Users/you/software/FLIRT/build/macos/env.sh` |
| tcsh | `~/.tcshrc`, or `~/.cshrc` if that is the one you have | `source /home/you/software/FLIRT/build/linux/env.csh` |

Open the file in any text editor and add the line at the end. New
terminals then find FLIRT's programs by name; the one already open does
not, until you run the line there too.

To check your build, convert the example run and compare it with mvme's
own files, as steps 5 and 6 of the [Quick start](../README.md#quick-start)
show. Each program lists its options with `--help`, for example
`flirt-convert --help`. The [tutorial](tutorial.md) goes on step by step,
and [usage.md](usage.md) is the full reference.

## Building without flirt-tui

`flirt-tui` is built with FTXUI, which `git clone --recurse-submodules`
downloads together with FLIRT. If you don't need `flirt-tui`, you can
leave it out of the build with `-DFLIRT_BUILD_TUI=OFF`. Add it to the
first `cmake` command, for example on Linux:

```sh
cmake -S . -B build/linux -DFLIRT_BUILD_TUI=OFF
```

With the script on a Mac, add it after the script's name:

```sh
scripts/build_macos.sh -DFLIRT_BUILD_TUI=OFF
```

`-D` sets one of the build's options, and this one turns `flirt-tui` off.
The other programs are built as usual.

## Updating FLIRT

From the FLIRT folder:

```sh
git pull
git submodule update --init --recursive
```

`git pull` downloads the newest version of FLIRT. The second command brings
mvme and FTXUI to the versions that this FLIRT version uses. Then build
again with the same commands as before (on Linux, starting with the
`source /opt/rh/gcc-toolset-13/enable` line). Only what changed is compiled
again, so this is quicker than the first build.

After an update, FLIRT does not skip the runs that an earlier version
converted. It refuses them instead, with the reason `outputs-exist`,
because their reports name the earlier version, and FLIRT never overwrites
a file. To convert those runs with the new version, choose a new output
folder, or delete the runs' files first ([usage.md, "Skipping and
refusing"](usage.md#skipping-and-refusing)).

FLIRT's programs use the ROOT they were built with. If you install another
ROOT version, or move ROOT to another folder, delete the build folders and
build again. From the FLIRT folder, on Linux:

```sh
rm -r build/linux build/linux-compare
```

On a Mac:

```sh
rm -r build/macos build/macos-compare
```

This also deletes `flirt-tui`'s saved settings. With another ROOT version,
check the example run with `compare` again, as steps 5 and 6 of the [Quick
start](../README.md#quick-start) show.

## Removing FLIRT

Delete the FLIRT folder, for example with `rm -r ~/software/FLIRT`. FLIRT
keeps nothing anywhere else: even `flirt-tui`'s settings are inside it. If
you added a `source` line for `env.sh` or `env.csh` to your shell's startup
file, delete that line too. ROOT, which you installed separately, stays
installed.

## If FLIRT cannot use its folder

The full path of the FLIRT folder must not contain `$`, `)` or `;`:

- **`$` and `)`.** ROOT, which FLIRT uses to write the files, has to find
  files inside the FLIRT folder by their path. ROOT reads a `$` in a path as
  the start of a variable name, and removes every `)` from a path that also
  contains one of `~ * ? [ ] { } $`. If ROOT would change the FLIRT
  folder's path, FLIRT does not convert, and says:

  ```
  ROOT cannot use FLIRT's install folder <path>: ROOT would change its path (it reads a '$' as the start of a variable name, and removes every ')' when the path also holds any of ~ * ? [ ] { } $); install FLIRT in a folder whose path has no '$' and no ')'
  ```

- **`;`.** CMake, which prepares the build, reads a `;` as the separator
  between the items of a list. It cannot build FLIRT in a folder whose path
  contains one, and stops with an error before anything is built.

Spaces and characters such as `(`, `'`, `&` and `~` are fine.

If the path contains `$`, `)` or `;`, move the FLIRT folder to a folder
whose path has none of them, or rename the part of the path that has one.
Then delete the build folders and build again (see [Updating
FLIRT](#updating-flirt)), because the build remembers the old path.

This rule is only for FLIRT's own folder. It does not apply to your
listfiles or output folders: the folders above them may have any name.
Only the name of a listfile itself and the name of the output folder
itself must be plain: letters, digits, `_`, `-`, `.` and spaces
([usage.md, "Names"](usage.md#names)).
