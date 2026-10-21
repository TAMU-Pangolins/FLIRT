# The example run

`example_run.zip` is a made-up run for trying FLIRT, used by the
[tutorial](../docs/tutorial.md). It has the form of a real mvme 1.18.0
recording (MVLC over USB, one event with three MDPP-32 modules, `mdpp_a`,
`mdpp_b` and `mdpp_c`, and an event stamper), about 20 seconds long and
dated 3 March 2026. Its readings and traces are invented:

- `mdpp_c` shows a gamma spectrum with peaks at the energies of the
  Eu-152 gamma lines, on a falling background, through the calibration
  amplitude = 32 × energy in keV;
- `mdpp_a` and `mdpp_b` hold made-up distributions;
- every recorded trace is a made-up pulse whose height is 0.1 × its hit's
  amplitude.

`official/` holds the ROOT files mesytec's own software made from it:
mvme 1.18.0 for Linux replaying the listfile, with
`mvme_root_client --single-run`. Converted with
`--official-waveform-branches`, FLIRT's files are identical to them (on a
Mac apart from the rounding of one histogram statistic, see the README's
"What FLIRT promises").
