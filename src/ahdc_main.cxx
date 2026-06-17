// ahdc_beamspot -- CLAS12 beam-spot analysis from AHDC::kftrack tracks.
//
// Same engine and outputs as `beamspot`, but the per-event selection takes
// AHDC central tracks instead of forward-detector electrons (see
// include/ahdc_beam_spot.h for the method and its tan(theta) caveat).
//
// Usage:
//   ahdc_beamspot [options] FILES...
// See --help for the full option list.

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include <TROOT.h>

#include "ahdc_beam_spot.h"
#include "beam_spot.h"
#include "cli.h"

namespace fs = std::filesystem;

namespace {

// process exit codes (named, never bare integers)
enum class exit_code : int {
  success          = 0,
  no_input         = 1,
  output_dir_error = 2,
  input_error      = 3,
  write_error      = 4,
};
constexpr int code(exit_code e) { return static_cast<int>(e); }

struct options {
  std::string output_dir      = "beamspot_ahdc_out";
  std::string prefix          = "BeamSpotAHDC";
  double      fit_range       = 1.0;
  double      target_z        = 0.0;     // AHDC default (cm); see ahdc_default_config()
  int         bins_per_sector = 10;
  int         jobs            = 0;        // worker processes for the fill; 0 = all hardware cores
  double      z_map_min       = -10.0;    // 2D z-vs-phi map z range (drives the fits)
  double      z_map_max       = 10.0;
  double      z_1d_min        = -10.0;     // 1D z-vertex QA histogram z range
  double      z_1d_max        = 10.0;
  int         min_hits        = 4;        // AHDC::kftrack n_hits floor
  double      max_chi2        = 50.0;     // AHDC::kftrack chi2 ceiling (mm^2)
  bool        merge           = false;
  bool        dry_run         = false;
  std::vector<std::string> inputs;
};

}  // namespace

int main(int argc, char** argv) {
  options opt;

  cli::parser app("ahdc_beamspot", "ahdc_beamspot -- CLAS12 beam-spot analysis (AHDC tracks)");
  app.add_option("-o", "--output-dir",      opt.output_dir,      "Output folder (created if missing)");
  app.add_option("-p", "--prefix",          opt.prefix,          "Filename prefix");
  app.add_option("-r", "--fit-range",       opt.fit_range,       "Fit-range scale factor");
  app.add_option("-z", "--target-z",        opt.target_z,        "Nominal target/foil Z (cm)");
  app.add_option("-n", "--bins-per-sector", opt.bins_per_sector, "Phi bins per sector");
  app.add_option("-j", "--jobs",            opt.jobs,            "Worker processes for the fill (0 = all cores)");
  app.add_option("",   "--z-min",           opt.z_map_min,       "z-vs-phi map: min Z (cm)");
  app.add_option("",   "--z-max",           opt.z_map_max,       "z-vs-phi map: max Z (cm)");
  app.add_option("",   "--z1d-min",         opt.z_1d_min,        "1D z-vertex QA histogram: min Z (cm)");
  app.add_option("",   "--z1d-max",         opt.z_1d_max,        "1D z-vertex QA histogram: max Z (cm)");
  app.add_option("",   "--min-hits",        opt.min_hits,        "AHDC::kftrack minimum n_hits");
  app.add_option("",   "--max-chi2",        opt.max_chi2,        "AHDC::kftrack maximum chi2 (mm^2)");
  app.add_flag  ("-m", "--merge-histograms", opt.merge,   "Treat inputs as ROOT results files and Add() them");
  app.add_flag  ("",   "--dry-run",          opt.dry_run, "Run the analysis but write no files");
  app.add_positional("files", opt.inputs, "Input DST files, or ROOT results files with -m");
  app.set_version("ahdc_beamspot 1.0");
  app.parse(argc, argv);

  if (opt.inputs.empty()) {
    fmt::print(stderr, "ahdc_beamspot: ERROR: no input files specified.\n\n{}", app.usage());
    return code(exit_code::no_input);
  }

  ROOT::EnableThreadSafety();

  // path helper: every output path is assembled here, never by hand
  const fs::path output_dir(opt.output_dir);
  if (!opt.dry_run) {
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec) {
      fmt::print(stderr, "ahdc_beamspot: ERROR: cannot create output dir '{}': {}\n",
                 output_dir.string(), ec.message());
      return code(exit_code::output_dir_error);
    }
  }
  const auto out = [&](const std::string& suffix) {
    return (output_dir / (opt.prefix + suffix)).string();
  };

  const unsigned hw = std::thread::hardware_concurrency();
  const int n_jobs = opt.jobs > 0 ? opt.jobs : (hw > 0 ? static_cast<int>(hw) : 1);

  // central-tracker defaults (theta bins) with the CLI-tunable fields applied
  beamspot::config cfg = beamspot::ahdc_default_config();
  cfg.fit_range_scale = opt.fit_range;
  cfg.target_z        = opt.target_z;
  cfg.bins_per_sector = opt.bins_per_sector;
  cfg.z_map_min       = opt.z_map_min;
  cfg.z_map_max       = opt.z_map_max;
  cfg.z_1d_min        = opt.z_1d_min;
  cfg.z_1d_max        = opt.z_1d_max;

  // functional pipeline: fill/merge -> analyze -> results -> write
  beamspot::analysis a;
  try {
    const beamspot::ahdc_selector sel(beamspot::ahdc_cuts{opt.min_hits, opt.max_chi2});
    beamspot::histograms histos = opt.merge ? beamspot::merge_files(opt.inputs, cfg)
                                            : beamspot::fill_dst(opt.inputs, cfg, sel, n_jobs);
    a = beamspot::analyze(std::move(histos), cfg);
  } catch (const std::exception& e) {
    fmt::print(stderr, "ahdc_beamspot: ERROR while reading input: {}\n", e.what());
    return code(exit_code::input_error);
  }

  const beamspot::beam_spot_result r = beamspot::results(a);
  fmt::print("\nBeam-spot results (AHDC):\n"
             "  Z    = {} +- {} cm\n"
             "  R    = {} +- {} cm\n"
             "  Phi0 = {} +- {} deg\n"
             "  X    = {} +- {} cm\n"
             "  Y    = {} +- {} cm\n",
             r.z.v, r.z.e, r.r.v, r.r.e, r.phi.v, r.phi.e, r.x.v, r.x.e, r.y.v, r.y.e);

  if (opt.dry_run) {
    fmt::print("\n(dry-run: no files written)\n");
    return code(exit_code::success);
  }

  try {
    beamspot::write_root(a, out("_results.root"));
    beamspot::write_results_txt(a, out("_results.txt"));
    beamspot::write_ccdb_table(a, out("_ccdb_table.txt"));
  } catch (const std::exception& e) {
    fmt::print(stderr, "ahdc_beamspot: ERROR while writing output: {}\n", e.what());
    return code(exit_code::write_error);
  }
  fmt::print("\nWrote:\n  {}\n  {}\n  {}\n",
             out("_results.root"), out("_results.txt"), out("_ccdb_table.txt"));

  return code(exit_code::success);
}
