<div align="center">
  <h1>🧁 Muffin</h1>
  
  <img src="https://readme-typing-svg.demolab.com?font=Fira+Code&weight=600&size=20&pause=1000&color=C084FC&center=true&vCenter=true&width=600&lines=Cross-platform+GUI+for+bypassing+DPI+censorship;Windows+(winws)+%2B+Linux+(nfqws);Built+with+C%2B%2B+and+Qt6;Seamless+Cloudflare+WARP+Integration" alt="Muffin GUI" />

  <br><br>

  <a href="https://github.com/F0RSES/zapret-muffin-gui/actions/workflows/ci.yml">
    <img src="https://github.com/F0RSES/zapret-muffin-gui/actions/workflows/ci.yml/badge.svg" alt="CI Status" />
  </a>
  <img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License" />
  <img src="https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20Arch-0078D4?logo=windows" alt="Platforms" />
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=c%2B%2B" alt="C++20" />
  <img src="https://img.shields.io/badge/Qt-6.6+-41CD52?logo=qt" alt="Qt6" />
</div>

<br>

> **Muffin** — это современный, безопасный и полностью автоматизированный графический интерфейс (GUI) для управления утилитой обхода блокировок **zapret**. Забудьте о ручном редактировании `.bat` файлов, запуске скриптов в терминале и сложной настройке Cloudflare WARP.

---

## ⚡ Terminal Overview

```bash
user@muffin:~$ ./muffin --info
> Initializing Muffin Core...
> OS detected: Linux (Kali) / Windows (x64)
> Zapret engine: nfqws (Portable) / winws
> WARP Integration: Standby
> [OK] GUI initialized in 0.04s
```

<details>
  <summary><b>🔥 Нажми, чтобы увидеть список фич (Features)</b></summary>
  <br>

  * 🚀 **Zero-Config Setup:** Muffin сам скачает `zapret` с GitHub, проверит SHA-256 и безопасно установит в систему.
  * 🛡️ **Real Blockcheck:** Встроенный движок проверяет реальный TLS-handshake (а не просто пинг). Узнайте, какой из 22 профилей пробивает DPI вашего провайдера.
  * 🌐 **WARP-as-a-Service:** Автоматическая установка, регистрация и настройка Cloudflare WARP (MASQUE). GUI Cloudflare скрывается, вы остаетесь в одном окне.
  * 🐶 **Watchdog:** Фоновый мониторинг. Если DPI сломает обход, Muffin заметит это и предложит переключиться.
  * 📊 **Statistics:** Локальная база данных с историей эффективности каждого профиля.
  * 🎨 **Premium UI:** Кастомное окно без рамок, неоновые акценты, тёмная/светлая темы и плавные анимации.
</details>

---

## 📸 Interface

<div align="center">
  <table>
    <tr>
      <td align="center"><b>Dashboard</b></td>
      <td align="center"><b>Profiles & Blockcheck</b></td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/dashboard.png" width="400" alt="Dashboard" /></td>
      <td><img src="docs/screenshots/profiles.png" width="400" alt="Profiles" /></td>
    </tr>
    <tr>
      <td align="center"><b>Statistics & History</b></td>
      <td align="center"><b>Settings</b></td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/stats.png" width="400" alt="Stats" /></td>
      <td><img src="docs/screenshots/settings.png" width="400" alt="Settings" /></td>
    </tr>
  </table>
</div>

---

## 🚀 Quick Start

<div align="center">
  <a href="https://github.com/F0RSES/zapret-muffin-gui/releases/latest">
    <img src="https://img.shields.io/badge/⬇_Download_Latest_Release-100000?style=for-the-badge&logo=github&logoColor=white" alt="Download" />
  </a>
</div>

### 🪟 Windows
```powershell
# 1. Скачайте Muffin-Setup.exe из раздела Releases
# 2. Запустите установщик (требуются права администратора)
# 3. Muffin сам скачает winws и WinDivert при первом запуске
```

### 🐧 Linux (Debian / Ubuntu / Kali)
```bash
# 1. Скачайте .deb пакет из Releases
# 2. Установите его:
sudo apt install ./muffin_x.x.x_amd64.deb
```

### 🦊 Arch Linux
```bash
git clone https://github.com/F0RSES/zapret-muffin-gui.git
cd zapret-muffin-gui/packaging/arch
makepkg -si
```

---

## 📦 Powered by

Muffin не изменяет оригинальные бинарники. Приложение загружает их из официальных источников:
* **Windows:** [Flowseal/zapret-discord-youtube](https://github.com/Flowseal/zapret-discord-youtube)
* **Linux:** [bol-van/zapret](https://github.com/bol-van/zapret)

---

## 🛠️ Build from Source

```bash
git clone https://github.com/F0RSES/zapret-muffin-gui.git
cd zapret-muffin-gui
./scripts/build.sh
ctest --output-on-failure
```
* **Requirements:** Qt 6.6+, CMake 3.21+, C++20 Compiler (GCC 11+ / MSVC 2022).

---

<div align="center">
  <h3>⭐ Star History</h3>
  <a href="https://star-history.com/#F0RSES/zapret-muffin-gui&Date">
    <img src="https://api.star-history.com/svg?repos=F0RSES/zapret-muffin-gui&type=Date" alt="Star History Chart" />
  </a>
</div>

<br>

<div align="center">
  <i>Distributed under the MIT License. See <code>LICENSE</code> for more information.</i>
</div>
