#!/usr/bin/env python3
"""Plot the BeamSpot results from a *_results.root file with matplotlib + mplhep.

Everything is read with uproot -- no PyROOT. The C++ binary stores not only the
histograms and graphs but also the fit parameters (the ``modulation`` and
``slice_fits`` TTrees), so every fit curve is redrawn here from plain numbers
rather than from a TF1.

By default every figure is written as one page of a single multi-page PDF
(``<prefix>_plots.pdf``). Pass ``-f png`` (or any matplotlib format) to write the
figures as separate image files instead. The pages are, in order:
  - parameters (x0, y0, z0, phi0, r0) vs theta
  - one z-vs-phi map per theta bin (+ fitted points + modulation curve)
  - one z-slice grid per theta bin (+ gaussian fits)
"""

import argparse
import glob
import os
import sys

import numpy as np
import uproot
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import mplhep as hep
from matplotlib.backends.backend_pdf import PdfPages

plt.style.use(hep.style.ROOT)


def find_results(output_dir, explicit):
    if explicit:
        return explicit
    hits = sorted(glob.glob(os.path.join(output_dir, "*_results.root")))
    if not hits:
        sys.exit(f"plot_beamspot: no *_results.root found in {output_dir!r}")
    return hits[0]


def graph_xy(g):
    """Return (x, y, ex, ey) from a TGraph/TGraphErrors read by uproot."""
    x = np.asarray(g.member("fX"), dtype=float)
    y = np.asarray(g.member("fY"), dtype=float)
    ex = np.asarray(g.member("fEX"), dtype=float) if "fEX" in g.all_members else np.zeros_like(x)
    ey = np.asarray(g.member("fEY"), dtype=float) if "fEY" in g.all_members else np.zeros_like(y)
    return x, y, ex, ey


def gaussian_plus_line(z, p):
    return p["amp"] * np.exp(-0.5 * ((z - p["mean"]) / p["sigma"]) ** 2) + p["c"] + p["d"] * z


# --------------------------------------------------------------------------
def plot_summary(f, save):
    pts = f["points"].arrays(library="np")
    final = f["final"].arrays(library="np")
    theta = pts["theta"]

    panels = [
        ("x0", "ex0", r"$x_0$ (cm)", "X", "eX"),
        ("y0", "ey0", r"$y_0$ (cm)", "Y", "eY"),
        ("z0", "ez0", r"$z_0$ (cm)", "Z", "eZ"),
        ("phi0", "ephi0", r"$\phi_0$ (deg)", "Phi0", "ePhi0"),
        ("r0", "er0", r"$r_0$ (cm)", "R", "eR"),
    ]

    fig, axes = plt.subplots(3, 2, figsize=(12, 13))
    axes = axes.flatten()
    for ax, (yk, eyk, ylabel, fk, efk) in zip(axes, panels):
        ax.errorbar(theta, pts[yk], yerr=pts[eyk], fmt="o", color="black",
                    capsize=3, label=r"per-$\theta$ bin")
        c, ec = float(final[fk][0]), float(final[efk][0])
        ax.axhline(c, color="red", lw=2, label=f"pol0 = {c:.4g} $\\pm$ {ec:.2g}")
        ax.axhspan(c - ec, c + ec, color="red", alpha=0.15)
        ax.set_xlabel(r"$\theta$ (degrees)")
        ax.set_ylabel(ylabel)
        ax.legend(fontsize=11, loc="best")
    axes[-1].axis("off")
    fig.suptitle("Beam spot vs polar angle", fontsize=18)
    fig.tight_layout()
    save(fig, "summary")


def plot_bins(f, save):
    mod = f["modulation"].arrays(library="np") if "modulation" in f else None
    i = 0
    while f"h2_z_phi_{i}" in f:
        h2 = f[f"h2_z_phi_{i}"]
        values, zedges, phiedges = h2.to_numpy()  # values[z, phi]

        fig, ax = plt.subplots(figsize=(8, 6))
        mesh = ax.pcolormesh(zedges, phiedges, values.T, cmap="viridis", shading="flat")
        fig.colorbar(mesh, ax=ax, label="counts")

        if f"g_results_{i}" in f:
            # g_results stores (x = phi, y = fitted z, ey = z error); the map has
            # z on x and phi on y, so plot at (z, phi) with a horizontal error bar.
            phi_pts, zfit, _, zerr = graph_xy(f[f"g_results_{i}"])
            ax.errorbar(zfit, phi_pts, xerr=zerr, fmt="o", color="white", mec="black",
                        capsize=2, label="fitted target z")

        if mod is not None and i in mod["bin"]:
            k = int(np.where(mod["bin"] == i)[0][0])
            phi = np.linspace(phiedges[0], phiedges[-1], 400)
            z = mod["z0"][k] - mod["A"][k] * np.cos(np.deg2rad(phi) - mod["phi0"][k])
            ax.plot(z, phi, color="red", lw=2, label=r"$z_0 - A\cos(\phi-\phi_0)$")

        ax.set_xlabel("z vertex (cm)")
        ax.set_ylabel(r"$\phi$ (degrees)")
        title = (h2.member("fTitle") or f"theta bin {i}").replace("#theta", r"$\theta$")
        ax.set_title(title)
        ax.legend(loc="upper right", fontsize=10)
        fig.tight_layout()
        save(fig, f"bin{i}")
        i += 1


def plot_slices(f, save):
    if "slice_fits" not in f:
        return
    sf = f["slice_fits"].arrays(library="np")
    names = [n.decode() if isinstance(n, bytes) else str(n) for n in sf["name"]]
    params = {
        names[j]: {k: sf[k][j] for k in ("amp", "mean", "sigma", "c", "d", "lo", "hi")}
        for j in range(len(names))
    }

    if "slices" not in f:
        return
    slices_dir = f["slices"]
    for bkey in slices_dir.keys(recursive=False):
        bin_name = bkey.split(";")[0]          # e.g. "bin_6"
        try:
            bin_idx = int(bin_name.split("_")[1])
        except (IndexError, ValueError):
            continue
        d = slices_dir[bin_name]
        hkeys = [k.split(";")[0] for k in d.keys(recursive=False)]
        if not hkeys:
            continue

        ncol = 6
        nrow = (len(hkeys) + ncol - 1) // ncol
        fig, axes = plt.subplots(nrow, ncol, figsize=(2.6 * ncol, 2.2 * nrow), squeeze=False)
        for ax in axes.flatten():
            ax.axis("off")
        for j, hk in enumerate(sorted(hkeys)):
            ax = axes.flatten()[j]
            ax.axis("on")
            counts, edges = d[hk].to_numpy()
            centers = 0.5 * (edges[:-1] + edges[1:])
            ax.step(centers, counts, where="mid", color="black", lw=0.8)
            p = params.get(hk)
            if p is not None:
                zz = np.linspace(p["lo"], p["hi"], 200)
                ax.plot(zz, gaussian_plus_line(zz, p), color="red", lw=1.2)
                ax.plot(zz, p["c"] + p["d"] * zz, color="tab:green", lw=1.0, ls="--")
                ax.set_xlim(p["mean"] - 6, p["mean"] + 6)
            ax.tick_params(labelsize=6)
        fig.suptitle(f"z slices, theta bin {bin_idx}", fontsize=12)
        fig.tight_layout()
        save(fig, f"slices_bin{bin_idx}")


def main():
    ap = argparse.ArgumentParser(description="Plot BeamSpot results (matplotlib + mplhep)")
    ap.add_argument("results", nargs="?", help="path to *_results.root (overrides --output-dir lookup)")
    ap.add_argument("-o", "--output-dir", default="beamspot_out",
                    help="folder to read *_results.root from and write figures into")
    ap.add_argument("-f", "--format", default="pdf",
                    help="output format: 'pdf' (default) for a single multi-page PDF, "
                         "or an image format (png, svg, ...) for one file per figure")
    args = ap.parse_args()

    path = find_results(args.output_dir, args.results)
    output_dir = args.output_dir or os.path.dirname(path) or "."
    prefix = os.path.basename(path).replace("_results.root", "")

    print(f"plotting {path}")
    if args.format == "pdf":
        out = os.path.join(output_dir, f"{prefix}_plots.pdf")
        with PdfPages(out) as pdf, uproot.open(path) as f:
            def save(fig, _suffix):
                pdf.savefig(fig)
                plt.close(fig)
            plot_summary(f, save)
            plot_bins(f, save)
            plot_slices(f, save)
        print("wrote", out)
    else:
        def save(fig, suffix):
            out = os.path.join(output_dir, f"{prefix}_{suffix}.{args.format}")
            fig.savefig(out, dpi=150)
            plt.close(fig)
            print("wrote", out)
        with uproot.open(path) as f:
            plot_summary(f, save)
            plot_bins(f, save)
            plot_slices(f, save)


if __name__ == "__main__":
    main()
