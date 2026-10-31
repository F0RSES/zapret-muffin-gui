<div align="center">
  <br>
  <h1>🧁 Muffin</h1>
  
  <img src="https://readme-typing-svg.demolab.com?font=Fira+Code&weight=600&size=24&pause=1000&color=C084FC&center=true&vCenter=true&width=600&lines=%3E+Initializing+Muffin...;%3E+Compiling+C%2B%2B+modules;%3E+Bypassing+DPI+censorship;%3E+Access+Granted." alt="Muffin GUI" />

  <p><b>A modern, secure, and fully automated cross-platform GUI for zapret.</b></p>

  <a href="https://github.com/F0RSES/zapret-muffin-gui/actions/workflows/ci.yml">
    <img src="https://img.shields.io/badge/CI-Passing-10B981?style=for-the-badge&logo=githubactions&logoColor=white" alt="CI" />
  </a>
  <a href="https://github.com/F0RSES/zapret-muffin-gui/releases">
    <img src="https://img.shields.io/badge/Release-v1.0.0-C084FC?style=for-the-badge&logo=github&logoColor=white" alt="Release" />
  </a>
  <a href="https://opensource.org/licenses/MIT">
    <img src="https://img.shields.io/badge/License-MIT-22A7F0?style=for-the-badge" alt="License" />
  </a>
  <br>
  <img src="https://img.shields.io/badge/Platform-Windows_%7C_Linux_%7C_Arch-15161C?style=flat-square" alt="Platforms" />
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=c%2B%2B" alt="C++20" />
  <img src="https://img.shields.io/badge/Qt-6.6+-41CD52?style=flat-square&logo=qt" alt="Qt6" />
  <br><br>
</div>

> **Muffin** eliminates the need to manually edit `.bat` files, type terminal commands, or struggle with Cloudflare WARP configurations. Everything is controlled from a single, beautifully crafted interface.

---

## ⚡ Terminal Overview

```bash
$ muffin --status
> [OK] GUI initialized (Qt6/C++20)
> [OK] Zapret engine: nfqws (Portable) / winws
> [OK] Cloudflare WARP: Connected (MASQUE)
> [OK] Active Strategy: nfqws (portable) | Latency: 42ms
```

<br>

## ✨ Features that stand out

<table>
  <tr>
    <td width="50%">
      <h3>🚀 Zero-Config Setup</h3>
      Muffin downloads the latest zapret release, verifies SHA-256 checksums, and securely installs it to <code>/opt</code> or <code>Program Files</code>.
    </td>
    <td width="50%">
      <h3>🛡️ Real Blockcheck</h3>
      Tests actual TLS handshakes (not just ICMP) to find out which of the 22 profiles truly bypasses your ISP's DPI today.
    </td>
  </tr>
  <tr>
    <td width="50%">
      <h3>🌐 WARP-as-a-Service</h3>
      Auto-installs, registers, and configures Cloudflare WARP. Hides its native GUI so you stay in one window.
    </td>
    <td width="50%">
      <h3>🐶 Smart Watchdog</h3>
      Background monitoring. If your ISP changes DPI rules, Muffin detects the failure and prompts you to switch.
    </td>
  </tr>
  <tr>
    <td width="50%">
      <h3>📊 Local Statistics</h3>
      Tracks the historical success rate of every profile so you always know which one is the most stable.
    </td>
    <td width="50%">
      <h3>🎨 Premium UI</h3>
      Frameless window, dark/light themes, smooth animations, and neon accents matching the Muffin identity.
    </td>
  </tr>
</table>

---

## 📸 Interface Preview

<div align="center">
  <table>
    <tr>
      <td align="center"><b>Dashboard</b></td>
      <td align="center"><b>Profiles & Blockcheck</b></td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/dashboard.png" width="400" style="border-radius: 12px; border: 1px solid #2C2E3E;" alt="Dashboard" /></td>
      <td><img src="docs/screenshots/profiles.png" width="400" style="border-radius: 12px; border: 1px solid #2C2E3E;" alt="Profiles" /></td>
    </tr>
    <tr>
      <td align="center"><b>Statistics & History</b></td>
      <td align="center"><b>Settings</b></td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/stats.png" width="400" style="border-radius: 12px; border: 1px solid #2C2E3E;" alt="Stats" /></td>
      <td><img src="docs/screenshots/settings.png" width="400" style="border-radius: 12px; border: 1px solid #2C2E3E;" alt="Settings" /></td>
    </tr>
  </table>
</div>

---

## 📥 Quick Start

<div align="center">
  <a href="https://github.com/F0RSES/zapret-muffin-gui/releases/latest">
    <img src="https://img.shields.io/badge/⬇_Download_Latest_Release-C084FC?style=for-the-badge&logo=github&logoColor=black" alt="Download" />
  </a>
</div>

<br>

<details>
  <summary><b>🪟 Windows Installation</b></summary>
  <br>
  
  1. Download `Muffin-Setup-x.x.x.exe` from Releases.
  2. Run the installer (requires Administrator privileges for WinDivert).
  3. Muffin will automatically download and configure `winws` on the first launch.
</details>

<details>
  <summary><b>🐧 Linux Installation (Debian / Ubuntu / Kali)</b></summary>
  <br>
  
  Download the `.deb` package from Releases and install it via terminal:
  ```bash
  sudo apt install ./muffin_x.x.x_amd64.deb
  ```
</details>

<details>
  <summary><b>🦊 Arch Linux Installation</b></summary>
  <br>
  
  A ready-to-use `PKGBUILD` is available in the repository:
  ```bash
  git clone https://github.com/F0RSES/zapret-muffin-gui.git
  cd zapret-muffin-gui/packaging/arch
  makepkg -si
  ```
</details>

---

## 🛠️ Build from Source

```bash
# Requirements: Qt 6.6+, CMake 3.21+, GCC 11+ / MSVC 2022
git clone https://github.com/F0RSES/zapret-muffin-gui.git
cd zapret-muffin-gui
./scripts/build.sh
ctest --output-on-failure
```

---

## 📦 Official Sources

Muffin does not modify the original binaries. It securely downloads them from:
* **Windows:** [Flowseal/zapret-discord-youtube](https://github.com/Flowseal/zapret-discord-youtube)
* **Linux:** [bol-van/zapret](https://github.com/bol-van/zapret)

---

<div align="center">
  <h3>⭐ Star History</h3>
  <a href="https://star-history.com/#F0RSES/zapret-muffin-gui&Date">
    <img src="https://api.star-history.com/svg?repos=F0RSES/zapret-muffin-gui&type=Date&theme=dark" alt="Star History" />
  </a>
</div>

<br>

<div align="center">
  <i>Distributed under the MIT License. Built with ❤️ and C++.</i>
</div>
