# Setting up a new machine

How to reproduce the AyuGram build (with the built-in bulk download API) on a
fresh Linux install. This build and test run happened on **Arch Linux**; the
steps below are the exact recipe.

## Which distro / desktop?

- **Arch-based systems are the recommended path** (Arch, EndeavourOS, Manjaro,
  HoloISO, ...): their `qt6-base` package ships the tdesktop-internal Qt patches
  (`QtCore/QPointer` and friends), so the whole tree builds against the stock
  distro Qt.
- NixOS and Gentoo have official AyuGram packaging (flake / ebuild).
- Debian/Ubuntu/Fedora stock Qt *usually lacks those patches* — tdesktop
  upstream builds there via its own Docker environment (which compiles a patched
  Qt). Doable, but avoid on a first setup.
- **Desktop environment is a non-issue.** The app is a normal Qt client: X11 or
  Wayland, any DE. The API server is independent of the desktop environment.

## 1. System dependencies (Arch)

```bash
sudo pacman -S --needed base-devel git qt6-base qt6-shadertools \
  qt6-imageformats qt6-svg qt6-wayland \
  abseil-cpp ada cmark-gfm ffmpeg glib2 hunspell kcoreaddons libavif libfido2 \
  libheif libjpeg-turbo libjxl libpipewire libsrtp libxcb libxcomposite \
  libxdamage libxext libxfixes libxkbcommon libxrandr libxtst lz4 minizip \
  openal openh264 openssl pipewire protobuf rnnoise wayland xxhash zlib \
  boost libtg_owt microsoft-gsl range-v3 tl-expected vulkan-headers \
  gobject-introspection glib2-devel gperf python python-pipx
yay -S ninja ccache   # or: sudo pacman -S --needed ninja ccache (if in extra)
```

## 2. Clone the fork and submodules

```bash
git clone https://github.com/tokenbleed/ayugram-desktop-api.git AyuGramDesktop
cd AyuGramDesktop
git submodule update --init --recursive --jobs 12
```

## 3. Build the external tde2e library

```bash
git clone --depth 1 https://github.com/tdlib/td.git /tmp/td
cd /tmp/td && git checkout 022d60202e446ad1287b9fb68e687c8a0760788b
cmake -B build -S . -DCMAKE_BUILD_TYPE=None -DTD_E2E_ONLY=ON \
  -DCMAKE_INSTALL_PREFIX=$HOME/tools/tde2e
cmake --build build -j$(nproc) && cmake --install build
```

## 4. Configure and build AyuGram

```bash
cd ~/AyuGramDesktop
cmake -S . -B ../out -G Ninja -Wno-dev \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTDESKTOP_API_ID=2040 \
  -DTDESKTOP_API_HASH=b18441a1ff607e10a989891a5462e627 \
  -DDESKTOP_APP_DISABLE_AUTOUPDATE=True \
  -Dtde2e_DIR=$HOME/tools/tde2e/lib/cmake/tde2e \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build ../out -j10
# binary: ../out/bin/AyuGram
```

No `-api-*` configure flags are needed - the API server is compiled in always;
it only *listens* when activated at runtime.

## 5. Run with the API enabled

```bash
cd out/bin
AYUGRAM_API_PORT=48123 AYUGRAM_API_TOKEN=mysecret ./AyuGram
# or, flags also work (they are forwarded through the launcher):
./AyuGram -api-port=48123 -api-token=mysecret
```

Then log in once in the GUI on the new machine (QR code / phone number), and:

```bash
curl -H "X-Api-Token: mysecret" http://127.0.0.1:48123/api/v1/health
python3 scripts/bulk_dl.py download --dest /home/you/archive \
  --peer_id 3581232090 --topic_root_id 307529 --poll
```

## Troubleshooting

- **`tde2e_DIR` not found** - the tde2e install path above must point at the
  folder containing `tde2eConfig.cmake`.
- **Qt missing `QtCore/QPointer`** - your distro Qt lacks the tdesktop patches;
  use Arch-based or the NixOS flake.
- **Python 3.13+ "No module named distutils" during GIR generation** - install
  setuptools: `python3 -m pip install --user --break-system-packages setuptools`
  and export `SETUPTOOLS_USE_DISTUTILS=local` before building.
- **`TAKEOUT_INIT_DELAY_86400` from jobs** - Telegram permits one takeout
  session per account per ~24 h; the account recently ran an export. Retry
  later or use an account that did not.