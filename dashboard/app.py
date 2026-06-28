"""Sircadian deployment simulator — desktop control surface (PySide6).

Configure one deployment world, build the C++ engine, launch a fleet of
independently-rolled deployments (one process per deployment, 1 core per run),
and explore the results: a Monte-Carlo view across deployments and a per-
deployment drill-down (wall-time error, drift tracking, indoor temperature,
cloud cover, and per-fit-day light curves).

Run:
    pip install -r dashboard/requirements.txt
    python dashboard/app.py
"""
import os
import sys

# Pin pyqtgraph to PySide6 *before* importing it, so it doesn't auto-pick a
# different Qt binding (e.g. PyQt) when several are installed -- a mismatch makes
# its widgets miss our QApplication ("QGuiApplication::font(): no QGuiApplication
# instance" spam).
os.environ.setdefault("PYQTGRAPH_QT_LIB", "PySide6")

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QTabWidget, QWidget, QVBoxLayout, QHBoxLayout,
    QGridLayout, QFormLayout, QGroupBox, QLabel, QPushButton, QDoubleSpinBox,
    QSpinBox, QLineEdit, QScrollArea, QComboBox, QProgressBar, QPlainTextEdit,
    QTableWidget, QTableWidgetItem, QMessageBox, QSplitter, QSpinBox as _Spin)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runs as runlib
import sim_iface as sim
import schema

pg.setConfigOptions(antialias=True)
REGIME_NAMES = {0: "clear", 1: "broken", 2: "overcast"}


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Sircadian deployment simulator")
        self.resize(1280, 860)

        self.binary = runlib.sim_binary()
        self.cfg = sim.last_config() or (
            sim.default_config(self.binary) if self.binary else None)
        self.widgets = {}        # path -> (kind, widget) or ("range", lo, hi)
        self.worker = None

        self.tabs = QTabWidget()
        self.setCentralWidget(self.tabs)
        self.tabs.addTab(self._build_configure_tab(), "Configure && Run")
        self.tabs.addTab(self._build_results_tab(), "Results")
        self.tabs.addTab(self._build_history_tab(), "Run history")
        self.refresh_runs()

    # Configure & Run
    def _build_configure_tab(self):
        w = QWidget()
        outer = QVBoxLayout(w)

        bar = QHBoxLayout()
        self.build_btn = QPushButton("Build sim")
        self.build_btn.clicked.connect(self.on_build)
        self.binary_lbl = QLabel(self._binary_text())
        self.defaults_btn = QPushButton("Load defaults")
        self.defaults_btn.clicked.connect(self.on_load_defaults)
        bar.addWidget(self.build_btn)
        bar.addWidget(self.defaults_btn)
        bar.addWidget(self.binary_lbl, 1)
        outer.addLayout(bar)

        run_bar = QHBoxLayout()
        run_bar.addWidget(QLabel("cores:"))
        self.cores_spin = _Spin()
        self.cores_spin.setRange(1, 1024)
        self.cores_spin.setValue(os.cpu_count() or 4)
        run_bar.addWidget(self.cores_spin)
        self.run_btn = QPushButton("Run fleet")
        self.run_btn.clicked.connect(self.on_run)
        self.stop_btn = QPushButton("Stop")
        self.stop_btn.clicked.connect(self.on_stop)
        self.stop_btn.setEnabled(False)
        self.progress = QProgressBar()
        run_bar.addWidget(self.run_btn)
        run_bar.addWidget(self.stop_btn)
        run_bar.addWidget(self.progress, 1)
        outer.addLayout(run_bar)

        self.form_host = QWidget()
        self.form_layout = QVBoxLayout(self.form_host)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self.form_host)
        outer.addWidget(scroll, 1)

        self.build_log = QPlainTextEdit()
        self.build_log.setReadOnly(True)
        self.build_log.setMaximumHeight(120)
        outer.addWidget(self.build_log)

        self.populate_form()
        return w

    def _binary_text(self):
        return "Binary: " + (self.binary or "not built — click Build sim")

    def _make_float(self, val):
        s = QDoubleSpinBox()
        s.setRange(-1e12, 1e12)
        s.setDecimals(6)
        s.setValue(float(val if val is not None else 0.0))
        return s

    def _make_int(self, val):
        s = QSpinBox()
        s.setRange(-2_000_000_000, 2_000_000_000)
        s.setValue(int(val if val is not None else 0))
        return s

    def _make_i64(self, val):
        e = QLineEdit(str(int(val if val is not None else 0)))
        return e

    def _make_field(self, kind, val):
        return {"f": self._make_float, "i": self._make_int,
                "i64": self._make_i64}[kind](val)

    def populate_form(self):
        while self.form_layout.count():
            item = self.form_layout.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        self.widgets = {}
        if not self.cfg:
            self.form_layout.addWidget(QLabel(
                "Build the sim to load the default configuration."))
            return

        for title, fields in schema.SCALAR_SECTIONS:
            box = QGroupBox(title)
            form = QFormLayout(box)
            for path, kind in fields:
                wdg = self._make_field(kind, schema.get_path(self.cfg, path))
                self.widgets[path] = (kind, wdg)
                form.addRow(path.split(".")[-1], wdg)
            self.form_layout.addWidget(box)

        for title, names in schema.RANGE_SECTIONS:
            box = QGroupBox(title)
            grid = QGridLayout(box)
            grid.addWidget(QLabel("<b>parameter</b>"), 0, 0)
            grid.addWidget(QLabel("<b>min</b>"), 0, 1)
            grid.addWidget(QLabel("<b>max</b>"), 0, 2)
            for row, name in enumerate(names, start=1):
                rng = schema.get_path(self.cfg, "ranges." + name) or {}
                lo = self._make_float(rng.get("lo", 0.0))
                hi = self._make_float(rng.get("hi", 0.0))
                grid.addWidget(QLabel(name), row, 0)
                grid.addWidget(lo, row, 1)
                grid.addWidget(hi, row, 2)
                self.widgets["ranges." + name] = ("range", lo, hi)
            self.form_layout.addWidget(box)
        self.form_layout.addStretch(1)

    def collect_config(self):
        for path, spec in self.widgets.items():
            if spec[0] == "range":
                _, lo, hi = spec
                schema.set_path(self.cfg, path, {"lo": lo.value(), "hi": hi.value()})
            else:
                kind, wdg = spec
                if kind == "f":
                    schema.set_path(self.cfg, path, float(wdg.value()))
                elif kind == "i":
                    schema.set_path(self.cfg, path, int(wdg.value()))
                else:  # i64
                    try:
                        schema.set_path(self.cfg, path, int(wdg.text()))
                    except ValueError:
                        schema.set_path(self.cfg, path, 0)
        return self.cfg

    def on_build(self):
        self.build_btn.setEnabled(False)
        self.build_log.setPlainText("building…")
        QApplication.processEvents()
        ok, log = sim.build_sim()
        self.build_log.setPlainText(log)
        self.build_btn.setEnabled(True)
        self.binary = runlib.sim_binary()
        self.binary_lbl.setText(self._binary_text())
        if ok and not self.cfg:
            self.cfg = sim.default_config(self.binary)
            self.populate_form()
        QMessageBox.information(self, "Build", "Build OK" if ok else "Build failed")

    def on_load_defaults(self):
        if not self.binary:
            QMessageBox.warning(self, "Defaults", "Build the sim first.")
            return
        self.cfg = sim.default_config(self.binary)
        self.populate_form()

    def on_run(self):
        if not self.binary:
            QMessageBox.warning(self, "Run", "Build the sim first.")
            return
        cfg = self.collect_config()
        run_dir = runlib.new_run_dir()
        sim.write_run_config(run_dir, cfg)
        self.run_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self.worker = sim.FleetWorker(self.binary, run_dir,
                                      cfg.get("numDeployments", 1),
                                      self.cores_spin.value())
        self.worker.progress.connect(self.on_progress)
        self.worker.finished_run.connect(self.on_run_done)
        self.worker.start()

    def on_progress(self, done, total):
        self.progress.setMaximum(total)
        self.progress.setValue(done)

    def on_stop(self):
        if self.worker:
            self.worker.stop()

    def on_run_done(self, failures, run_dir):
        self.run_btn.setEnabled(True)
        self.stop_btn.setEnabled(False)
        self.refresh_runs()
        # select the new run in Results
        name = os.path.basename(run_dir)
        idx = self.run_combo.findText(name)
        if idx >= 0:
            self.run_combo.setCurrentIndex(idx)
        self.tabs.setCurrentIndex(1)
        if failures:
            QMessageBox.warning(self, "Run", "%d deployment(s) failed." % failures)

    # Results
    def _build_results_tab(self):
        w = QWidget()
        lay = QVBoxLayout(w)

        top = QHBoxLayout()
        top.addWidget(QLabel("Run:"))
        self.run_combo = QComboBox()
        self.run_combo.currentTextChanged.connect(self.on_run_selected)
        top.addWidget(self.run_combo, 1)
        lay.addLayout(top)

        self.res_tabs = QTabWidget()
        lay.addWidget(self.res_tabs, 1)

        # Monte-Carlo across deployments
        mc = QWidget()
        mcl = QVBoxLayout(mc)
        self.mc_err = pg.PlotWidget(title="Steady-state error by deployment (s)")
        self.mc_acc = pg.PlotWidget(title="Accept fraction by deployment")
        scatter_bar = QHBoxLayout()
        scatter_bar.addWidget(QLabel("error vs rolled param:"))
        self.scatter_combo = QComboBox()
        self.scatter_combo.currentTextChanged.connect(self.refresh_scatter)
        scatter_bar.addWidget(self.scatter_combo, 1)
        self.mc_scatter = pg.PlotWidget(title="Steady error vs parameter")
        self.mc_table = QTableWidget()
        split = QSplitter(Qt.Vertical)
        up = QWidget(); upl = QVBoxLayout(up)
        upl.addWidget(self.mc_err); upl.addWidget(self.mc_acc)
        lowq = QWidget(); lowl = QVBoxLayout(lowq)
        lowl.addLayout(scatter_bar); lowl.addWidget(self.mc_scatter)
        lowl.addWidget(self.mc_table)
        split.addWidget(up); split.addWidget(lowq)
        mcl.addWidget(split)
        self.res_tabs.addTab(mc, "Monte-Carlo")

        # Deployment drill-down
        dd = QWidget()
        ddl = QVBoxLayout(dd)
        sel = QHBoxLayout()
        sel.addWidget(QLabel("deployment:"))
        self.dep_combo = QComboBox()
        self.dep_combo.currentTextChanged.connect(self.refresh_drilldown)
        sel.addWidget(self.dep_combo)
        sel.addStretch(1)
        ddl.addLayout(sel)
        self.p_err = pg.PlotWidget(title="Wall-time error (s) vs day")
        self.p_ppm = pg.PlotWidget(title="Drift: true vs learned ppm")
        self.p_ppm.addLegend()
        self.p_temp = pg.PlotWidget(title="Indoor temperature (°C) vs day")
        self.p_cloud = pg.PlotWidget(title="Daily cloudiness vs day")
        for p in (self.p_err, self.p_ppm, self.p_temp, self.p_cloud):
            ddl.addWidget(p)
        cur = QHBoxLayout()
        cur.addWidget(QLabel("light-curve fit day:"))
        self.curve_day = QComboBox()
        self.curve_day.currentTextChanged.connect(self.refresh_curve)
        cur.addWidget(self.curve_day)
        cur.addStretch(1)
        ddl.addLayout(cur)
        self.p_curve = pg.PlotWidget(title="Light curve: observed vs fitted model")
        self.p_curve.addLegend()
        ddl.addWidget(self.p_curve)
        self.res_tabs.addTab(self._scrolled(dd), "Deployment drill-down")

        return w

    def _scrolled(self, inner):
        s = QScrollArea(); s.setWidgetResizable(True); s.setWidget(inner)
        return s

    def refresh_runs(self):
        runs = runlib.list_runs()
        ok = [r["name"] for r in runs if r["status"] in ("ok", "partial")]
        cur = self.run_combo.currentText() if hasattr(self, "run_combo") else ""
        self.run_combo.blockSignals(True)
        self.run_combo.clear()
        self.run_combo.addItems(ok)
        if cur in ok:
            self.run_combo.setCurrentText(cur)
        self.run_combo.blockSignals(False)
        self._refresh_history_table(runs)
        if ok:
            self.on_run_selected(self.run_combo.currentText())

    def _current_run_dir(self):
        name = self.run_combo.currentText()
        if not name:
            return None
        return os.path.join(runlib.runs_root(), name)

    def on_run_selected(self, _name):
        run_dir = self._current_run_dir()
        if not run_dir:
            return
        self.summary = runlib.load_summary(run_dir)
        self.refresh_montecarlo()
        idxs = runlib.deployment_indices(run_dir)
        self.dep_combo.blockSignals(True)
        self.dep_combo.clear()
        self.dep_combo.addItems([str(i) for i in idxs])
        self.dep_combo.blockSignals(False)
        if idxs:
            self.refresh_drilldown(str(idxs[0]))

    def refresh_montecarlo(self):
        df = getattr(self, "summary", None)
        if df is None or df.empty:
            return
        idx = df["index"].to_numpy()
        steady_s = df["steady_err_ms"].to_numpy() / 1000.0
        self.mc_err.clear()
        self.mc_err.addItem(pg.BarGraphItem(x=idx, height=steady_s, width=0.6,
                                            brush="#4C78A8"))
        self.mc_acc.clear()
        self.mc_acc.addItem(pg.BarGraphItem(x=idx, height=df["accept_frac"].to_numpy(),
                                            width=0.6, brush="#59A14F"))
        # scatter param choices = rolled columns (after the 8 summary stats)
        params = [c for c in df.columns if c not in (
            "index", "steady_err_ms", "max_err_ms", "accept_frac",
            "convergence_day", "longest_gap_days", "fits", "accepts")]
        cur = self.scatter_combo.currentText()
        self.scatter_combo.blockSignals(True)
        self.scatter_combo.clear()
        self.scatter_combo.addItems(params)
        if cur in params:
            self.scatter_combo.setCurrentText(cur)
        self.scatter_combo.blockSignals(False)
        self.refresh_scatter()
        self._fill_table(df)

    def refresh_scatter(self):
        df = getattr(self, "summary", None)
        col = self.scatter_combo.currentText()
        if df is None or df.empty or col not in df.columns:
            return
        self.mc_scatter.clear()
        self.mc_scatter.setLabel("bottom", col)
        self.mc_scatter.setLabel("left", "steady error (s)")
        sp = pg.ScatterPlotItem(x=df[col].to_numpy(),
                                y=df["steady_err_ms"].to_numpy() / 1000.0,
                                size=9, brush="#E45756")
        self.mc_scatter.addItem(sp)

    def _fill_table(self, df):
        show = ["index", "steady_err_ms", "max_err_ms", "accept_frac",
                "convergence_day", "longest_gap_days"]
        show = [c for c in show if c in df.columns]
        self.mc_table.setRowCount(len(df))
        self.mc_table.setColumnCount(len(show))
        self.mc_table.setHorizontalHeaderLabels(show)
        for r in range(len(df)):
            for c, col in enumerate(show):
                v = df.iloc[r][col]
                if col.endswith("_ms"):
                    txt = "%.2f s" % (v / 1000.0)
                elif col == "accept_frac":
                    txt = "%.2f" % v
                else:
                    txt = str(int(v)) if float(v).is_integer() else "%.3g" % v
                self.mc_table.setItem(r, c, QTableWidgetItem(txt))

    def refresh_drilldown(self, idx_text):
        run_dir = self._current_run_dir()
        if not run_dir or not idx_text:
            return
        idx = int(idx_text)
        df = runlib.load_daily(run_dir, idx)
        if df.empty:
            return
        day = df["day"].to_numpy()
        self.p_err.clear()
        self.p_err.plot(day, df["err_ms"].to_numpy() / 1000.0, pen="#4C78A8")
        acc = df[df["accepted"] == 1]
        self.p_err.addItem(pg.ScatterPlotItem(
            x=acc["day"].to_numpy(), y=acc["err_ms"].to_numpy() / 1000.0,
            size=4, brush="#59A14F"))
        self.p_ppm.clear()
        self.p_ppm.plot(day, df["true_ppm"].to_numpy(), pen="#E45756", name="true ppm")
        self.p_ppm.plot(day, df["learned_ppm"].to_numpy(), pen="#4C78A8",
                        name="learned (−driftPpm)")
        self.p_temp.clear()
        self.p_temp.plot(day, df["indoor_temp_c"].to_numpy(), pen="#F58518")
        self.p_cloud.clear()
        self.p_cloud.plot(day, df["cloudiness"].to_numpy(), pen="#72B7B2")

        curves = runlib.load_curves(run_dir, idx)
        self._curves = curves
        self.curve_day.blockSignals(True)
        self.curve_day.clear()
        if not curves.empty:
            days = sorted(curves["day"].unique())
            self.curve_day.addItems([str(int(d)) for d in days])
        self.curve_day.blockSignals(False)
        self.p_curve.clear()
        if not curves.empty:
            self.refresh_curve(self.curve_day.currentText())
        else:
            self.p_curve.setTitle("Light curves only stored for the detailed deployment")

    def refresh_curve(self, day_text):
        curves = getattr(self, "_curves", None)
        if curves is None or curves.empty or not day_text:
            return
        d = curves[curves["day"] == int(day_text)]
        self.p_curve.clear()
        obs = d.dropna(subset=["observed"])
        self.p_curve.addItem(pg.ScatterPlotItem(
            x=obs["minute"].to_numpy(), y=obs["observed"].to_numpy(),
            size=3, brush="#4C78A8", name="observed"))
        self.p_curve.plot(d["minute"].to_numpy(), d["theoretical"].to_numpy(),
                          pen="#E45756", name="fitted model")
        self.p_curve.setLabel("bottom", "local minute of day")
        self.p_curve.setLabel("left", "ADC counts")

    # Run history
    def _build_history_tab(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        self.hist_table = QTableWidget()
        lay.addWidget(self.hist_table)
        btns = QHBoxLayout()
        self.del_btn = QPushButton("Delete selected run")
        self.del_btn.clicked.connect(self.on_delete_run)
        self.del_all_btn = QPushButton("Delete all but latest")
        self.del_all_btn.clicked.connect(self.on_delete_all)
        btns.addWidget(self.del_btn)
        btns.addWidget(self.del_all_btn)
        btns.addStretch(1)
        lay.addLayout(btns)
        return w

    def _refresh_history_table(self, runs):
        cols = ["name", "status", "deployments", "horizonDays"]
        self.hist_table.setRowCount(len(runs))
        self.hist_table.setColumnCount(len(cols))
        self.hist_table.setHorizontalHeaderLabels(cols)
        for r, run in enumerate(runs):
            for c, col in enumerate(cols):
                self.hist_table.setItem(r, c, QTableWidgetItem(str(run.get(col, ""))))

    def _selected_run_name(self):
        row = self.hist_table.currentRow()
        if row < 0:
            return None
        item = self.hist_table.item(row, 0)
        return item.text() if item else None

    def on_delete_run(self):
        name = self._selected_run_name()
        if not name:
            return
        runlib.delete_run(os.path.join(runlib.runs_root(), name))
        self.refresh_runs()

    def on_delete_all(self):
        runlib.delete_all_but_latest()
        self.refresh_runs()


def main():
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
