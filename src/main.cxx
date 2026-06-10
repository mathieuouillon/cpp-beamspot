// BeamSpot -- CLAS12 beam-spot analysis (headless C++/ROOT port).
//
// Usage:
//   beamspot [options] FILES...
// See --help for the full option list.

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include <TROOT.h>

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
  std::string output_dir      = "beamspot_out";
  std::string prefix          = "BeamSpot";
  double      fit_range       = 1.0;
  double      target_z        = 25.4;
  int         bins_per_sector = 10;
  int         threads         = 0;  // 0 = all hardware cores
  bool        merge           = false;
  bool        dry_run         = false;
  std::vector<std::string> inputs;
};

}  // namespace

int main(int argc, char** argv) {
  options opt;

  cli::parser app("beamspot", "BeamSpot -- CLAS12 beam-spot analysis");
  app.add_option("-o", "--output-dir",      opt.output_dir,      "Output folder (created if missing)");
  app.add_option("-p", "--prefix",          opt.prefix,          "Filename prefix");
  app.add_option("-r", "--fit-range",       opt.fit_range,       "Fit-range scale factor");
  app.add_option("-z", "--target-z",        opt.target_z,        "Nominal target/foil Z (cm)");
  app.add_option("-n", "--bins-per-sector", opt.bins_per_sector, "Phi bins per sector");
  app.add_option("-j", "--threads",         opt.threads,         "Worker threads for the fill (0 = all cores)");
  app.add_flag  ("-m", "--merge-histograms", opt.merge,   "Treat inputs as ROOT results files and Add() them");
  app.add_flag  ("",   "--dry-run",          opt.dry_run, "Run the analysis but write no files");
  app.add_positional("files", opt.inputs, "Input DST files, or ROOT results files with -m");
  app.set_version("beamspot 1.0");
  app.parse(argc, argv);

  if (opt.inputs.empty()) {
    fmt::print(stderr, "beamspot: ERROR: no input files specified.\n\n{}", app.usage());
    return code(exit_code::no_input);
  }

  ROOT::EnableThreadSafety();

  // path helper: every output path is assembled here, never by hand
  const fs::path output_dir(opt.output_dir);
  if (!opt.dry_run) {
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec) {
      fmt::print(stderr, "beamspot: ERROR: cannot create output dir '{}': {}\n",
                 output_dir.string(), ec.message());
      return code(exit_code::output_dir_error);
    }
  }
  const auto out = [&](const std::string& suffix) {
    return (output_dir / (opt.prefix + suffix)).string();
  };

  const unsigned hw = std::thread::hardware_concurrency();
  const int n_threads = opt.threads > 0 ? opt.threads : (hw > 0 ? static_cast<int>(hw) : 1);

  const beamspot::config cfg{{10, 11, 12, 13, 14, 16, 18, 22, 30},
                             opt.fit_range, opt.target_z, opt.bins_per_sector};

  // functional pipeline: fill/merge -> analyze -> results -> write
  beamspot::analysis a;
  try {
    beamspot::histograms histos = opt.merge ? beamspot::merge_files(opt.inputs, cfg)
                                            : beamspot::fill_dst(opt.inputs, cfg, n_threads);
    a = beamspot::analyze(std::move(histos), cfg);
  } catch (const std::exception& e) {
    fmt::print(stderr, "beamspot: ERROR while reading input: {}\n", e.what());
    return code(exit_code::input_error);
  }

  const beamspot::beam_spot_result r = beamspot::results(a);
  fmt::print("\nBeam-spot results:\n"
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
    fmt::print(stderr, "beamspot: ERROR while writing output: {}\n", e.what());
    return code(exit_code::write_error);
  }
  fmt::print("\nWrote:\n  {}\n  {}\n  {}\n",
             out("_results.root"), out("_results.txt"), out("_ccdb_table.txt"));

  return code(exit_code::success);
}
