# compare

`compare` checks whether two ROOT files hold the same data. You give it a
file FLIRT made and the file mvme made from the same run. It compares them
value by value and tells you whether they are identical, and if not, what
differs.

`compare` is built on its own and shares no code with FLIRT's converter.
That way, a mistake in the converter cannot hide itself in the check.

## Build it

[docs/building.md](../../docs/building.md) also builds `compare`, in its
own build folder. The program is then `build/linux-compare/compare` on Linux and
`build/macos-compare/compare` on a Mac. After you load the build's
`env.sh` or `env.csh`, you can type just `compare`.

## Run it

Give it FLIRT's `_raw.root` file first and mvme's second. To compare the
histograms too, add `--histos` with FLIRT's `_histos.root` file and
mvme's. For example, with the example run (README, Quick start, step 6):

```sh
compare --allow-fused-sumwx2 \
    practice/as-mvme/example_run_raw.root examples/official/example_run_raw.root \
    --histos practice/as-mvme/example_run_histos.root examples/official/example_run_histos.root
```

On Linux the last line is `IDENTICAL`, and on a Mac `IDENTICAL EXCEPT
fTsumwx2 fused rounding in 83 histograms` (see [FLIRT ran on a
Mac](#flirt-ran-on-a-mac---allow-fused-sumwx2)).

All the options:

```sh
compare <cand_raw.root> <ref_raw.root> \
    [--histos <cand_histos.root> <ref_histos.root>] [--max-report N] \
    [--ignore-branch-regex <regex>] [--allow-fused-sumwx2] [--ignore-run-id] [--data-only]
```

- `cand` is the candidate, the file to check (FLIRT's). `ref` is the
  reference, the file to check it against (mvme's).
- `--histos` also compares the two `_histos.root` files.
- `--max-report N` lists up to N differences for each check (10 if you
  leave it out).
- Four options leave out one known kind of difference. You need them only
  in the cases their sections below describe:
  - `--ignore-branch-regex`: FLIRT left some branches out on purpose;
  - `--data-only`: FLIRT wrote the files with another compression;
  - `--ignore-run-id`: mvme wrote its files during the run;
  - `--allow-fused-sumwx2`: FLIRT ran on a Mac with Apple silicon.

## Read the result

`compare` prints one line for each check, starting with `PASS` or `FAIL`.
Under a failed check, it lists the differences it found. Lines that start
with `info:` give extra facts and never change the result.

The last line is the verdict, followed by the time `compare` took:

| Verdict | Meaning |
|---|---|
| `IDENTICAL` | The files hold exactly the same data. |
| `IDENTICAL EXCEPT ...` | The same, apart from what an option told `compare` to leave out. The rest of the line says what. |
| `SAME DATA (layout not compared)` | With `--data-only`: every value is the same. Only the way ROOT packed the data was not compared. |
| `DIFFERENT` | Something differs. The `FAIL` lines above say what. |

The exit code says the same for scripts: `0` for identical (or the same
data, with `--data-only`), `1` for different, and `2` when the command was
wrong or a file could not be read.

[docs/usage.md, "Verifying against official output"](../../docs/usage.md#verifying-against-official-output)
says which options to use for which of mvme's files.

## What it checks

Every check is exact: numbers must match bit for bit.

| Check | What must be the same |
|---|---|
| keys | The list of objects stored in the file (trees, histograms, run information), in every folder of the file. |
| streamer info | ROOT's description of the layout of the experiment's own classes, the ones that hold the events. ROOT's own classes are only listed as `info:`, because they can differ between ROOT versions. |
| MVMERunInfo | mvme's run information: the run's name (`RunID`) and the experiment's name (`ExperimentName`). |
| tree layout | How each tree is built: its branches, their types, and how ROOT stored them in the file. |
| tree values | Every value of every event. Two "not a number" values count as equal only when their bits are equal. |
| histos | With `--histos`: the same keys and layouts, and for each histogram its axes, every bin (including the bins for values below and above the range), and the sums ROOT keeps with it. |

`compare` does not compare what always differs between two conversions:
the file's unique ID, the dates and times stored in it, and how many bytes
the compressed data take. The byte counts are listed as `info:` lines; the
ID and the times are not shown.

## FLIRT left some branches out (`--ignore-branch-regex`)

By default, FLIRT leaves out the waveform branches (`mdpp_a_samples` and
the like) that mvme writes, because it writes the complete traces to its
`_waveforms.root` file instead. To compare everything else, tell
`compare` which names to leave out:

```sh
compare --ignore-branch-regex _samples cand_raw.root ref_raw.root
```

Every branch, class member and histogram whose name contains a match for
`<regex>` (here `_samples`) is then left out on both sides. `<regex>` is
a regular expression, so a plain word like `_samples` simply matches
names that contain it.

Leaving data out changes how ROOT packs the rest into blocks, so `compare`
then does not compare that packing. It still compares every value that is
not left out. The first few packing differences of each tree are listed
as `info:` lines starting with `IGNORED`. The verdict reads `IDENTICAL
EXCEPT IGNORED '_samples' (basket layout not compared)`.

## FLIRT used another compression (`--data-only`)

`flirt-convert --compression fast` or `small` makes ROOT pack the data
differently. The values are the same, but in a larger run ROOT then splits
the trees into blocks at other places, and without `--data-only`
`compare` says `DIFFERENT`. (The small example run is too small for that,
so it is `IDENTICAL` even without the option.) `--data-only`
compares what matters and leaves the packing out:

- still compared exactly: the list of objects, the classes, the run
  information, how each tree is built, every value of every event, and
  every histogram;
- not compared: how ROOT split the data into blocks, how many bytes they
  take, and the element ids.

Each tree gets an `info:` line starting with `NOT COMPARED` that says how
many packing details differ, followed by the first few of them. The
verdict reads `SAME DATA (layout not compared)`. Any different value still
makes it `DIFFERENT`: a copy with one changed bin was `DIFFERENT`.

## mvme wrote its files during the run (`--ignore-run-id`)

mvme can write its ROOT files live, while it records the run. It then
names the run after its start time, for example `260303_141005`. A
replay of the listfile, and FLIRT, name it after the listfile, for example
`run042`. That name is stored in the run information as `RunID`.

`--ignore-run-id` leaves out just that one entry. Everything else,
including the experiment's name, is still compared. The verdict reads
`IDENTICAL EXCEPT RunID`.

## FLIRT ran on a Mac (`--allow-fused-sumwx2`)

Each histogram keeps a few sums of its values. One of them, `fTsumwx2`
(a sum of squares), can differ in its very last digit between a Mac with
Apple silicon and Linux. The reason is ROOT itself: on these processors
it computes each step of that sum with one rounding instead of two
([docs/design.md](../../docs/design.md), "macOS (Apple silicon)").

With `--allow-fused-sumwx2`, `compare` computes the sum again both ways,
from the values in FLIRT's raw file. It accepts a difference only if one
file holds exactly the one result and the other file exactly the other.
Nothing else is accepted: any other difference in this sum, or in any
other number, is still a `FAIL`. Each accepted histogram gets an `info:`
line, and the verdict reads `IDENTICAL EXCEPT fTsumwx2 fused rounding in N
histograms`. On Linux the option changes nothing.

## When several options apply

The verdict names everything that was left out, in this order: `RunID`,
then `IGNORED '<regex>' (...)`, then `fTsumwx2 fused rounding ...`, joined
by `AND`. For example: `IDENTICAL EXCEPT IGNORED '_samples' (basket
layout not compared) AND fTsumwx2 fused rounding in 20 histograms`. With
`--data-only`, it is `SAME DATA (layout not compared) EXCEPT ...`.

## How it works (for programmers)

- `compare` reads the files with ROOT only. It does not need the class
  library that FLIRT generates for the experiment: it reads the trees in
  ROOT's MakeClass mode, and binds each branch that has no sub-branches
  through its `TBranch` pointer. Binding by name would go wrong, because
  sub-branch names such as `fName` repeat in every module.
- Values are compared whenever the branch paths and leaf types match,
  even if the layout check failed, so a layout difference never hides
  what the values show.
- The tree layout check covers, per tree and cycle: the title, entries,
  `fAutoFlush`, `fAutoSave`, `fEstimate`, `fMaxEntryLoop`,
  `fMaxVirtualSize` and `fTotBytes`; and per branch: the class, element
  class, streamer type, type, id, leaf types, split level, basket size,
  entries, `fTotBytes`, number of baskets and each basket's first entry.
  The value check includes `fUniqueID`, `fBits`, `fName` and `fTitle`.
  The histogram check covers the class, title, axes, every bin, `fEntries`,
  `fTsumw`, `fTsumw2`, `fTsumwx`, `fTsumwx2` and `fSumw2`.
- `--ignore-branch-regex` uses C++'s ECMAScript regular expressions. With
  it, and with `--data-only`, these fields are not compared: `fAutoFlush`,
  `fTotBytes`, the basket sizes, the number of baskets and where they
  start, and the element ids (they shift when members are left out). A
  class's checksum is not compared when one of its members was left out.
- `--allow-fused-sumwx2` (`fused_sumwx2.cc`) refills each histogram's sum
  from the candidate's raw tree in fill order, following `TH1::Fill`: NaN
  values are skipped, and values in the underflow and overflow bins do not
  count. Histogram k of an event's folder is filled from the k-th value of
  the event's tree entry, in the order `mvme_root_client` creates them
  (modules, then data sources, then indices). The separate sum rounds
  `x*x` before adding it (the file is compiled with `-ffp-contract=off`),
  the fused one is `std::fma(x, x, sum)`.
- Not stored in the file, so not compared: `fMaxTreeSize`, a setting of
  the whole ROOT process.
