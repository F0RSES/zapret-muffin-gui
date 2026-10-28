#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QSettings;

namespace zapret {

// Typed view over QSettings for everything the GUI persists.
//
// The wrapper exists so no widget ever spells a key name or a default twice: readers get sane
// defaults, writers emit changed() so open views (MainWindow, SettingsPage, tray) can react in
// one place.
//
// The application-wide store uses Qt's NATIVE format for the platform — QSettings::setDefaultFormat()
// is never called, so a default-constructed QSettings resolves organizationName()/applicationName() to
// wherever this platform keeps them: the registry under HKCU\Software\F0RSES\Muffin on Windows, a plist
// under ~/Library/Preferences on macOS, and an ini file in the organization's config directory on Linux
// (~/.config/F0RSES/Muffin.conf, honouring XDG_CONFIG_HOME) — Qt's native format on XDG platforms
// already being IniFormat, which is why the file looks like the one an ini store would write.
//
// That is a deliberate choice, not an oversight: it is the format the 0.1.0 store was written in, so
// importLegacySettings() can read the old identity with QSettings::defaultFormat() and be sure it is
// naming the same kind of store. It also carries the one cost worth knowing — on Windows the values live
// in the registry rather than in a text file, so nothing here is a portable file the user can copy, and
// renaming the organization relocates the whole store (again: that is what the import repairs).
//
// Only the explicit-path constructor below forces IniFormat, and only so that a test or an isolated
// store is handed a plain text file at a known location.
class Settings : public QObject
{
    Q_OBJECT

public:
    // ---- the watchdog's cadence, spelled exactly once ----
    //
    // The clamp below is the policy; the spin box's range, the model's own clamp and the QTimer's
    // interval are three places that would otherwise each get their own copy of "1..60". A range that
    // lives in one of them only is a range that silently disagrees with the other two the first time
    // somebody widens it, so they are published here and read from everywhere else.
    static constexpr int kWatchdogDefaultIntervalMin = 5;
    static constexpr int kWatchdogMinIntervalMin = 1;
    static constexpr int kWatchdogMaxIntervalMin = 60;

    // Application-wide store (organizationName/applicationName from QApplication).
    explicit Settings(QObject *parent = nullptr);
    // Explicit ini file — used by tests and by anyone who wants an isolated store.
    explicit Settings(const QString &iniFilePath, QObject *parent = nullptr);
    ~Settings() override;

    // ---- zapret location ----
    QString zapretPath() const;
    void setZapretPath(const QString &path);

    // ---- strategy bookkeeping ----
    QString lastStrategyId() const;
    void setLastStrategyId(const QString &id);

    // ---- behaviour toggles ----
    bool startLastOnLaunch() const;      // auto-start the previous strategy at startup
    void setStartLastOnLaunch(bool on);

    bool minimizeToTray() const;         // hide to tray instead of quitting on close
    void setMinimizeToTray(bool on);

    // ---- the mode toggle -------------------------------------------------------
    // Which halves of the dashboard's pill are lit, stored as the two independent switches the pill
    // now is. Zapret's half is on by default because it is the mode the app was designed around;
    // WARP's is off because picking the tunnel is an explicit decision about somebody's traffic, and
    // a first run that lit it unasked would be the interface changing the shape of the network on
    // its own.
    //
    // The pair is read together: the pill's own rule (WARP cannot be lit without Zapret) is applied
    // by MainWindow when it restores them, so a hand-edited store holding the impossible combination
    // comes up as the combination it means rather than as a half that cannot be pressed.
    bool useZapret() const;
    void setUseZapret(bool on);

    bool useWarp() const;
    void setUseWarp(bool on);

    // ---- the WARP window ----
    // Whether the WARP tray application may sit on screen beside this one. On by default, because the
    // tunnel is driven from here and a second control over the same daemon invites the two to disagree;
    // the switch only means something on the platform whose package ships a window, so the settings page
    // shows it where WarpInstaller::platformHasWarpGui() says there is one to close.
    bool hideWarpGui() const;
    void setHideWarpGui(bool on);

    // ---- the WARP tunnel's setup state ----
    // Whether this machine has already been made ready for a tunnel: the transport set to the protocol
    // that survives a blocking provider, and warp-svc restarted once so that a daemon which came up
    // while a zapret queue was rewriting the routing does not answer every connect with silence.
    //
    // Stored rather than kept in a member, because it is a fact about the machine and not a wish: the
    // chain costs seconds and can sit behind a root prompt, and paying that on every flip of the toggle
    // is what made turning WARP on feel broken to a user who had already set it up once. Off by default,
    // which is what a machine that has never been through it means.
    //
    // Cleared when a tunnel fails — that is the recovery path, and the next press pays the full chain
    // again. Deliberately not cleared by a reboot: a daemon that started with the machine cannot be the
    // one that came up behind a running queue, so the condition the restart fixes has not had a chance
    // to occur.
    bool warpEnvironmentPrepared() const;
    void setWarpEnvironmentPrepared(bool on);

    // ---- motion ----
    // Whether the interface may move at all: page fades, card entrances, the power button's breath. On
    // by default, because the design was drawn moving; the switch exists for the machines the movement
    // costs. Read once by MainWindow at startup and again on every write, and it is the whole policy —
    // no widget consults this directly, they all ask Motion, which the window keeps in step with it.
    bool animationsEnabled() const;
    void setAnimationsEnabled(bool on);

    // ---- ping ----
    int pingIntervalSec() const;         // clamped to 3..60
    void setPingIntervalSec(int seconds);

    // ---- watchdog ----
    // How often a live profile is re-asked "do you still bypass?", in minutes, clamped to the range
    // above. The window re-arms its timer from this value, so the setting is the whole policy.
    int watchdogIntervalMin() const;
    void setWatchdogIntervalMin(int minutes);

    // ---- appearance ----
    QString theme() const;               // "dark" | "light" | "system"
    void setTheme(const QString &name);

    // The UI language: a LanguageManager code ("ru", "en", "es", "de", "zh", "fr", "hi"). Unset
    // resolves to "ru", the language every screen was drawn and tested in, so a first run and a
    // headless test agree on what the chrome says.
    QString language() const;
    void setLanguage(const QString &code);

    QString filePath() const;
    void sync();

signals:
    // Emitted after any successful write. Views reconnect to it to re-read what they care about.
    void changed();

private:
    template <typename T>
    T read(const QString &key, const T &fallback) const;
    template <typename T>
    void write(const QString &key, const T &value);

    std::unique_ptr<QSettings> m_settings;
};

// One-time carry-over of everything the 0.1.0 identity persisted, into the store this build uses.
//
// QSettings resolves its file/registry location from organizationName()+applicationName(), so renaming
// the organization moves the settings too — a user who upgrades would lose their theme, ping interval,
// last strategy and the stored zapret path with no warning at all. This imports them.
//
// It must run before the first Settings instance is constructed, i.e. from the entry point, and it
// never overwrites: if the current identity already holds any value the upgrade has been imported (or
// the user started fresh), so the legacy store is left alone. Two stored values can name a path inside
// the folder the migration moved — the zapret root, and a strategy id, which StrategyParser stamps as
// an absolute path into that tree for entries it cannot name — and both are rebased onto the new
// location, but only when the old one has really gone away. See ZapretPaths::migrateManagedInstallDir(),
// whose settings half this is.
void importLegacySettings();

} // namespace zapret
