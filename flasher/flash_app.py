"""
AIRSQAD Cam Linker — Flasher.

Скачивает последнюю собранную прошивку из GitHub Release (тег "latest",
публикуется автоматически через .github/workflows/build-firmware.yml при
каждом пуше в master) и заливает её на плату ESP32-C3 через esptool.

Запуск из исходников:
    pip install -r requirements.txt
    python flash_app.py

Сборка в один .exe: см. build_exe.bat (PyInstaller).
"""

import io
import json
import os
import queue
import sys
import threading
import tkinter as tk
from tkinter import ttk
from pathlib import Path

import requests
import serial.tools.list_ports

GITHUB_REPO = "andrewkena/airsqad-cam-linker"
RELEASE_TAG = "latest"
CACHE_DIR = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "AirsqadFlasher" / "firmware"

# Палитра — та же тёмная тема, что и веб-портал платы (см. status_portal.h).
BG = "#2b2b2b"
BG_PANEL = "#3a3a3a"
FG = "#e0e0e0"
FG_DIM = "#999999"
ACCENT = "#4a90d9"
BORDER = "#666666"
LOG_BG = "#1e1e1e"
LOG_FG = "#d4d4d4"


def resource_path(relative: str) -> Path:
    """Путь к ресурсу, работает и из исходников, и из собранного PyInstaller .exe."""
    base = Path(getattr(sys, "_MEIPASS", Path(__file__).parent))
    return base / relative


def list_com_ports():
    ports = serial.tools.list_ports.comports()
    return [(p.device, f"{p.device} — {p.description}") for p in ports]


def fetch_latest_manifest():
    """Возвращает (manifest_dict, {filename: download_url})."""
    url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/tags/{RELEASE_TAG}"
    resp = requests.get(url, timeout=15)
    resp.raise_for_status()
    release = resp.json()

    assets = {a["name"]: a["browser_download_url"] for a in release["assets"]}
    if "manifest.json" not in assets:
        raise RuntimeError("В релизе нет manifest.json — сборка ещё не завершена?")

    manifest_resp = requests.get(assets["manifest.json"], timeout=15)
    manifest_resp.raise_for_status()
    manifest = manifest_resp.json()
    return manifest, assets


def ensure_firmware_downloaded(manifest, assets, log):
    """Скачивает файлы прошивки в локальный кэш (по commit_short), если их там ещё нет."""
    commit = manifest.get("commit_short", "unknown")
    target_dir = CACHE_DIR / commit
    target_dir.mkdir(parents=True, exist_ok=True)

    for entry in manifest["files"]:
        fname = entry["file"]
        fpath = target_dir / fname
        if fpath.exists():
            log(f"  {fname} уже в кэше")
            continue
        if fname not in assets:
            raise RuntimeError(f"Файл {fname} из manifest.json отсутствует в релизе")
        log(f"  Скачиваю {fname}...")
        r = requests.get(assets[fname], timeout=60)
        r.raise_for_status()
        fpath.write_bytes(r.content)

    return target_dir


def flash_firmware(port, manifest, firmware_dir, log):
    import esptool

    argv = [
        "--chip", manifest["chip"],
        "--port", port,
        "--baud", "921600",
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash", "-z",
        "--flash_mode", manifest["flash_mode"],
        "--flash_freq", manifest["flash_freq"],
        "--flash_size", manifest["flash_size"],
    ]
    for entry in manifest["files"]:
        argv.append(entry["offset"])
        argv.append(str(firmware_dir / entry["file"]))

    log(f"esptool " + " ".join(argv))

    old_stdout = sys.stdout
    captured = io.StringIO()

    class Tee(io.TextIOBase):
        def write(self, s):
            captured.write(s)
            log(s, newline=False)
            return len(s)

    sys.stdout = Tee()
    try:
        esptool.main(argv)
    finally:
        sys.stdout = old_stdout


class FlasherApp:
    def __init__(self, root):
        self.root = root
        root.title("AIRSQAD Cam Linker — Flasher")
        root.geometry("640x600")
        root.resizable(False, False)
        root.configure(bg=BG)

        self._setup_style()

        self.log_queue: queue.Queue = queue.Queue()
        self.manifest = None
        self.assets = None
        self.firmware_dir = None

        pad = {"padx": 10, "pady": 6}

        # --- Шапка: логотип + заголовок по центру (как в веб-портале платы) ---
        frm_header = tk.Frame(root, bg=BG)
        frm_header.pack(fill="x", pady=(20, 4))

        self._logo_img = None
        try:
            self._logo_img = tk.PhotoImage(file=str(resource_path("assets/logo.png")))
            tk.Label(frm_header, image=self._logo_img, bg=BG).pack()
        except tk.TclError:
            pass

        tk.Label(
            frm_header, text="AIRSQAD Cam Linker", bg=BG, fg=FG,
            font=("Segoe UI", 16, "bold"),
        ).pack(pady=(8, 0))
        tk.Label(
            frm_header, text="Flasher", bg=BG, fg=FG_DIM, font=("Segoe UI", 10),
        ).pack()
        ttk.Separator(frm_header, orient="horizontal").pack(fill="x", padx=80, pady=(12, 0))

        frm_top = tk.Frame(root, bg=BG)
        frm_top.pack(fill="x", **pad)

        tk.Label(frm_top, text="COM-порт:", bg=BG, fg=FG).pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(frm_top, textvariable=self.port_var, width=30, state="readonly")
        self.port_combo.pack(side="left", padx=8)
        ttk.Button(frm_top, text="Обновить список", command=self.refresh_ports).pack(side="left")

        frm_fw = tk.Frame(root, bg=BG)
        frm_fw.pack(fill="x", **pad)
        self.fw_status_var = tk.StringVar(value="Прошивка: не проверена")
        tk.Label(frm_fw, textvariable=self.fw_status_var, bg=BG, fg=FG).pack(side="left")
        ttk.Button(frm_fw, text="Проверить обновления", command=self.check_updates).pack(side="right")

        frm_actions = tk.Frame(root, bg=BG)
        frm_actions.pack(fill="x", **pad)
        self.flash_btn = ttk.Button(
            frm_actions, text="Прошить плату", command=self.start_flash,
            state="disabled", style="Accent.TButton",
        )
        self.flash_btn.pack(fill="x")

        self.progress = ttk.Progressbar(root, mode="indeterminate")
        self.progress.pack(fill="x", **pad)

        tk.Label(root, text="Лог:", bg=BG, fg=FG).pack(anchor="w", padx=10)
        self.log_text = tk.Text(root, height=14, state="disabled", bg=LOG_BG, fg=LOG_FG,
                                 relief="flat", borderwidth=0)
        self.log_text.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        self.refresh_ports()
        self.root.after(100, self.poll_log_queue)
        self.root.after(300, self.check_updates)

    def _setup_style(self):
        style = ttk.Style()
        style.theme_use("clam")

        style.configure("TFrame", background=BG)
        style.configure("TLabel", background=BG, foreground=FG)
        style.configure("TSeparator", background=BORDER)

        style.configure("TButton", background=BG_PANEL, foreground=FG,
                         borderwidth=1, focusthickness=0, padding=6)
        style.map("TButton", background=[("active", "#4a4a4a"), ("disabled", "#333333")],
                  foreground=[("disabled", FG_DIM)])

        style.configure("Accent.TButton", background=ACCENT, foreground="white", padding=10)
        style.map("Accent.TButton", background=[("active", "#5a9fe0"), ("disabled", "#333333")],
                  foreground=[("disabled", FG_DIM)])

        style.configure("TCombobox", fieldbackground=BG_PANEL, background=BG_PANEL,
                         foreground=FG, arrowcolor=FG, borderwidth=1)
        style.map("TCombobox", fieldbackground=[("readonly", BG_PANEL)],
                   foreground=[("readonly", FG)])
        self.root.option_add("*TCombobox*Listbox.background", BG_PANEL)
        self.root.option_add("*TCombobox*Listbox.foreground", FG)
        self.root.option_add("*TCombobox*Listbox.selectBackground", ACCENT)

        style.configure("TProgressbar", background=ACCENT, troughcolor=BG_PANEL,
                         borderwidth=0, lightcolor=ACCENT, darkcolor=ACCENT)

    def log(self, msg, newline=True):
        self.log_queue.put(msg + ("\n" if newline else ""))

    def poll_log_queue(self):
        try:
            while True:
                msg = self.log_queue.get_nowait()
                self.log_text.configure(state="normal")
                self.log_text.insert("end", msg)
                self.log_text.see("end")
                self.log_text.configure(state="disabled")
        except queue.Empty:
            pass
        self.root.after(100, self.poll_log_queue)

    def refresh_ports(self):
        ports = list_com_ports()
        self.port_combo["values"] = [label for _, label in ports]
        self._port_devices = [dev for dev, _ in ports]
        if ports:
            self.port_combo.current(0)

    def selected_port(self):
        idx = self.port_combo.current()
        if idx < 0 or idx >= len(self._port_devices):
            return None
        return self._port_devices[idx]

    def check_updates(self):
        threading.Thread(target=self._check_updates_worker, daemon=True).start()

    def _check_updates_worker(self):
        self.log("Проверяю последнюю версию прошивки на GitHub...")
        try:
            manifest, assets = fetch_latest_manifest()
        except Exception as e:
            self.log(f"Не удалось проверить обновления: {e}")
            self.fw_status_var.set("Прошивка: ошибка проверки")
            return
        self.manifest = manifest
        self.assets = assets
        commit = manifest.get("commit_short", "?")
        built_at = manifest.get("built_at", "?")
        self.fw_status_var.set(f"Прошивка: commit {commit}, собрана {built_at}")
        self.log(f"Актуальная версия: {commit} ({built_at})")
        self.flash_btn.configure(state="normal")

    def start_flash(self):
        port = self.selected_port()
        if not port:
            self.log("Выберите COM-порт.")
            return
        if not self.manifest:
            self.log("Сначала проверьте обновления.")
            return
        self.flash_btn.configure(state="disabled")
        self.progress.start(10)
        threading.Thread(target=self._flash_worker, args=(port,), daemon=True).start()

    def _flash_worker(self, port):
        try:
            self.log("Скачиваю файлы прошивки (если нужно)...")
            firmware_dir = ensure_firmware_downloaded(self.manifest, self.assets, self.log)
            self.log(f"Прошиваю {port}...")
            flash_firmware(port, self.manifest, firmware_dir, self.log)
            self.log("Готово! Плата прошита.")
        except Exception as e:
            self.log(f"ОШИБКА: {e}")
        finally:
            self.progress.stop()
            self.flash_btn.configure(state="normal")


def main():
    root = tk.Tk()
    FlasherApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
