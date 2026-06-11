#include "beam_spot.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>

#include <fmt/format.h>

#include <TDirectory.h>
#include <TFile.h>
#include <TMath.h>
#include <TTree.h>

#include <hipo4/chain.h>
#include <hipo4/progresstracker.hpp>
#include <hipo4/reader.h>

namespace beamspot {

namespace {

// gaussian peak on a linear background: amp*exp(-0.5((x-mean)/sigma)^2)+c+d*x
// (ROOT's "gaus" is [0]*exp(...); "pol1(3)" is [3]+[4]*x).
constexpr const char* gaus_plus_line = "gaus(0)+pol1(3)";

// groot histogram accessors are 0-indexed (no under/overflow); ROOT is
// 1-indexed with bin 0 = underflow. This translates a groot bin index to a
// ROOT bin index so the ported logic matches the Java bin-by-bin.
[[nodiscard]] double groot_bin_center(const TH1& h, int groot_idx) {
  return h.GetBinCenter(groot_idx + 1);
}

// weighted mean of a 1D histogram restricted to [lo, hi].
[[nodiscard]] double mean_in_interval(TH1& h, double lo, double hi) {
  hi = std::min(hi, h.GetXaxis()->GetXmax() - 1e-5);
  lo = std::max(lo, h.GetXaxis()->GetXmin() + 1e-5);
  double s = 0.0;
  double n = 0.0;
  for (int b = h.FindBin(lo); b <= h.FindBin(hi); ++b) {
    s += h.GetBinCenter(b) * h.GetBinContent(b);
    n += h.GetBinContent(b);
  }
  return s / n;
}

// weighted rms of a 1D histogram restricted to [lo, hi].
[[nodiscard]] double rms_in_interval(TH1& h, double lo, double hi) {
  const double m = mean_in_interval(h, lo, hi);
  hi = std::min(hi, h.GetXaxis()->GetXmax() - 1e-5);
  lo = std::max(lo, h.GetXaxis()->GetXmin() + 1e-5);
  double s = 0.0;
  double n = 0.0;
  for (int b = h.FindBin(lo); b <= h.FindBin(hi); ++b) {
    s += (h.GetBinCenter(b) - m) * (h.GetBinCenter(b) - m) * h.GetBinContent(b);
    n += h.GetBinContent(b);
  }
  return std::sqrt(s / n);
}

// fit a constant (pol0) to a summary graph, seeded with the sample mean/spread;
// the graph takes ownership of the fitted function.
void fit_pol0(TGraphErrors& g) {
  const int n = g.GetN();
  if (n == 0) return;

  double mean = 0.0;
  for (int i = 0; i < n; ++i) mean += g.GetPointY(i);
  mean /= n;

  double variance = 0.0;
  for (int i = 0; i < n; ++i) variance += (g.GetPointY(i) - mean) * (g.GetPointY(i) - mean);
  const double sigma = std::sqrt(variance / n);

  auto* f = new TF1(fmt::format("pol0_{}", g.GetName()).c_str(), "[0]", g.GetPointX(0), g.GetPointX(n - 1));
  f->SetParameter(0, mean);
  f->SetParError(0, 2 * sigma);
  f->SetLineColor(kRed);
  f->SetLineWidth(2);
  g.Fit(f, "Q");
}

// pol0 constant + error from a summary graph's attached fit ({0,0} if none).
[[nodiscard]] value pol0_constant(const TGraphErrors* g) {
  if (g == nullptr || g->GetListOfFunctions()->GetSize() == 0) return {};
  const auto* f = dynamic_cast<const TF1*>(g->GetListOfFunctions()->First());
  return f != nullptr ? value{f->GetParameter(0), f->GetParError(0)} : value{};
}

// run the track/particle cuts for one event and invoke fill(bin, vz, phi) for
// every accepted electron track. Shared by every fill backend.
template <class Fill>
void for_each_hit(const hipo::bank& part, const hipo::bank& trk, const config& cfg, Fill&& fill) {
  const int n_tracks    = trk.getRows();
  const int n_particles = part.getRows();
  if (n_particles == 0 || n_tracks == 0) return;

  for (int i = 0; i < n_tracks; ++i) {
    // track cuts: forward-detector (DC) negative tracks only
    if (trk.getByte("detector", i) != cuts::dc_detector_id) continue;
    if (trk.getByte("q", i) > 0) continue;

    const int pindex = trk.getShort("pindex", i);
    if (pindex < 0 || pindex >= n_particles) continue;

    // particle cuts: an electron above the momentum threshold
    if (part.getInt("pid", pindex) != cuts::electron_pid) continue;
    const float px = part.getFloat("px", pindex);
    const float py = part.getFloat("py", pindex);
    const float pz = part.getFloat("pz", pindex);
    if (px * px + py * py + pz * pz < cuts::min_momentum_sq) continue;

    // phi and theta from the transverse momentum components
    auto phi = static_cast<float>(TMath::RadToDeg() * std::atan2(py, px));
    if (phi < 0)   phi += 360.0F;  // map [-180,180) -> [0,360)
    if (phi > 330) phi -= 360.0F;  // pop the split sector back together

    const auto theta = static_cast<float>(
        TMath::RadToDeg() * std::atan2(std::sqrt(px * px + py * py), pz));

    // find the theta bin: first edge greater than theta, minus one
    const auto upper = std::upper_bound(cfg.theta_bins.begin(), cfg.theta_bins.end(), theta);
    const auto bin   = std::distance(cfg.theta_bins.begin(), upper) - 1;
    if (bin < 0 || static_cast<std::size_t>(bin) >= cfg.n_theta()) continue;

    fill(static_cast<std::size_t>(bin), part.getFloat("vz", pindex), phi);
  }
}

// analyse one theta bin: fit the z slices, then the phi modulation. Appends the
// kept slices (each with its attached fit) to `slices_out`, sets the modulation
// fit on `g_results`, and returns that fit (owned by `g_results`).
TF1* analyze_theta_bin(std::size_t bin, TH2F& h2, TGraphErrors& g_results,
                       std::vector<std::unique_ptr<TH1>>& slices_out, const config& cfg) {
  const double xmin = cfg.target_z - cuts::peak_half_window;
  const double xmax = cfg.target_z + cuts::peak_half_window;

  const int n_phi = h2.GetNbinsY();
  int n_points = 0;

  // loop over the phi bins (ROOT 1-indexed) and fit the target window in z.
  // h2 has X = z, Y = phi; the per-phi z distribution is ProjectionX over a
  // single Y(phi) bin -- this is the analog of groot's H2F.sliceY(i).
  for (int iy = 1; iy <= n_phi; ++iy) {
    std::unique_ptr<TH1> h{h2.ProjectionX(fmt::format("h_slice_{}_{}", bin, iy).c_str(), iy, iy)};
    h->SetDirectory(nullptr);
    h->SetTitle("");

    if (h->Integral() < cuts::min_slice_integral) continue;

    // require the maximum to sit within the expected target-window range
    const double hmax = h->GetBinCenter(h->GetMaximumBin());
    if (hmax < xmin || hmax > xmax) continue;

    const double rms  = rms_in_interval(*h, hmax - 5.0, hmax + 5.0);
    double       rmin = hmax - 2.0 * rms * cfg.fit_range_scale;
    double       rmax = hmax + 1.5 * rms * cfg.fit_range_scale;

    // truncate the fit range to the axis (mirrors the groot bin indexing)
    const int nx = h->GetNbinsX();
    rmin = std::max(rmin, groot_bin_center(*h, 1));       // ROOT bin 2
    rmax = std::min(rmax, groot_bin_center(*h, nx - 1));  // ROOT bin nx

    // need enough entries inside the fit window
    if (h->Integral(h->FindBin(rmin), h->FindBin(rmax)) < cuts::min_fit_integral) continue;

    // ROOT attaches the fitted function to the histogram and owns it, so the
    // raw `new` here is freed when `h` (or its owning unique_ptr) is destroyed.
    auto* func = new TF1(fmt::format("func_{}_{}", bin, iy).c_str(), gaus_plus_line, rmin, rmax);
    func->SetParameters(h->GetBinContent(h->GetMaximumBin()), hmax, rms / 2.0, 1.0, 0.01);
    h->Fit(func, "QR");  // R: use the function's range, Q: quiet

    // quality cuts on the fit (amplitude, sigma band, reduced chi2 band)
    const double amp   = func->GetParameter(0);
    const double sigma = std::abs(func->GetParameter(2));
    const double ndf   = func->GetNDF();
    const double rchi2 = ndf > 0 ? func->GetChisquare() / ndf : 1e9;
    if (amp < cuts::min_amplitude ||
        sigma < cuts::min_sigma || sigma > cuts::max_sigma ||
        rchi2 < cuts::min_chi2_ndf || rchi2 > cuts::max_chi2_ndf) {
      continue;
    }

    // store the fit result: x = phi bin center, y = fitted mean
    g_results.SetPoint(n_points, h2.GetYaxis()->GetBinCenter(iy), func->GetParameter(1));
    g_results.SetPointError(n_points, 0.0, func->GetParError(1));
    ++n_points;

    slices_out.push_back(std::move(h));  // keep the slice (with its attached fit)
  }

  // modulation of the target z position vs phi: z0 - A cos(phi - phi0)
  auto* mod = new TF1(fmt::format("modfit_{}", bin).c_str(), "[0]-[1]*cos(x*pi/180.0-[2])", -30.0, 330.0);
  mod->SetParameters(28.0, 2.0, 0.0);
  mod->SetParNames("z_0", "A", "phi_0");
  mod->SetLineColor(kRed);
  mod->SetLineWidth(3);
  if (g_results.GetN() > 0) {
    g_results.Fit(mod, "Q");  // graph takes ownership of mod
  } else {
    g_results.GetListOfFunctions()->Add(mod);  // empty bin: hand ownership to the graph
  }
  return mod;
}

// fill a single set of histograms by streaming one chain sequentially. If
// `counter` is non-null it is bumped once per event read -- the multi-process
// fill points it at a shared-memory slot so the parent can total live progress.
histograms fill_dst_sequential(const std::vector<std::string>& files, const config& cfg,
                               std::atomic<long>* counter) {
  histograms result = make_histograms(cfg);

  hipo::chain ch(/*threads=*/1, /*progress=*/false, /*verbose=*/false);
  for (const auto& f : files) ch.add(f);
  hipo::banklist banks = ch.getBanks({"REC::Particle", "REC::Track"});
  const auto i_particle = hipo::getBanklistIndex(banks, "REC::Particle");
  const auto i_track    = hipo::getBanklistIndex(banks, "REC::Track");

  for (auto&& [event, file_index, event_index] : ch) {
    event.readBanks(banks);
    for_each_hit(banks[i_particle], banks[i_track], cfg, [&](std::size_t bin, float vz, float phi) {
      result.h1_z->Fill(vz);
      result.h1_phi->Fill(phi);
      result.h2_z_phi[bin]->Fill(vz, phi);
    });
    if (counter != nullptr) counter->fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

// dump just the raw fill histograms (h1_z, h1_phi, h2_z_phi_*) so a parent
// process can sum them back with merge_root_files(). Used by each fill worker.
void write_partial_histograms(const histograms& h, const std::string& path) {
  const std::unique_ptr<TFile> f{TFile::Open(path.c_str(), "RECREATE")};
  if (!f || f->IsZombie()) {
    throw std::runtime_error("beamspot: cannot open partial output file: " + path);
  }
  f->cd();
  h.h1_z->Write();
  h.h1_phi->Write();
  for (const auto& hh : h.h2_z_phi) hh->Write();
  f->Close();
}

// sum the per-theta TH2F (and the 1D summaries) from a list of results/partial
// ROOT files into a fresh histogram set, using TH1::Add (errors in quadrature),
// so the merged result is bit-identical to a single-pass fill.
histograms merge_root_files(const std::vector<std::string>& root_files, const config& cfg, bool verbose) {
  histograms h = make_histograms(cfg);
  for (const auto& filename : root_files) {
    if (verbose) fmt::print("Merging: {} ...\n", filename);
    const std::unique_ptr<TFile> f{TFile::Open(filename.c_str(), "READ")};
    if (!f || f->IsZombie()) {
      throw std::runtime_error("beamspot: cannot open histogram file: " + filename);
    }
    for (std::size_t i = 0; i < h.h2_z_phi.size(); ++i) {
      auto* src = f->Get<TH2F>(fmt::format("h2_z_phi_{}", i).c_str());
      if (src == nullptr) {
        throw std::runtime_error(fmt::format("beamspot: missing h2_z_phi_{} in {}", i, filename));
      }
      h.h2_z_phi[i]->Add(src);
    }
    if (auto* hz = f->Get<TH1F>("h1_z"))   h.h1_z->Add(hz);
    if (auto* hp = f->Get<TH1F>("h1_phi")) h.h1_phi->Add(hp);
  }
  return h;
}

// split files into `n_jobs` groups, balanced by event count (greedy
// least-loaded-first). Unreadable files are dropped with a warning. Returns the
// groups (empty groups removed) and sets `total_events` to the grand total.
std::vector<std::vector<std::string>> split_files(const std::vector<std::string>& files,
                                                  int n_jobs, long& total_events) {
  struct counted { std::string name; long events; };
  std::vector<counted> counts;
  counts.reserve(files.size());
  for (const auto& f : files) {
    try {
      hipo::reader r;
      r.open(f.c_str());
      counts.push_back({f, static_cast<long>(r.getEntries())});
    } catch (const std::exception& e) {
      fmt::print(stderr, "[beamspot] skipping unreadable file '{}': {}\n", f, e.what());
    }
  }

  // largest files first makes the greedy assignment balance much better
  std::sort(counts.begin(), counts.end(),
            [](const counted& a, const counted& b) { return a.events > b.events; });

  const int groups = std::clamp(n_jobs, 1, std::max<int>(1, static_cast<int>(counts.size())));
  std::vector<std::vector<std::string>> out(groups);
  std::vector<long> load(groups, 0);
  total_events = 0;
  for (const auto& c : counts) {
    const auto k = std::distance(load.begin(), std::min_element(load.begin(), load.end()));
    out[k].push_back(c.name);
    load[k] += c.events;
    total_events += c.events;
  }
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const auto& g) { return g.empty(); }),
            out.end());
  return out;
}

// fill by forking one worker process per file group, each running its own chain
// single-threaded. On ifarm this beats in-process threads: no shared ROOT/heap
// contention, and the kernel schedules independent readers across cores. Each
// worker writes a partial-histogram ROOT file; the parent shows aggregate live
// progress (via a shared-memory counter per worker) then merges the partials.
histograms fill_dst_multiprocess(const std::vector<std::string>& files, const config& cfg,
                                 int n_jobs, long& total_events) {
  const auto groups = split_files(files, n_jobs, total_events);
  const int  n_proc = static_cast<int>(groups.size());
  if (n_proc == 0) {
    throw std::runtime_error("beamspot: no readable input files");
  }
  if (n_proc == 1) {
    // nothing to parallelise -- skip the fork/merge overhead entirely
    std::atomic<long> ctr{0};
    histograms h = fill_dst_sequential(groups.front(), cfg, &ctr);
    total_events = ctr.load();
    return h;
  }

  // shared-memory array of per-worker progress counters (visible across fork)
  const std::size_t shm_bytes = sizeof(std::atomic<long>) * static_cast<std::size_t>(n_proc);
  void* shm = mmap(nullptr, shm_bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (shm == MAP_FAILED) {
    throw std::runtime_error("beamspot: mmap failed for shared progress counters");
  }
  auto* counters = static_cast<std::atomic<long>*>(shm);
  for (int k = 0; k < n_proc; ++k) new (&counters[k]) std::atomic<long>(0);

  // per-run scratch dir for the partial histogram files
  const std::filesystem::path tmp =
      std::filesystem::temp_directory_path() / fmt::format("beamspot_{}", ::getpid());
  std::error_code ec;
  std::filesystem::create_directories(tmp, ec);
  if (ec) {
    munmap(shm, shm_bytes);
    throw std::runtime_error("beamspot: cannot create temp dir " + tmp.string() + ": " + ec.message());
  }
  const auto partial_path = [&](int k) {
    return (tmp / fmt::format("partial_{}.root", k)).string();
  };

  std::fflush(nullptr);  // flush inherited buffers before forking (workers _exit)

  std::vector<pid_t> pids(n_proc, -1);
  for (int k = 0; k < n_proc; ++k) {
    const pid_t pid = ::fork();
    if (pid < 0) {
      munmap(shm, shm_bytes);
      std::filesystem::remove_all(tmp, ec);
      throw std::runtime_error("beamspot: fork failed");
    }
    if (pid == 0) {
      // ---- worker process ----
      try {
        histograms h = fill_dst_sequential(groups[k], cfg, &counters[k]);
        write_partial_histograms(h, partial_path(k));
      } catch (const std::exception& e) {
        fmt::print(stderr, "[beamspot] worker {} failed: {}\n", k, e.what());
        ::_exit(1);
      }
      ::_exit(0);
    }
    pids[k] = pid;
  }

  // ---- parent: drive an aggregate progress bar and reap workers ----
  ProgressTracker::Config pcfg;
  pcfg.label     = fmt::format("Filling ({} procs)", n_proc);
  pcfg.show_eta  = true;
  pcfg.show_rate = true;
  ProgressTracker progress(static_cast<std::size_t>(std::max(total_events, 1L)), pcfg);
  progress.start();

  const auto sum_counters = [&]() {
    long s = 0;
    for (int k = 0; k < n_proc; ++k) s += counters[k].load(std::memory_order_relaxed);
    return s;
  };

  int  remaining = n_proc;
  bool failed    = false;
  while (remaining > 0) {
    int   status = 0;
    pid_t done   = 0;
    while ((done = ::waitpid(-1, &status, WNOHANG)) > 0) {
      --remaining;
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failed = true;
    }
    progress.set_processed(static_cast<std::size_t>(sum_counters()));
    if (remaining > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  progress.set_processed(static_cast<std::size_t>(std::max(sum_counters(), total_events)));
  progress.finish();

  if (failed) {
    munmap(shm, shm_bytes);
    std::filesystem::remove_all(tmp, ec);
    throw std::runtime_error("beamspot: one or more fill workers failed");
  }

  // merge the partial histograms back into one set, then clean up
  std::vector<std::string> partials;
  partials.reserve(n_proc);
  for (int k = 0; k < n_proc; ++k) partials.push_back(partial_path(k));
  histograms result = merge_root_files(partials, cfg, /*verbose=*/false);

  munmap(shm, shm_bytes);
  std::filesystem::remove_all(tmp, ec);
  return result;
}

}  // namespace

// --------------------------------------------------------------------------
histograms make_histograms(const config& cfg) {
  if (cfg.theta_bins.size() < 2) {
    throw std::runtime_error("beamspot::make_histograms: need >= 2 theta bin edges");
  }

  const double zmin = cfg.z_map_min;
  const double zmax = cfg.z_map_max;
  const int    bins = 6 * cfg.bins_per_sector;

  histograms h;
  h.h1_z   = std::make_unique<TH1F>("h1_z",   "z vertex;Z vertex (cm);counts",    200, cfg.z_1d_min, cfg.z_1d_max);
  h.h1_phi = std::make_unique<TH1F>("h1_phi", "phi vertex;#phi (degrees);counts", 180, -30, 330);
  h.h1_z->SetDirectory(nullptr);
  h.h1_phi->SetDirectory(nullptr);

  h.h2_z_phi.reserve(cfg.n_theta());
  for (std::size_t i = 0; i < cfg.n_theta(); ++i) {
    const double th = (cfg.theta_bins[i] + cfg.theta_bins[i + 1]) / 2.0;
    auto hh = std::make_unique<TH2F>(fmt::format("h2_z_phi_{}", i).c_str(),
                                     fmt::format("#theta = {:g};Z vertex (cm);#phi (degrees)", th).c_str(),
                                     100, zmin, zmax, bins, -30, 330);
    hh->SetDirectory(nullptr);
    h.h2_z_phi.push_back(std::move(hh));
  }
  return h;
}

// --------------------------------------------------------------------------
histograms fill_dst(const std::vector<std::string>& files, const config& cfg, int n_jobs) {
  long       n   = 0;
  const auto t0  = std::chrono::steady_clock::now();
  histograms result;

  if (n_jobs <= 1) {
    // single process: stream one chain sequentially, count events as we go
    std::atomic<long> ctr{0};
    result = fill_dst_sequential(files, cfg, &ctr);
    n = ctr.load();
  } else {
    // multi-process: one worker per file group, merged from partial outputs
    result = fill_dst_multiprocess(files, cfg, n_jobs, n);
  }

  const auto   t1  = std::chrono::steady_clock::now();
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  fmt::print("### {} files, {} events, {} process(es), EVENT RATE: {:.4f} kHz\n",
             files.size(), n, std::max(n_jobs, 1), sec > 0 ? n / sec / 1000.0 : 0.0);
  return result;
}

// --------------------------------------------------------------------------
histograms merge_files(const std::vector<std::string>& root_files, const config& cfg) {
  return merge_root_files(root_files, cfg, /*verbose=*/true);
}

// --------------------------------------------------------------------------
analysis analyze(histograms histos, const config& cfg) {
  analysis a;
  a.histos = std::move(histos);
  const std::size_t nb = cfg.n_theta();
  a.g_results.reserve(nb);
  a.z_slices.resize(nb);

  const auto make_graph = [](const char* name, const char* title) {
    auto g = std::make_unique<TGraphErrors>();
    g->SetNameTitle(name, title);
    return g;
  };
  a.g_z = make_graph("gZ", "z_{0} vs #theta;#theta (degrees);z_{0} (cm)");
  a.g_r = make_graph("gR", "r_{0} vs #theta;#theta (degrees);r_{0} (cm)");
  a.g_p = make_graph("gP", "#phi_{0} vs #theta;#theta (degrees);#phi_{0} (degrees)");
  a.g_x = make_graph("gX", "x_{0} vs #theta;#theta (degrees);x_{0} (cm)");
  a.g_y = make_graph("gY", "y_{0} vs #theta;#theta (degrees);y_{0} (cm)");

  for (std::size_t i = 0; i < nb; ++i) {
    const double theta = (cfg.theta_bins[i] + cfg.theta_bins[i + 1]) / 2.0;

    auto g = std::make_unique<TGraphErrors>();
    g->SetName(fmt::format("g_results_{}", i).c_str());
    g->SetTitle(fmt::format("#theta = {:g};#phi (degrees);Z vertex (cm)", theta).c_str());
    a.g_results.push_back(std::move(g));

    const TF1& mod = *analyze_theta_bin(i, *a.histos.h2_z_phi[i], *a.g_results[i], a.z_slices[i], cfg);

    const double z0   = mod.GetParameter(0);
    const double ez0  = mod.GetParError(0);
    const double amp  = mod.GetParameter(1);
    const double eamp = mod.GetParError(1);
    const double phi0 = mod.GetParameter(2);
    const double ephi = mod.GetParError(2);

    const double tan_theta = std::tan(TMath::DegToRad() * theta);
    double r   = amp * tan_theta;
    double er  = eamp * tan_theta;
    double phi = std::remainder(TMath::RadToDeg() * phi0 + 180.0, 360.0) + 180.0;
    const double x  = r * std::cos(phi0);
    const double y  = r * std::sin(phi0);
    const double ex = std::sqrt(std::pow(std::cos(phi0) * er, 2) + std::pow(r * std::sin(phi0) * ephi, 2));
    const double ey = std::sqrt(std::pow(std::sin(phi0) * er, 2) + std::pow(r * std::cos(phi0) * ephi, 2));

    // munge the signs for more human-friendly plots
    if (r < 0) phi = std::remainder(phi + 180.0, 360.0);
    r = std::abs(r);

    const auto n = static_cast<int>(i);
    a.g_z->SetPoint(n, theta, z0);  a.g_z->SetPointError(n, 0.0, ez0);
    a.g_r->SetPoint(n, theta, r);   a.g_r->SetPointError(n, 0.0, er);
    a.g_p->SetPoint(n, theta, phi); a.g_p->SetPointError(n, 0.0, TMath::RadToDeg() * ephi);
    a.g_x->SetPoint(n, theta, x);   a.g_x->SetPointError(n, 0.0, ex);
    a.g_y->SetPoint(n, theta, y);   a.g_y->SetPointError(n, 0.0, ey);
  }

  fit_pol0(*a.g_z);
  fit_pol0(*a.g_r);
  fit_pol0(*a.g_p);
  fit_pol0(*a.g_x);
  fit_pol0(*a.g_y);
  return a;
}

// --------------------------------------------------------------------------
beam_spot_result results(const analysis& a) {
  return {pol0_constant(a.g_x.get()), pol0_constant(a.g_y.get()), pol0_constant(a.g_z.get()),
          pol0_constant(a.g_r.get()), pol0_constant(a.g_p.get())};
}

// --------------------------------------------------------------------------
void write_root(const analysis& a, const std::string& path) {
  const std::unique_ptr<TFile> f{TFile::Open(path.c_str(), "RECREATE")};
  if (!f || f->IsZombie()) {
    throw std::runtime_error("beamspot: cannot open ROOT output file: " + path);
  }

  f->cd();
  a.histos.h1_z->Write();
  a.histos.h1_phi->Write();
  for (const auto& h : a.histos.h2_z_phi) h->Write();
  for (const auto& g : a.g_results)       g->Write();  // modulation TF1 travels with the graph

  // z slices, one subdirectory per theta bin, each slice with its fit attached
  TDirectory* slices = f->mkdir("slices");
  for (std::size_t i = 0; i < a.z_slices.size(); ++i) {
    TDirectory* d = slices->mkdir(fmt::format("bin_{}", i).c_str());
    d->cd();
    for (const auto& h : a.z_slices[i]) h->Write();
    f->cd();
  }

  // summary graphs vs theta (pol0 travels with the graph)
  for (const auto* g : {a.g_z.get(), a.g_r.get(), a.g_p.get(), a.g_x.get(), a.g_y.get()}) {
    if (g != nullptr) g->Write();
  }

  // flat TTree of the per-theta points (uproot-friendly), read off the graphs
  if (a.g_z) {
    double theta = 0, z = 0, ez = 0, r = 0, er = 0, phi = 0, ephi = 0, x = 0, ex = 0, y = 0, ey = 0;
    auto* tree = new TTree("points", "per-theta extracted beam-spot points");
    tree->Branch("theta", &theta);
    tree->Branch("z0", &z);     tree->Branch("ez0", &ez);
    tree->Branch("r0", &r);     tree->Branch("er0", &er);
    tree->Branch("phi0", &phi); tree->Branch("ephi0", &ephi);
    tree->Branch("x0", &x);     tree->Branch("ex0", &ex);
    tree->Branch("y0", &y);     tree->Branch("ey0", &ey);
    for (int i = 0; i < a.g_z->GetN(); ++i) {
      theta = a.g_z->GetPointX(i);
      z = a.g_z->GetPointY(i); ez = a.g_z->GetErrorY(i);
      r = a.g_r->GetPointY(i); er = a.g_r->GetErrorY(i);
      phi = a.g_p->GetPointY(i); ephi = a.g_p->GetErrorY(i);
      x = a.g_x->GetPointY(i); ex = a.g_x->GetErrorY(i);
      y = a.g_y->GetPointY(i); ey = a.g_y->GetErrorY(i);
      tree->Fill();
    }
    tree->Write();
  }

  // flat TTree of the final fitted constants (single entry)
  {
    const beam_spot_result fr = results(a);
    double x = fr.x.v, ex = fr.x.e, y = fr.y.v, ey = fr.y.e, z = fr.z.v, ez = fr.z.e;
    double r = fr.r.v, er = fr.r.e, phi = fr.phi.v, ephi = fr.phi.e;
    auto* tree = new TTree("final", "final fitted beam-spot constants");
    tree->Branch("X", &x);      tree->Branch("eX", &ex);
    tree->Branch("Y", &y);      tree->Branch("eY", &ey);
    tree->Branch("Z", &z);      tree->Branch("eZ", &ez);
    tree->Branch("R", &r);      tree->Branch("eR", &er);
    tree->Branch("Phi0", &phi); tree->Branch("ePhi0", &ephi);
    tree->Fill();
    tree->Write();
  }

  // per-theta modulation parameters z0 - A*cos(phi - phi0), read from each
  // g_results graph's attached fit, so matplotlib can redraw the curve.
  {
    int    bin = 0;
    double theta = 0, z0 = 0, amp = 0, phi0 = 0;
    auto* tree = new TTree("modulation", "per-theta modulation fit parameters");
    tree->Branch("bin", &bin);
    tree->Branch("theta", &theta);
    tree->Branch("z0", &z0);
    tree->Branch("A", &amp);
    tree->Branch("phi0", &phi0);
    for (std::size_t i = 0; i < a.g_results.size(); ++i) {
      const auto* fn = a.g_results[i]->GetListOfFunctions()->GetSize() > 0
                           ? dynamic_cast<TF1*>(a.g_results[i]->GetListOfFunctions()->First())
                           : nullptr;
      if (fn == nullptr) continue;
      bin   = static_cast<int>(i);
      theta = (a.g_z && static_cast<int>(i) < a.g_z->GetN()) ? a.g_z->GetPointX(static_cast<int>(i)) : 0.0;
      z0    = fn->GetParameter(0);
      amp   = fn->GetParameter(1);
      phi0  = fn->GetParameter(2);
      tree->Fill();
    }
    tree->Write();
  }

  // per-slice gaussian+linear parameters, keyed by the slice histogram name,
  // so matplotlib can overlay each fit.
  {
    int    bin = 0;
    double amp = 0, mean = 0, sigma = 0, c = 0, d = 0, lo = 0, hi = 0;
    std::string name;
    auto* tree = new TTree("slice_fits", "per-slice gaussian+linear fit parameters");
    tree->Branch("bin", &bin);
    tree->Branch("name", &name);
    tree->Branch("amp", &amp); tree->Branch("mean", &mean); tree->Branch("sigma", &sigma);
    tree->Branch("c", &c);     tree->Branch("d", &d);
    tree->Branch("lo", &lo);   tree->Branch("hi", &hi);
    for (std::size_t i = 0; i < a.z_slices.size(); ++i) {
      for (const auto& h : a.z_slices[i]) {
        const auto* fn = h->GetListOfFunctions()->GetSize() > 0
                             ? dynamic_cast<TF1*>(h->GetListOfFunctions()->First())
                             : nullptr;
        if (fn == nullptr) continue;
        bin   = static_cast<int>(i);
        name  = h->GetName();
        amp   = fn->GetParameter(0);
        mean  = fn->GetParameter(1);
        sigma = fn->GetParameter(2);
        c     = fn->GetParameter(3);
        d     = fn->GetParameter(4);
        lo    = fn->GetXmin();
        hi    = fn->GetXmax();
        tree->Fill();
      }
    }
    tree->Write();
  }

  f->Close();
}

// --------------------------------------------------------------------------
void write_results_txt(const analysis& a, const std::string& path) {
  const beam_spot_result r = results(a);
  std::ofstream wr;
  wr.exceptions(std::ofstream::failbit | std::ofstream::badbit);
  wr.open(path);
  wr << fmt::format("Z    = {} +- {}\n", r.z.v, r.z.e);
  wr << fmt::format("R    = {} +- {}\n", r.r.v, r.r.e);
  wr << fmt::format("Phi0 = {} +- {}\n", r.phi.v, r.phi.e);
  wr << fmt::format("X    = {} +- {}\n", r.x.v, r.x.e);
  wr << fmt::format("Y    = {} +- {}\n", r.y.v, r.y.e);
}

// --------------------------------------------------------------------------
void write_ccdb_table(const analysis& a, const std::string& path) {
  const beam_spot_result r = results(a);
  std::ofstream wr;
  wr.exceptions(std::ofstream::failbit | std::ofstream::badbit);
  wr.open(path);
  wr << "# x y ex ey\n";
  wr << fmt::format("0 0 0 {:.2f} {:.2f} {:.2f} {:.2f}\n", r.x.v, r.y.v, r.x.e, r.y.e);
}

}  // namespace beamspot
