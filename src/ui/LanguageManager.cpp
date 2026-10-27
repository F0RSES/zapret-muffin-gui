#include "LanguageManager.h"

#include <QAbstractButton>
#include <QAction>
#include <QDebug>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QWidget>
#include <QIcon>
#include <QVector>

namespace zapret {

namespace {

// ---- the catalog ------------------------------------------------------------
//
// Column order, everywhere below: ru, en, es, de, zh, fr, hi. One row per key is deliberate — there are
// no separate per-language lists a translation can be dropped from, and uitest asserts that every row
// is complete and that every key the UI asks for exists in the catalog.
//
// Text that is a design token rather than a sentence (the header's "CONNECTED", the footer's "Updated
// every 5s", the BAT/PowerShell kind tags) stays untranslated in the source's own words, the way the
// mock spells them.
constexpr int kLangCount = 7;

struct Row
{
    QString key;
    QString text[kLangCount];
};

#define ROW(K, RU, EN, ES, DE, ZH, FR, HI)                                  \
    Row { QStringLiteral(K), { QStringLiteral(RU), QStringLiteral(EN),      \
                               QStringLiteral(ES), QStringLiteral(DE),      \
                               QStringLiteral(ZH), QStringLiteral(FR),      \
                               QStringLiteral(HI) } }

const QStringList &orderedCodes()
{
    // Menu order = the order every row below is written in.
    static const QStringList codes = { QStringLiteral("ru"), QStringLiteral("en"),
                                       QStringLiteral("es"), QStringLiteral("de"),
                                       QStringLiteral("zh"), QStringLiteral("fr"),
                                       QStringLiteral("hi") };
    return codes;
}

// Each language's name, in that language. Not part of the translatable catalog: "Deutsch" must read as
// Deutsch inside a Russian UI too, or the picker fails the one person who needs it.
const QHash<QString, QString> &nativeNames()
{
    static const QHash<QString, QString> names = {
        { QStringLiteral("ru"), QStringLiteral("Русский") },
        { QStringLiteral("en"), QStringLiteral("English") },
        { QStringLiteral("es"), QStringLiteral("Español") },
        { QStringLiteral("de"), QStringLiteral("Deutsch") },
        { QStringLiteral("zh"), QStringLiteral("中文") },
        { QStringLiteral("fr"), QStringLiteral("Français") },
        { QStringLiteral("hi"), QStringLiteral("हिन्दी") },
    };
    return names;
}

const QVector<Row> &catalog()
{
    static const QVector<Row> rows = {

        // ---- rail / navigation ----
        ROW("nav.logs", "Логи", "Logs", "Registros", "Protokolle", "日志", "Journaux", "लॉग"),
        ROW("nav.profiles", "Профили", "Profiles", "Perfiles", "Profile", "档案",
            "Profils", "प्रोफ़ाइलें"),
        ROW("nav.history", "История", "History", "Historial", "Verlauf", "历史",
            "Historique", "इतिहास"),
        ROW("nav.settings", "Настройки", "Settings", "Ajustes", "Einstellungen", "设置",
            "Paramètres", "सेटिंग्स"),
        ROW("nav.settings_tip", "Настройки (Ctrl+S)", "Settings (Ctrl+S)", "Ajustes (Ctrl+S)",
            "Einstellungen (Strg+S)", "设置（Ctrl+S）", "Paramètres (Ctrl+S)", "सेटिंग्स (Ctrl+S)"),
        ROW("nav.language", "Язык интерфейса", "Interface language", "Idioma de la interfaz",
            "Sprache der Oberfläche", "界面语言", "Langue de l'interface", "इंटरफ़ेस भाषा"),

        // ---- header ----
        ROW("header.status_tip", "Состояние подключения", "Connection status",
            "Estado de la conexión", "Verbindungsstatus", "连接状态", "État de la connexion",
            "कनेक्शन स्थिति"),
        // The window has no system frame, so its two chrome controls are the app's own and have to
        // name themselves.
        ROW("window.minimize", "Свернуть", "Minimize", "Minimizar", "Minimieren", "最小化",
            "Réduire", "घटाएँ"),
        ROW("window.close", "Закрыть", "Close", "Cerrar", "Schließen", "关闭", "Fermer",
            "बंद करें"),

        // ---- updates ----
        // The header's release badge and the one question it asks. The version arrives as %1 everywhere
        // it is shown, because a notice that says "new" without saying which new is a notice the user
        // has to open a browser to understand.
        ROW("update.badge_tip", "Доступна новая версия Muffin",
            "A new version of Muffin is available",
            "Hay una nueva versión de Muffin disponible",
            "Eine neue Version von Muffin ist verfügbar",
            "Muffin 有新版本可用",
            "Une nouvelle version de Muffin est disponible",
            "Muffin का नया संस्करण उपलब्ध है"),
        ROW("update.title", "Обновление Muffin", "Muffin update", "Actualización de Muffin",
            "Muffin-Aktualisierung", "Muffin 更新", "Mise à jour de Muffin", "Muffin अद्यतन"),
        ROW("update.question", "Доступна новая версия Muffin (%1). Хотите скачать обновление?",
            "A new version of Muffin (%1) is available. Download the update?",
            "Hay una nueva versión de Muffin (%1). ¿Quieres descargar la actualización?",
            "Eine neue Version von Muffin (%1) ist verfügbar. Möchten Sie das Update herunterladen?",
            "Muffin 有新版本 (%1)。要下载更新吗？",
            "Une nouvelle version de Muffin (%1) est disponible. Télécharger la mise à jour ?",
            "Muffin का नया संस्करण (%1) उपलब्ध है। क्या आप अद्यतन डाउनलोड करना चाहेंगे?"),
        ROW("update.log.found", "Доступна новая версия Muffin: %1",
            "A new version of Muffin is available: %1",
            "Hay una nueva versión de Muffin disponible: %1",
            "Eine neue Version von Muffin ist verfügbar: %1",
            "Muffin 有新版本可用：%1",
            "Une nouvelle version de Muffin est disponible : %1",
            "Muffin का नया संस्करण उपलब्ध है: %1"),
        ROW("update.log.current", "Обновлений нет — установлена последняя версия (%1)",
            "No updates — this is the latest version (%1)",
            "Sin actualizaciones: esta es la versión más reciente (%1)",
            "Keine Updates — das ist die neueste Version (%1)",
            "无更新 — 这是最新版本 (%1)",
            "Aucune mise à jour — c'est la dernière version (%1)",
            "कोई अद्यतन नहीं — यह नवीनतम संस्करण है (%1)"),
        ROW("update.log.failed", "Проверка обновлений не удалась: %1",
            "The update check failed: %1",
            "La comprobación de actualizaciones falló: %1",
            "Die Update-Prüfung ist fehlgeschlagen: %1",
            "检查更新失败：%1",
            "La vérification des mises à jour a échoué : %1",
            "अद्यतन जाँच विफल रही: %1"),
        ROW("update.open_failed", "Не удалось открыть страницу обновлений в браузере",
            "Could not open the release page in a browser",
            "No se pudo abrir la página de versiones en el navegador",
            "Die Versionsseite ließ sich nicht im Browser öffnen",
            "无法在浏览器中打开发布页面",
            "Impossible d'ouvrir la page des versions dans le navigateur",
            "ब्राउज़र में रिलीज़ पृष्ठ खोलने में असमर्थ"),

        // ---- pages: shared chrome ----
        ROW("page.back", "←  Muffin", "←  Muffin", "←  Muffin",
            "←  Muffin", "←  Muffin", "←  Muffin", "←  Muffin"),
        ROW("page.back_tip", "Вернуться на главный экран (Esc)",
            "Back to the main screen (Esc)", "Volver a la pantalla principal (Esc)",
            "Zurück zur Hauptansicht (Esc)", "返回主界面（Esc）",
            "Retour à l'écran principal (Échap)", "मुख्य स्क्रीन पर लौटें (Esc)"),
        ROW("common.ok", "OK", "OK", "Aceptar", "OK", "确定", "OK", "ठीक है"),
        ROW("common.cancel", "Отмена", "Cancel", "Cancelar", "Abbrechen", "取消", "Annuler",
            "रद्द करें"),
        ROW("common.or", "или", "or", "o", "oder", "或", "ou", "या"),

        // ---- strategies page ----
        ROW("action.refresh", "Обновить", "Refresh", "Actualizar", "Aktualisieren", "刷新",
            "Actualiser", "रीफ़्रेश करें"),
        ROW("action.refresh_tip", "Пересканировать папку zapret (Ctrl+R)",
            "Rescan the zapret folder (Ctrl+R)", "Volver a escanear la carpeta de zapret (Ctrl+R)",
            "Ordner zapret erneut scannen (Strg+R)", "重新扫描 zapret 文件夹（Ctrl+R）",
            "Rescanner le dossier zapret (Ctrl+R)", "zapret फ़ोल्डर फिर से स्कैन करें (Ctrl+R)"),
        ROW("action.download_zapret", "Скачать zapret", "Download zapret", "Descargar zapret",
            "zapret herunterladen", "下载 zapret", "Télécharger zapret", "zapret डाउनलोड करें"),
        ROW("action.download_zapret_tip", "Загрузить последнюю версию zapret с GitHub",
            "Fetch the latest zapret release from GitHub",
            "Obtener la última versión de zapret desde GitHub",
            "Die neueste zapret-Version von GitHub laden", "从 GitHub 获取最新 zapret 版本",
            "Récupérer la dernière version de zapret depuis GitHub",
            "GitHub से नवीनतम zapret संस्करण लाएँ"),
        ROW("action.ping_test", "Тест пинга", "Ping test", "Prueba de ping", "Ping-Test",
            "Ping 测试", "Test de ping", "पिंग परीक्षण"),
        // The same button while its round is in flight. Wording it as a state rather than as a disabled
        // control is what lets the row of cards answer the press visibly.
        ROW("action.ping_checking", "Проверка…", "Checking…", "Comprobando…", "Prüfung läuft…",
            "检测中…", "Vérification…", "जाँच हो रही है…"),
        ROW("action.ping_test_tip",
            "Измерить доступность сервисов прямо сейчас",
            "Measure the services' reachability right now",
            "Medir ahora mismo la disponibilidad de los servicios",
            "Die Erreichbarkeit der Dienste jetzt neu messen",
            "立即重新测量服务可达性",
            "Mesurer maintenant la joignabilité des services",
            "सेवाओं की पहुँच अभी दोबारा नापें"),

        // ---- the real-bypass check (BlockcheckEngine's button, bar and verdict sheet) ----
        ROW("action.blockcheck", "Проверить реальный обход", "Verify real bypass",
            "Verificar evasión real", "Echten Bypass prüfen", "验证真实绕过",
            "Vérifier le contournement réel", "वास्तविक बाईपास जाँचें"),
        ROW("action.blockcheck_tip",
            "Запустить выбранный профиль и проверить реальный доступ к сервисам",
            "Start the selected profile and check real access to the services",
            "Iniciar el perfil seleccionado y comprobar el acceso real a los servicios",
            "Das gewählte Profil starten und den echten Zugriff auf die Dienste prüfen",
            "启动所选配置文件并检查服务的真实访问",
            "Démarrer le profil sélectionné et vérifier l'accès réel aux services",
            "चुनी गई प्रोफ़ाइल शुरू करें और सेवाओं तक वास्तविक पहुँच जाँचें"),
        // The same button while the cycle is in flight — a state, not a disabled control, exactly
        // like action.ping_checking above.
        ROW("action.blockcheck_running", "Проверка обхода…", "Checking bypass…",
            "Verificando evasión…", "Bypass-Prüfung läuft…", "正在检查绕过…",
            "Vérification du contournement…", "बाईपास जाँचा जा रहा है…"),
        ROW("blockcheck.title", "Проверка реального обхода", "Real bypass check",
            "Verificación de evasión real", "Echter-Bypass-Test", "真实绕过检查",
            "Vérification du contournement réel", "वास्तविक बाईपास जाँच"),
        // Appended to a host's line in the verdict sheet when its TLS certificate was rejected:
        // the one result that says "traffic is intercepted", not merely "unreachable".
        ROW("blockcheck.cert_invalid",
            "сертификат недействителен (возможна подмена)",
            "certificate invalid (possible interception)",
            "certificado no válido (posible interceptación)",
            "Zertifikat ungültig (möglicherweise abgefangen)",
            "证书无效（可能被劫持）",
            "certificat invalide (interception possible)",
            "प्रमाणपत्र अमान्य (संभावित अवरोधण)"),
        ROW("blockcheck.no_results",
            "Проверка не дала результатов",
            "The check produced no results",
            "La verificación no produjo resultados",
            "Die Prüfung lieferte keine Ergebnisse",
            "检查未产生任何结果",
            "La vérification n'a donné aucun résultat",
            "जाँच से कोई परिणाम नहीं मिला"),
        ROW("log.blockcheck_started",
            "Проверка обхода: запускается профиль %1",
            "Bypass check: starting profile %1",
            "Verificación de evasión: iniciando el perfil %1",
            "Bypass-Prüfung: Profil %1 wird gestartet",
            "绕过检查：正在启动配置文件 %1",
            "Vérification du contournement : démarrage du profil %1",
            "बाईपास जाँच: प्रोफ़ाइल %1 शुरू हो रही है"),

        // ---- the watchdog: the same measurement, unsolicited ----
        // Where the button above answers a question the user asked, the watchdog asks it on a timer
        // and only speaks when the answer has turned from OK to FAIL. Three voices for one event,
        // because the point is a window that is probably hidden: the journal keeps the sentence
        // (watchdog.alert, in error red), the balloon reaches the desktop (watchdog.notify), and the
        // question (watchdog.question) is the only part that waits for an answer.
        ROW("watchdog.title", "Watchdog", "Watchdog", "Watchdog", "Watchdog", "看门狗",
            "Watchdog", "वॉचडॉग"),
        ROW("watchdog.alert",
            "Внимание! Текущий профиль перестал обходить блокировки (Watchdog).",
            "Warning! The current profile stopped bypassing blocks (Watchdog).",
            "¡Atención! El perfil actual ha dejado de eludir los bloqueos (Watchdog).",
            "Achtung! Das aktuelle Profil umgeht Sperren nicht mehr (Watchdog).",
            "注意！当前配置文件已停止绕过屏蔽（看门狗）。",
            "Attention ! le profil actuel ne contourne plus les blocages (Watchdog).",
            "चेतावनी! मौजूदा प्रोफ़ाइल ने अवरोध तोड़ना बंद कर दिया (वॉचडॉग)."),
        ROW("watchdog.notify",
            "%1 перестал обходить блокировки (Watchdog)",
            "%1 stopped bypassing blocks (Watchdog)",
            "%1 ha dejado de eludir los bloqueos (Watchdog)",
            "%1 umgeht Sperren nicht mehr (Watchdog)",
            "%1 已停止绕过屏蔽（看门狗）",
            "%1 ne contourne plus les blocages (Watchdog)",
            "%1 ने अवरोध तोड़ना बंद कर दिया (वॉचडॉग)"),
        ROW("watchdog.question",
            "Похоже, провайдер изменил настройки, и текущий профиль перестал работать. "
            "Попробовать переключиться на следующий профиль?",
            "It looks like the provider changed its settings and the current profile stopped "
            "working. Try switching to the next profile?",
            "Parece que el proveedor cambió sus ajustes y el perfil actual ha dejado de "
            "funcionar. ¿Intentar cambiar al siguiente perfil?",
            "Der Anbieter scheint die Einstellungen geändert zu haben — das aktuelle Profil "
            "funktioniert nicht mehr. Auf das nächste Profil wechseln?",
            "运营商似乎更改了设置，当前配置文件已失效。要尝试切换到下一个配置文件吗？",
            "Le fournisseur a apparemment modifié ses réglages et le profil actuel ne fonctionne "
            "plus. Essayer de passer au profil suivant ?",
            "लगता है प्रदाता ने सेटिंग बदल दी हैं और मौजूदा प्रोफ़ाइल काम करना बंद कर दी है। "
            "अगली प्रोफ़ाइल पर बदलकर देखें?"),
        // Said when the user said yes: which profile the window is about to bring up through the
        // backend (on Linux, through pkexec), so the journal names the change it is making.
        ROW("watchdog.rotating",
            "Watchdog: переключение на профиль %1",
            "Watchdog: switching to profile %1",
            "Watchdog: cambiando al perfil %1",
            "Watchdog: wechselt zu Profil %1",
            "看门狗：正在切换到配置文件 %1",
            "Watchdog : bascule vers le profil %1",
            "वॉचडॉग: प्रोफ़ाइल %1 पर बदल रहे हैं"),
        // The honest answer when the list holds only the profile that just failed: restarting the same
        // entry is not a rotation, so there is nothing to ask.
        ROW("watchdog.no_next",
            "Watchdog: обход не работает, но в списке не осталось других профилей",
            "Watchdog: the bypass is down, but the list holds no other profile to switch to",
            "Watchdog: la evasión no funciona, pero la lista no ofrece otro perfil",
            "Watchdog: der Bypass ist tot, aber die Liste enthält kein anderes Profil",
            "看门狗：绕过已失效，但列表中没有其他配置文件",
            "Watchdog : le contournement est mort, mais la liste n'offre aucun autre profil",
            "वॉचडॉग: बाईपास ठीक नहीं है, पर सूची में कोई दूसरी प्रोफ़ाइल नहीं बची"),
        // The manual press that lands in the middle of a quiet cycle: one engine, one operation, and a
        // button that refuses silently reads as a button that is broken.
        ROW("watchdog.busy",
            "Фоновая проверка обхода уже идёт — подождите её ответа",
            "A background bypass check is already running — wait for its answer",
            "Ya hay una verificación en segundo plano en marcha; espere su respuesta",
            "Eine Hintergrund-Bypass-Prüfung läuft bereits — bitte auf ihre Antwort warten",
            "后台绕过检查已在进行——请等待其结果",
            "Une vérification en arrière-plan est déjà en cours — attendez sa réponse",
            "पृष्ठभूमि में बाईपास जाँच पहले से चल है — इसके उत्तर का इंतज़ार करें"),


        // ---- the Zapret/WARP pill ----
        // The one question the toggle is allowed to ask. Moving the thumb costs nothing, so most
        // switches need no dialogue at all; it is only when a bypass is already holding the path that
        // the window has to say what switching will do to it — stop it. The two halves' names are not
        // threaded through these seven spellings: they are data, and they go in the box's informative
        // line ("Zapret → WARP"), exactly like the watchdog's rotation question does.
        ROW("mode.switch_title", "Смена режима", "Switching mode", "Cambio de modo",
            "Moduswechsel", "切换模式", "Changement de mode", "मोड बदलना"),
        ROW("mode.switch_question",
            "Сейчас работает обход. Остановить его и переключить режим?",
            "A bypass is running right now. Stop it and switch the mode?",
            "En este momento se está eludiendo el bloqueo. ¿Detenerlo y cambiar de modo?",
            "Aktuell läuft eine Umgehung. Die Umgehung stoppen und den Modus wechseln?",
            "当前正在绕行。要停止绕行并切换模式吗？",
            "Un contournement est en cours. L'arrêter et changer de mode ?",
            "अभी बाईपास काम कर रहा है। उसे रोककर मोड बदलें?"),
        // Said in the footer when the answer was no: the pill went back, and nothing else moved. The
        // user needs to learn that a refused question had an effect (the thumb snapped back) rather
        // than that the click was swallowed.
        ROW("mode.switch_declined", "Режим не изменён — обход продолжает работать",
            "Mode unchanged — the bypass keeps running",
            "Modo sin cambios: la elusión sigue funcionando",
            "Modus unverändert — die Umgehung läuft weiter",
            "模式未改变——绕行仍在运行",
            "Mode inchangé : le contournement continue de fonctionner",
            "मोड वही रहा — बाईपास चलता रहेगा"),
        // The journal line a confirmed switch writes before it tears the live bypass down.
        ROW("mode.switch_stopping", "Переключение режима: останавливаю %1",
            "Switching mode: stopping %1", "Cambio de modo: deteniendo %1",
            "Moduswechsel: %1 wird gestoppt", "切换模式：正在停止 %1",
            "Changement de mode : arrêt de %1", "मोड बदल रहे हैं: %1 रोकर"),

        // ---- the two lines the pill writes while the tunnel comes up ----
        // Both are journal entries about ordering, not about states: the delay between the bypass
        // and the connect is where a WARP half can end up lit over a path that is not carrying it, and
        // the window has to say which of the two things happened — the tunnel went up, or it was
        // withheld. Neither is an error the user is asked to act on, so neither goes in a dialog.
        ROW("mode.warp_after_zapret",
            "Обход запущен — поднимаю туннель WARP",
            "The bypass is up — bringing the WARP tunnel up",
            "La elusión está activa: elevo el túnel WARP",
            "Umgehung läuft — der WARP-Tunnel wird aufgebaut",
            "绕行已就绪——正在建立 WARP 隧道",
            "Le contournement est actif — lancement du tunnel WARP",
            "बाईपास चल है — WARP टनल चढ़ाया जा रहा है"),
        // The mirror case: the three seconds are up, the bypass did not come up, and the connect that
        // was owed is not paid. Said because a dark WARP half after a lit one is otherwise unexplained.
        ROW("mode.warp_withheld",
            "Туннель WARP не запущен — обход не поднялся",
            "The WARP tunnel was not started — the bypass never came up",
            "El túnel WARP no se inició: la elusión no llegó a activarse",
            "WARP-Tunnel nicht gestartet — die Umgehung kam nicht hoch",
            "未启动 WARP 隧道——绕行未能建立",
            "Tunnel WARP non lancé — le contournement ne s'est pas établi",
            "WARP टनल शुरू नहीं किया गया — बाईपास ऊपर नहीं आया"),

        // ---- installing the WARP client ----
        // WARP is not this application's software: the window asks permission to put Cloudflare's
        // package on the machine, and the two lines below say what that costs. The question defaults to
        // No because the answer is a system change that outlives the application, not a preference.
        ROW("warp.install_question",
            "Cloudflare WARP не установлен. Установить его сейчас? Потребуется подтверждение "
            "прав администратора, и установщик нельзя будет отменить на середине.",
            "Cloudflare WARP is not installed. Install it now? This asks for administrator rights, "
            "and the installer cannot be cancelled halfway through.",
            "Cloudflare WARP no está instalado. ¿Instalarlo ahora? Se pedirán derechos de "
            "administrador y el instalador no podrá cancelarse a mitad.",
            "Cloudflare WARP ist nicht installiert. Jetzt installieren? Das verlangt "
            "Administratorrechte, und der Installer lässt sich nicht halb abbrechen.",
            "未安装 Cloudflare WARP。现在安装吗？这需要管理员权限，而且安装过程无法中途取消。",
            "Cloudflare WARP n'est pas installé. L'installer maintenant ? Cela demande des droits "
            "d'administrateur, et l'installation ne pourra pas être annulée à mi-chemin.",
            "Cloudflare WARP इंस्टॉल नहीं है। अभी इंस्टॉल करें? इसमें व्यवस्थापक अधिकार चाहिए, और "
            "इंस्टॉलर को बीच में रद्द नहीं किया जा सकता।"),
        // The repair's question, asked where the sentence above would be false. A machine that owns a
        // client which does not answer needs to be told the difference between "install" and "clear
        // what is here and install again", because the second one is what its own package manager
        // will do to its own leftovers.
        ROW("warp.reinstall_question",
            "На этом компьютере уже есть клиент Cloudflare WARP, но он не отвечает. Очистить его "
            "остатки и установить заново? Потребуется подтверждение прав администратора, и установщик "
            "нельзя будет отменить на середине.",
            "This computer already has a Cloudflare WARP client, but it does not answer. Clear what it "
            "left behind and install it again? This asks for administrator rights, and the installer "
            "cannot be cancelled halfway through.",
            "Este equipo ya tiene un cliente de Cloudflare WARP, pero no responde. ¿Limpiar lo que "
            "dejó e instalarlo de nuevo? Se pedirán derechos de administrador y el instalador no podrá "
            "cancelarse a mitad.",
            "Dieser Rechner hat bereits einen Cloudflare-WARP-Client, aber er antwortet nicht. Seine "
            "Reste entfernen und neu installieren? Das verlangt Administratorrechte, und der "
            "Installer lässt sich nicht halb abbrechen.",
            "此电脑上已经有 Cloudflare WARP 客户端，但它没有响应。清除它留下的内容并重新安装吗？这需要 "
            "管理员权限，而且安装过程无法中途取消。",
            "Ce ordinateur possède déjà un client Cloudflare WARP, mais il ne répond pas. Nettoyer ce "
            "qu'il a laissé et le réinstaller ? Cela demande des droits d'administrateur, et "
            "l'installation ne pourra pas être annulée à mi-chemin.",
            "या संगणकावर आधीच Cloudflare WARP क्लाइंट आहे, पण तो प्रतिसाद देत नाही. त्याचे उरलेले "
            "हटवून परत इंस्टॉल करायचे? याला व्यवस्थापक अधिकार लागतील, आणि इंस्टॉलर मधेत रद्द करता "
            "येणार नाही."),
        ROW("warp.install_title", "Установка Cloudflare WARP", "Installing Cloudflare WARP",
            "Instalando Cloudflare WARP", "Cloudflare WARP wird installiert",
            "正在安装 Cloudflare WARP", "Installation de Cloudflare WARP",
            "Cloudflare WARP इंस्टॉल हो रहा है"),
        ROW("warp.install_progress", "Устанавливаю Cloudflare WARP…", "Installing the WARP client…",
            "Instalando el cliente de WARP…", "WARP-Client wird installiert…",
            "正在安装 WARP 客户端…", "Installation du client WARP…",
            "WARP क्लाइंट इंस्टॉल हो रहा है…"),
        // The journal's two endings. Success is named as the thing the window can now do, not as an
        // adjective about a package; failure quotes the tool's own words back, which is the only part
        // of the message that explains anything.
        ROW("warp.installed", "Cloudflare WARP установлен — туннель доступен из этого окна",
            "Cloudflare WARP is installed — the tunnel is now driven from this window",
            "Cloudflare WARP se ha instalado: el túnel se controla desde esta ventana",
            "Cloudflare WARP ist installiert — der Tunnel wird nun von diesem Fenster gesteuert",
            "Cloudflare WARP 已安装——现在可以从本窗口控制隧道",
            "Cloudflare WARP est installé — le tunnel se pilote depuis cette fenêtre",
            "Cloudflare WARP इंस्टॉल हो गया — अब टनल इसी विंडो से नियंत्रित होगा"),
        ROW("warp.install_failed", "Установка Cloudflare WARP не завершилась: %1",
            "The Cloudflare WARP install did not finish: %1",
            "La instalación de Cloudflare WARP no terminó: %1",
            "Die Installation von Cloudflare WARP wurde nicht abgeschlossen: %1",
            "Cloudflare WARP 安装未完成：%1",
            "L'installation de Cloudflare WARP n'a pas abouti : %1",
            "Cloudflare WARP इंस्टॉल पूरा नहीं हुआ: %1"),
        // The same failure, when the transcript says the download itself died. It is called out apart
        // from the generic line above because it is the only install failure with something the user can
        // do about it inside this application: the package comes from Cloudflare over the same kind of
        // route Zapret exists to make reachable, so turning the bypass on and installing again is an
        // instruction rather than a consolation. Every other failure keeps quoting the tool's own words.
        ROW("warp.install_failed_network",
            "Не удалось скачать пакет WARP из-за ошибки сети. Включите обход Zapret и повторите "
            "установку. Подробные сообщения пакетного менеджера — в журнале.",
            "The WARP package could not be downloaded because of a network error. Turn the Zapret "
            "bypass on and install again; the package manager's own report is in the log.",
            "No se pudo descargar el paquete WARP por un error de red. Active el bypass de Zapret e "
            "intente instalar de nuevo; el informe del gestor de paquetes está en el registro.",
            "Das WARP-Paket konnte aufgrund eines Netzwerkfehlers nicht heruntergeladen werden. "
            "Aktivieren Sie den Zapret-Umgehung und wiederholen Sie die Installation; die Meldung des "
            "Paketmanagers steht im Protokoll.",
            "由于网络错误，无法下载 WARP 软件包。请开启 Zapret 绕行后重试安装；包管理器的报告见日志。",
            "Impossible de télécharger le paquet WARP à cause d'une erreur réseau. Activez le "
            "contournement Zapret puis relancez l'installation ; le rapport du gestionnaire de "
            "paquets est dans le journal.",
            "नेटवर्क त्रुटि के कारण WARP पैकेज डाउनलोड नहीं हो सका। Zapret बायपास चालू करें और "
            "इंस्टॉल दोहराएँ; पैकेज प्रबंधक की रिपोर्ट लॉग में है।"),
        // The footer's line when the user presses No. It names the consequence the pill now shows
        // (zapret mode) rather than the decision itself, because the interface did not refuse — the
        // user did, and the line is there to say the interface heard and moved back.
        ROW("warp.install_declined", "Установка отменена — переключено в режим zapret",
            "Installation cancelled — switched back to zapret mode",
            "Instalación cancelada: se ha vuelto al modo de zapret",
            "Installation abgebrochen — zurück in den Zapret-Modus",
            "安装已取消——已切回 zapret 模式",
            "Installation annulée — retour au mode zapret",
            "इंस्टॉल रद्द — zapret मोड पर वापस बदल दिया गया"),
        // Said instead of an offer, on a machine the package databases describe as already owning the
        // client. %1 is where the answer came from — "dpkg", "snap", "flatpak", "rpm", "service",
        // "registry" — because "it is already installed" is only reassuring if it says by what, and the
        // half of the interface that stayed dark is otherwise unexplained.
        ROW("warp.already_installed", "Cloudflare WARP уже установлен в системе (%1) — использую "
            "существующий клиент",
            "Cloudflare WARP is already installed on this machine (%1) — using the existing client",
            "Cloudflare WARP ya está instalado en este equipo (%1): se usará el cliente existente",
            "Cloudflare WARP ist auf diesem Rechner bereits installiert (%1) — der vorhandene Client "
            "wird verwendet",
            "此电脑上已安装 Cloudflare WARP（%1）——使用现有的客户端",
            "Cloudflare WARP est déjà installé sur cette machine (%1) — utilisation du client existant",
            "या संगणकावर Cloudflare WARP आधीच इंस्टॉल आहे (%1) — अस्तित्वातील क्लाइंट वापरत आहोत"),
        // The other reason a lit half goes dark without anybody asking it to: the client is here, and
        // it does not answer. Distinct from an absence because the fix is different — this machine needs
        // its package repaired, not a new one — and the offer that follows says so in those words.
        ROW("warp.client_broken", "Клиент Cloudflare WARP найден, но не отвечает — требуется "
            "переустановка",
            "The Cloudflare WARP client was found but does not answer — it needs reinstalling",
            "Se ha encontrado el cliente de Cloudflare WARP, pero no responde: hay que reinstalarlo",
            "Der Cloudflare-WARP-Client wurde gefunden, antwortet aber nicht — er muss neu "
            "installiert werden",
            "已找到 Cloudflare WARP 客户端，但它没有响应——需要重新安装",
            "Le client Cloudflare WARP a été trouvé mais ne répond pas — il faut le réinstaller",
            "Cloudflare WARP क्लाइंट सापडला पण प्रतिसाद देत नाही — त्याला परत इंस्टॉल करावे लागेल"),
        // The single-window policy's own line: it only writes when it actually took a window down, so
        // a host with no WARP GUI running never sees it.
        ROW("warp.gui_closed", "Закрыто окно Cloudflare WARP: %1",
            "Closed the Cloudflare WARP window: %1", "Se cerró la ventana de Cloudflare WARP: %1",
            "Cloudflare-WARP-Fenster geschlossen: %1", "已关闭 Cloudflare WARP 窗口：%1",
            "Fenêtre Cloudflare WARP fermée : %1",
            "Cloudflare WARP विंडो बंद की गई: %1"),

        // ---- enrolling the client with Cloudflare ----
        // Distinct from installing it: the package can be on the machine and still have no
        // registration, which is the state where `connect` is accepted and never completes. The first
        // line is said while that is being fixed — enrolling talks to Cloudflare and takes seconds — so
        // a lit button that has not moved yet has a reason next to it. It promises nothing beyond the
        // attempt, because the attempt is what is running.
        ROW("warp.enrolling", "Первичная настройка WARP…",
            "Setting WARP up for the first time…", "Configurando WARP por primera vez…",
            "WARP wird zum ersten Mal eingerichtet…", "正在进行 WARP 首次设置…",
            "Première configuration de WARP…", "WARP को पहली बार सेट किया जा रहा है…"),
        // The attempt's failure, in the window's words rather than warp-cli's, which are written into
        // the journal right above this line. Worth saying because the cost of a failed enrolment is not
        // an error dialog but a tunnel that is simply never brought up, and that needs naming.
        ROW("warp.enroll_failed", "Не удалось настроить WARP — туннель не запускается",
            "WARP could not be set up — the tunnel is not being started",
            "No se pudo configurar WARP: el túnel no se iniciará",
            "WARP konnte nicht eingerichtet werden — der Tunnel wird nicht gestartet",
            "无法设置 WARP——不会启动隧道",
            "Impossible de configurer WARP — le tunnel n'est pas lancé",
            "WARP सेट नहीं हो सका — टनल शुरू नहीं किया जा रहा"),
        // ---- the bypass lent to a WARP install, and given back ----
        //
        // Three lines for one loan, and each is worth its own sentence because each is a machine left in a
        // different state. A user who sees apt reach Cloudflare on a network that has never managed it
        // should be able to read why; one who finds the queue rules gone afterwards should be able to
        // read that too; and one who asked for Zapret + WARP must not be left wondering whether Muffin is
        // about to take their bypass down as collateral.
        ROW("warp.zapret_borrowed", "Zapret временно запущен для установки WARP",
            "Zapret was started temporarily for the WARP install",
            "Zapret se inició temporalmente para instalar WARP",
            "Zapret wurde für die WARP-Installation vorübergehend gestartet",
            "Zapret 已为安装 WARP 临时启动",
            "Zapret a été démarré temporairement pour l'installation de WARP",
            "WARP इंस्टॉल के लिए Zapret अस्थायी रूप से शुरू किया गया"),
        ROW("warp.zapret_released", "Zapret остановлен после установки WARP",
            "Zapret was stopped after the WARP install",
            "Zapret se detuvo tras instalar WARP",
            "Zapret wurde nach der WARP-Installation gestoppt",
            "WARP 安装完成后已停止 Zapret",
            "Zapret a été arrêté après l'installation de WARP",
            "WARP इंस्टॉल के बाद Zapret बंद कर दिया गया"),
        ROW("warp.zapret_kept", "Zapret продолжает работать для режима Zapret + WARP",
            "Zapret keeps running for the Zapret + WARP mode",
            "Zapret sigue en marcha para el modo Zapret + WARP",
            "Zapret läuft für den Modus Zapret + WARP weiter",
            "Zapret 继续运行以支持 Zapret + WARP 模式",
            "Zapret continue de fonctionner pour le mode Zapret + WARP",
            "Zapret + WARP मोड के लिए Zapret चलता रहेगा"),
        // ---- what the preparation says while it runs ----
        //
        // Three sentences for the three waits a tunnel start now contains. They exist because the
        // preparation is the first step of this app that can take a minute of visible nothing — the
        // service restart goes through a root prompt, and a program that has asked for a password is
        // not a program that has frozen. Each one names the thing happening rather than promising an
        // outcome, and the footer carries them so the answer to "what is it waiting on" is on screen
        // rather than in the journal the user has to open to find out.
        ROW("warp.prepare.protocol", "Переключаю WARP на протокол MASQUE…",
            "Switching WARP to the MASQUE protocol…", "Cambiando WARP al protocolo MASQUE…",
            "WARP wird auf das MASQUE-Protokoll umgestellt…", "正在将 WARP 切换到 MASQUE 协议…",
            "Basculement de WARP vers le protocole MASQUE…",
            "WARP को MASQUE प्रोटोकॉल पर बदला जा रहा है…"),
        ROW("warp.prepare.service", "Перезапускаю службу WARP…", "Restarting the WARP service…",
            "Reiniciando el servicio de WARP…", "WARP-Dienst wird neu gestartet…",
            "正在重启 WARP 服务…", "Redémarrage du service WARP…",
            "WARP सेवा फिर से शुरू की जा रही है…"),
        ROW("warp.prepare.waiting", "Жду запуска службы WARP…",
            "Waiting for the WARP service to come back…",
            "Esperando a que el servicio de WARP se recupere…",
            "Warten auf den WARP-Dienst…", "正在等待 WARP 服务恢复…",
            "En attente du retour du service WARP…", "WARP सेवा के लौटने की प्रतीक्षा…"),
        // The preparation was asked for and could not be completed. Distinct from enroll_failed, which
        // is a device that could not be enrolled: this one is the machine not being made ready, and the
        // tunnel is still attempted, so the sentence says what was skipped rather than that nothing
        // happened.
        ROW("warp.prepare_failed", "Muffin не смог полностью подготовить WARP — пробую подключить как есть",
            "Muffin could not fully prepare WARP — trying to connect as it stands",
            "Muffin no pudo preparar WARP por completo: intentando conectar tal cual",
            "Muffin konnte WARP nicht vollständig vorbereiten — Versuch mit dem aktuellen Zustand",
            "Muffin 无法完全准备 WARP——按现状尝试连接",
            "Muffin n'a pas pu préparer entièrement WARP — connexion tentée en l'état",
            "Muffin WARP को पूरी तरह तैयार नहीं कर सका — मौजूदा हालत में जोड़ने का प्रयास"),

        // The tunnel was asked for and never came up, after the protocol was switched and the service
        // restarted. The one case where the honest answer is that the problem is not in this program:
        // the provider is the remaining suspect, and saying so saves the user from restarting Muffin
        // forever over a tunnel their link will not carry.
        ROW("warp.connect_timeout",
            "WARP не поднялся за отведённое время. Протокол MASQUE установлен и служба перезапущена — "
            "похоже, провайдер блокирует сам WARP. Проверьте, открывается ли Cloudflare на этом соединении",
            "WARP did not come up in time. The MASQUE protocol was set and the service restarted, so "
            "the provider appears to be blocking WARP itself — check whether Cloudflare is reachable "
            "on this connection at all",
            "WARP no se inició a tiempo. Se definió el protocolo MASQUE y se reinició el servicio, así "
            "que todo indica que el proveedor bloquea WARP: comprueba si Cloudflare es alcanzable en "
            "esta conexión",
            "WARP kam nicht rechtzeitig hoch. MASQUE-Protokoll wurde gesetzt und der Dienst neu "
            "gestartet — es bleibt, dass der Anbieter WARP selbst blockiert: prüfen Sie, ob Cloudflare "
            "über diese Verbindung überhaupt erreichbar ist",
            "WARP 未能按时启动。已设置 MASQUE 协议并重启服务——因此请检查你的网络服务商是否直接屏蔽了 Cloudflare",
            "WARP ne s'est pas établi à temps. Le protocole MASQUE a été appliqué et le service "
            "redémarré : le fournisseur bloque probablement WARP lui-même — vérifiez si Cloudflare est "
            "accessible sur cette connexion",
            "WARP समय पर नहीं आया। MASQUE प्रोटोकॉल सेट किया और सेवा फिर शुरू की — प्रतीत होता है "
            "प्रदाता स्वयं WARP को रोक रहा है; जाँचें कि इस कनेक्शन पर Cloudflare पहुँच में है या नहीं"),

        // ---- the setup wizard ----
        //
        // A machine with no WARP at all needs four things done in order, and doing them silently is how
        // the user ends up staring at a lit toggle and a daemon that says `warp=off`. These strings are
        // the wizard's whole vocabulary: what it is about to do, which of the steps it is in, what broke,
        // and that it finished. Every one of them names a thing rather than promising an outcome, because
        // a wizard that says "installing" while a password prompt is waiting on the other window is the
        // difference between a user who types it and a user who kills the app.
        ROW("warp.setup.title", "Настройка Cloudflare WARP", "Cloudflare WARP setup",
            "Configuración de Cloudflare WARP", "Cloudflare WARP einrichten", "设置 Cloudflare WARP",
            "Configuration de Cloudflare WARP", "Cloudflare WARP सेटअप"),
        ROW("warp.setup.intro",
            "Для работы WARP необходимо установить клиент, зарегистрировать устройство и настроить "
            "сетевой протокол. Нажмите «Начать настройку», чтобы продолжить. Потребуются права "
            "администратора.",
            "WARP needs its client installed, this device registered and the tunnel protocol set. Press "
            "“Start setup” to continue. Administrator rights will be requested.",
            "WARP necesita instalar el cliente, registrar este dispositivo y definir el protocolo del "
            "túnel. Pulsa «Iniciar configuración» para continuar. Se pedirán permisos de administrador.",
            "WARP erfordert die Installation des Clients, die Registrierung dieses Geräts und die "
            "Einstellung des Tunnelprotokolls. Klicken Sie auf „Einrichtung starten“. Es werden "
            "Administratorrechte angefordert.",
            "WARP 需要安装客户端、注册此设备并设置隧道协议。点击“开始设置”继续。系统将请求管理员权限。",
            "WARP a besoin que le client soit installé, que cet appareil soit enregistré et que le "
            "protocole du tunnel soit défini. Cliquez sur « Démarrer la configuration ». Les droits "
            "d'administration seront demandés.",
            "WARP के लिए क्लाइंट इंस्टॉल करना, इस डिवाइस को पंजीकृत करना और टनल प्रोटोकॉल सेट करना "
            "ज़रूरी है। जारी रखने के लिए «सेटअप शुरू करें» दबाएँ। व्यवस्थापक अधिकार माँगे जाएँगे।"),
        ROW("warp.setup.start", "Начать настройку", "Start setup", "Iniciar configuración",
            "Einrichtung starten", "开始设置", "Démarrer la configuration", "सेटअप शुरू करें"),
        // The same button after a failure. It is a retry of the step that failed, not a fresh chain from
        // the top, and the word says so.
        ROW("warp.setup.retry", "Повторить", "Try again", "Reintentar", "Erneut versuchen", "重试",
            "Réessayer", "फिर कोशिश करें"),
        ROW("warp.setup.close", "Закрыть", "Close", "Cerrar", "Schließen", "关闭", "Fermer",
            "बंद करें"),
        // The running status line. %1 and %2 are the step's position, %3 is its own sentence, so the
        // numbering can never disagree with the plan the wizard is walking.
        ROW("warp.setup.step", "Шаг %1 из %2: %3", "Step %1 of %2: %3", "Paso %1 de %2: %3",
            "Schritt %1 von %2: %3", "第 %1 步，共 %2 步：%3", "Étape %1 sur %2 : %3",
            "चरण %1 / %2: %3"),
        // The steps' own sentences, keyed by the id the plan in WarpManager::setupSteps() carries
        // so a step can never be announced by the wrong words.
        ROW("warp.setup.step.install", "Установка пакета…", "Installing the package…",
            "Instalando el paquete…", "Paket wird installiert…", "正在安装软件包…",
            "Installation du paquet…", "पैकेज इंस्टॉल हो रहा है…"),
        ROW("warp.setup.step.daemon", "Запуск службы WARP…", "Starting the WARP service…",
            "Iniciando el servicio de WARP…", "WARP-Dienst wird gestartet…", "正在启动 WARP 服务…",
            "Démarrage du service WARP…", "WARP सेवा शुरू हो रही है…"),
        ROW("warp.setup.step.register", "Регистрация устройства…", "Registering the device…",
            "Registrando el dispositivo…", "Gerät wird registriert…", "正在注册设备…",
            "Enregistrement de l'appareil…", "डिवाइस पंजीकृत हो रहा है…"),
        ROW("warp.setup.step.protocol", "Настройка протокола MASQUE…", "Setting the MASQUE protocol…",
            "Definiendo el protocolo MASQUE…", "MASQUE-Protokoll wird eingestellt…",
            "正在设置 MASQUE 协议…", "Réglage du protocole MASQUE…",
            "MASQUE प्रोटोकॉल सेट हो रहा है…"),
        ROW("warp.setup.step.service", "Перезапуск службы WARP…", "Restarting the WARP service…",
            "Reiniciando el servicio de WARP…", "WARP-Dienst wird neu gestartet…",
            "正在重启 WARP 服务…", "Redémarrage du service WARP…",
            "WARP सेवा फिर से शुरू हो रही है…"),
        ROW("warp.setup.step.connect", "Подключение туннеля…", "Connecting the tunnel…",
            "Conectando el túnel…", "Tunnel wird verbunden…", "正在连接隧道…",
            "Connexion du tunnel…", "टनल जुड़ रहा है…"),
        // A failure, named by the step that produced it and quoting what the command said. The step
        // number is in the sentence rather than only in the log because the log is the long version:
        // this is what the user reads first.
        ROW("warp.setup.error", "Ошибка на шаге %1: %2", "Step %1 failed: %2",
            "Error en el paso %1: %2", "Fehler in Schritt %1: %2", "第 %1 步出错：%2",
            "Échec à l'étape %1 : %2", "चरण %1 विफल: %2"),
        // ---- the one failure whose cause the user can fix with a single command ----
        //
        // %1 is the service manager's own verb for this platform, composed by
        // WarpManager::serviceStartHint() rather than written here: a catalog that spelled "systemctl"
        // into seven languages is a catalog that would keep saying it to a Windows user. Shown under a
        // refusal that names the daemon, and under a failure of the step that was meant to start it —
        // because "Missing registration. Try running: 'warp-cli registration new'" is what a healthy
        // client prints when its daemon is dead, and read at its word it sends the user to enrol a
        // device that cannot be enrolled.
        ROW("warp.setup.daemon_down",
            "Не удалось запустить службу WARP. Попробуйте выполнить '%1' вручную.",
            "The WARP service could not be started. Try running '%1' by hand.",
            "No se pudo iniciar el servicio de WARP. Pruebe a ejecutar '%1' manualmente.",
            "Der WARP-Dienst konnte nicht gestartet werden. Versuchen Sie, '%1' manuell auszuführen.",
            "无法启动 WARP 服务。请尝试手动执行 '%1'。",
            "Le service WARP n'a pas pu être démarré. Essayez d'exécuter '%1' manuellement.",
            "WARP सेवा शुरू नहीं हो सकी। '%1' स्वयं चलाने का प्रयास करें।"),
        ROW("warp.setup.done", "WARP успешно настроен и подключен!",
            "WARP is set up and connected!", "¡WARP está configurado y conectado!",
            "WARP ist eingerichtet und verbunden!", "WARP 已设置并连接成功！",
            "WARP est configuré et connecté !", "WARP सफलतापूर्वक सेट और कनेक्ट हो गया!"),
        // Walking away is a legitimate answer, and saying so is what keeps it from reading as a crash:
        // the dialog closes, the half goes dark, and the journal names which step it stopped at.
        ROW("warp.setup.cancelled", "Настройка WARP прервана на шаге: %1",
            "WARP setup was cancelled during: %1", "Se canceló la configuración de WARP durante: %1",
            "Die WARP-Einrichtung wurde abgebrochen bei: %1", "WARP 设置已在此步骤取消：%1",
            "La configuration de WARP a été annulée pendant : %1",
            "WARP सेटअप इस चरण में रद्द किया गया: %1"),
        // ---- the gap the daemon owes after a step moved it, and the retry a refusal earns ----
        //
        // Two sentences for the two waits the chain now contains. Both exist because the wizard runs
        // commands, and a command that returns has not necessarily made the thing it changed usable:
        // systemd says warp-svc is active before the daemon has re-opened its control socket, and the verb
        // issued in that gap is refused by an answer about the service that the step's own title blames on
        // the device. Silence reads as a stuck program; these two lines say which of the two it is.
        ROW("warp.setup.settle", "Служба перезапущена — жду %1 с, пока демон WARP снова откроет сокет…",
            "The service was restarted — waiting %1 s for the WARP daemon to re-open its socket…",
            "El servicio se reinició: esperando %1 s a que el demonio de WARP reabra su socket…",
            "Der Dienst wurde neu gestartet — %1 s gewartet, bis der WARP-Demon seinen Socket wieder öffnet…",
            "服务已重启 — 等待 %1 秒让 WARP 守护进程重新打开套接字…",
            "Le service a été redémarré — attente de %1 s avant que le démon WARP rouvre sa socket…",
            "सेवा फिर से शुरू हुई — WARP डेमन के सॉकेट के फिर से खुलने की प्रतीक्षा %1 स…"),
        ROW("warp.setup.retry_daemon", "Демон WARP ещё не отвечает — повторная попытка %1 из %2…",
            "The WARP daemon is not answering yet — retry %1 of %2…",
            "El demonio de WARP aún no responde: reintento %1 de %2…",
            "Der WARP-Demon antwortet noch nicht — Versuch %1 von %2…",
            "WARP 守护进程尚未响应 — 第 %1 次重试（共 %2 次）…",
            "Le démon WARP ne répond pas encore — nouvelle tentative %1 sur %2…",
            "WARP डेमन अभी जवाब नहीं दे रहा — पुनःप्रयास %1 / %2…"),
        // ---- the rescan between the install and everything that depends on it ----
        //
        // Three lines, and the reason they exist is a specific failure: the install step succeeds, and
        // the next one is refused with "warp-cli is not installed on this machine" — a true sentence
        // about the machine as it was before the package manager ran. Saying the check happened, and
        // what it found, is what turns that from a contradiction into a diagnosis.
        ROW("warp.setup.rescan", "Проверка установки WARP…", "Checking the WARP installation…",
            "Comprobando la instalación de WARP…", "WARP-Installation wird geprüft…",
            "正在检查 WARP 安装…", "Vérification de l'installation de WARP…",
            "WARP इंस्टॉलेशन जाँचा जा रहा है…"),
        ROW("warp.setup.rescan_found", "Проверка установки WARP… Найден: %1",
            "Checking the WARP installation… Found: %1",
            "Comprobando la instalación de WARP… Encontrado: %1",
            "WARP-Installation geprüft — gefunden: %1", "检查 WARP 安装…已找到：%1",
            "Vérification de l'installation de WARP… Trouvé : %1",
            "WARP इंस्टॉलेशन जाँचा गया… मिला: %1"),
        ROW("warp.setup.rescan_missing",
            "Пакет сообщил об успехе, но warp-cli не найден ни в PATH, ни в известных каталогах — "
            "следующий шаг, скорее всего, будет отклонён",
            "The package reported success, but warp-cli was found neither on PATH nor in the directories "
            "it is known to use — the next step will most likely be refused",
            "El paquete informó de éxito, pero warp-cli no se encuentra ni en PATH ni en los directorios "
            "conocidos: es probable que el siguiente paso sea rechazado",
            "Das Paket meldete Erfolg, aber warp-cli wurde weder im PATH noch in den bekannten "
            "Verzeichnissen gefunden — der nächste Schritt wird wahrscheinlich abgelehnt",
            "软件包报告成功，但在 PATH 和已知目录中都找不到 warp-cli——下一步很可能会被拒绝",
            "Le paquet a signalé un succès, mais warp-cli est introuvable dans le PATH comme dans les "
            "répertoires connus — l'étape suivante sera probablement refusée",
            "पैकेज ने सफलता बताई, पर warp-cli न PATH में मिला न ज्ञात निर्देशिकाओं में — अगला चरण "
            "संभवतः अस्वीकृत होगा"),


        // ---- the history and statistics page ----
        // The page's own voice: one aggregate the reader acts on, five column names, and the three words
        // a row can be. The verdict words are short on purpose — they sit in a column the width of the
        // longest of them, and a sentence there would push the record off the screen.
        ROW("nav.history_tip", "Статистика и история проверок обхода",
            "Bypass-check statistics and history", "Estadísticas e historial de las comprobaciones",
            "Statistik und Verlauf der Bypass-Prüfungen", "绕过检查统计与历史",
            "Statistiques et historique des contrôles", "बाईपास जाँच के आँकड़े और इतिहास"),
        ROW("history.title", "Статистика и История", "Statistics & History",
            "Estadísticas e historial", "Statistik & Verlauf", "统计与历史",
            "Statistiques et historique", "आँकड़े और इतिहास"),
        ROW("history.headline", "Успешных проверок: %1%", "Successful checks: %1%",
            "Comprobaciones exitosas: %1 %", "Erfolgreiche Prüfungen: %1 %", "检查成功率：%1%",
            "Contrôles réussis : %1 %", "उत्तीर्ण जाँच: %1%"),
        // Said instead of a percentage. "0%" would be a claim about checks that never happened, and the
        // difference between "not yet" and "all of them failed" is the whole reason to open this page.
        ROW("history.headline.empty", "Проверок ещё не было", "No checks yet",
            "Todavía no hay comprobaciones", "Noch keine Prüfungen", "尚无检查记录",
            "Aucun contrôle pour l'instant", "अभी कोई जाँच नहीं हुई"),
        // Three counts, three chips, one number per chip: a chip is coloured on its own, so the wording
        // has to fit a small plate. The third is deliberately terse ("Подмен", not "Подмен сертификата")
        // and carries the full sentence as its tooltip — the column it counts spells the phrase out.
        ROW("history.badge.total", "Всего: %1", "Total: %1", "Total: %1", "Gesamt: %1", "总数：%1",
            "Total : %1", "कुल: %1"),
        ROW("history.badge.failed", "Ошибок: %1", "Failed: %1", "Errores: %1", "Fehler: %1",
            "失败：%1", "Échecs : %1", "विफल: %1"),
        ROW("history.badge.cert", "Подмен: %1", "Substituted: %1", "Sustituidos: %1",
            "Ausgetauscht: %1", "替换：%1", "Substitués : %1", "प्रतिस्थापित: %1"),
        ROW("history.col.time", "Время", "Time", "Hora", "Zeit", "时间", "Heure", "समय"),
        ROW("history.col.preset", "Профиль", "Profile", "Perfil", "Profil", "配置文件",
            "Profil", "प्रोफ़ाइल"),
        ROW("history.col.host", "Хост", "Host", "Servidor", "Host", "主机", "Hôte", "होस्ट"),
        ROW("history.col.verdict", "Статус", "Status", "Estado", "Status", "状态", "État",
            "स्थिति"),
        ROW("history.col.latency", "Задержка", "Latency", "Retardo", "Latenz", "延迟",
            "Latence", "विलंब"),
        ROW("history.verdict.passed", "Успех", "Passed", "Superada", "Bestanden", "通过",
            "Réussi", "उत्तीर्ण"),
        ROW("history.verdict.failed", "Ошибка", "Failed", "Fallida", "Fehlgeschlagen", "失败",
            "Échoué", "विफल"),
        // Not "cert invalid": the row's whole point is that the certificate was somebody else's, and
        // saying "invalid" lets the reader blame the service instead of the path in the middle.
        ROW("history.verdict.intercepted", "Подмена сертификата", "Certificate substituted",
            "Certificado sustituido", "Zertifikat ausgetauscht", "证书被替换",
            "Certificat substitué", "प्रमाणपत्र प्रतिस्थापित"),
        ROW("history.verdict.intercepted_tip",
            "Соединение прошло, но сертификат сервиса не подтверждён — возможен перехват трафика",
            "The connection went through, but the service's certificate was not the expected one — "
            "the traffic may be intercepted",
            "La conexión pasó, pero el certificado del servicio no fue el esperado: el tráfico "
            "podría estar interceptado",
            "Die Verbindung kam durch, aber das Zertifikat des Dienstes war nicht das erwartete — "
            "der Verkehr könnte abgefangen werden",
            "连接已建立，但服务证书未通过验证——流量可能被劫持",
            "La connexion est passée, mais le certificat du service n'était pas celui attendu — "
            "le trafic est peut-être intercepté",
            "कनेक्शन हो गई, पर सेवा का प्रमाणपत्र अपेक्षित नहीं था — ट्रैफ़िक अवरोधित हो सकता है"),
        ROW("history.verdict.failed_tip", "Соединение не установлено", "No connection was made",
            "No se estableció conexión", "Es wurde keine Verbindung hergestellt", "未建立连接",
            "Aucune connexion n'a été établie", "कोई कनेक्शन नहीं बना"),
        ROW("history.empty",
            "История пока пуста. Запустите проверку обхода или включите Watchdog",
            "The history is empty so far. Run a bypass check, or let the Watchdog run one for you",
            "El historial está vacío por ahora. Ejecute una comprobación de evasión o deje que "
            "Watchdog la ejecute",
            "Der Verlauf ist noch leer. Führen Sie eine Bypass-Prüfung aus oder überlassen Sie "
            "das dem Watchdog",
            "历史目前为空。请运行绕过检查，或启用看门狗",
            "L'historique est vide pour l'instant. Lancez une vérification de contournement, ou "
            "laissez Watchdog en lancer une",
            "इतिहास अभी खाली है। एक बाईपास जाँच चलाएँ, या वॉचडॉग को चलने दें"),
        ROW("history.clear", "Очистить историю", "Clear history", "Borrar historial",
            "Verlauf löschen", "清除历史", "Effacer l'historique", "इतिहास साफ़ करें"),
        ROW("history.clear_tip", "Забыть все записанные проверки", "Forget every recorded check",
            "Olvidar todas las comprobaciones registradas",
            "Alle aufgezeichneten Prüfungen vergessen", "忘记所有已记录的检查",
            "Oublier tous les contrôles enregistrés", "दर्ज की गई सारी जाँचें भूल जाएँ"),
        ROW("history.clear.title", "Очистить историю?", "Clear the history?",
            "¿Borrar el historial?", "Verlauf löschen?", "清除历史？", "Effacer l'historique ?",
            "इतिहास साफ़ करें?"),
        // The warning names how much is about to go, and why it is worth keeping: for a tool whose whole
        // job is to remember whether the path worked, the record is the evidence, not a cache.
        ROW("history.clear.confirm",
            "Будет удалено %1 записей. Это действие нельзя отменить — история и есть доказательство "
            "того, что обход раньше работал.",
            "%1 records will be removed. This cannot be undone — the history is the evidence that the "
            "bypass used to work.",
            "Se eliminarán %1 registros. No se puede deshacer: el historial es la prueba de que la "
            "evasión funcionaba antes.",
            "%1 Einträge werden entfernt. Das ist nicht rückgängig zu machen — der Verlauf ist der "
            "Beleg dafür, dass der Bypass früher funktionierte.",
            "将删除 %1 条记录。此操作无法撤销——历史记录正是绕过曾经有效的证据。",
            "%1 entrées seront supprimées. C'est irréversible : l'historique est la preuve que le "
            "contournement fonctionnait avant.",
            "%1 रिकॉर्ड हट जाएँगे। यह वापस नहीं होगा — इतिहास ही यह प्रमाण है कि बाईपास पहले चलता था।"),
        ROW("history.cleared", "История очищена: удалено %1 записей",
            "History cleared: %1 records removed", "Historial borrado: %1 registros eliminados",
            "Verlauf gelöscht: %1 Einträge entfernt", "历史已清除：删除 %1 条记录",
            "Historique effacé : %1 entrées supprimées", "इतिहास साफ़ हुआ: %1 रिकॉर्ड हटे"),


        // ---- the profile row's menu ----
        ROW("profile.copy", "Скопировать", "Copy", "Copiar", "Kopieren", "复制", "Copier",
            "कॉपी करें"),
        ROW("profile.delete", "Удалить", "Delete", "Eliminar", "Löschen", "删除", "Supprimer",
            "हटाएँ"),
        ROW("profile.menu_tip", "Действия с профилем", "Actions for this profile",
            "Acciones de este perfil", "Aktionen für dieses Profil", "此档案的操作",
            "Actions pour ce profil", "इस प्रोफ़ाइल की क्रियाएँ"),
        ROW("profile.copied", "Команда скопирована: %1", "Command copied: %1",
            "Comando copiado: %1", "Befehl kopiert: %1", "命令已复制：%1",
            "Commande copiée : %1", "कमांड कॉपी हुई: %1"),
        ROW("profile.delete_title", "Удалить профиль", "Delete profile", "Eliminar perfil",
            "Profil löschen", "删除档案", "Supprimer le profil", "प्रोफ़ाइल हटाएँ"),
        ROW("profile.delete_confirm",
            "Профиль — это файл в папке zapret. Удалить %1?",
            "A profile is a file in the zapret folder. Delete %1?",
            "Un perfil es un archivo de la carpeta de zapret. ¿Eliminar %1?",
            "Ein Profil ist eine Datei im zapret-Ordner. %1 löschen?",
            "档案就是 zapret 文件夹中的一个文件。删除 %1？",
            "Un profil est un fichier du dossier zapret. Supprimer %1 ?",
            "प्रोफ़ाइल zapret फ़ोल्डर की एक फ़ाइल है। %1 हटाएँ?"),
        ROW("profile.deleted", "Профиль удалён: %1", "Profile deleted: %1",
            "Perfil eliminado: %1", "Profil gelöscht: %1", "档案已删除：%1",
            "Profil supprimé : %1", "प्रोफ़ाइल हटाई गई: %1"),
        ROW("profile.delete_failed", "Не удалось удалить файл: %1", "Could not delete the file: %1",
            "No se pudo eliminar el archivo: %1", "Die Datei ließ sich nicht löschen: %1",
            "无法删除文件：%1", "Impossible de supprimer le fichier : %1",
            "फ़ाइल हटाई नहीं जा सकी: %1"),
        // ---- the profiles page ----
        ROW("placeholder.no_strategies",
            "Стратегии не найдены.\n\nСкачайте zapret сами — или укажите папку, которая уже есть на диске.",
            "No strategies found.\n\nDownload zapret yourself — or point to a folder that is already on this disk.",
            "No se encontraron estrategias.\n\nDescargue zapret usted mismo, o indique una carpeta que ya esté en el disco.",
            "Keine Strategien gefunden.\n\nLaden Sie zapret selbst herunter — oder wählen Sie einen Ordner, der bereits auf der Platte liegt.",
            "未找到策略。\n\n请自行下载 zapret，或指向磁盘上已有的文件夹。",
            "Aucune stratégie trouvée.\n\nTéléchargez zapret vous-même — ou indiquez un dossier déjà présent sur le disque.",
            "कोई रणनीति नहीं मिली।\n\nखुद zapret डाउनलोड करें — या डिस्क पर पहले से मौजूद फ़ोल्डर चुनें।"),

        // ---- the journal (LogPanel) ----
        ROW("log.copy", "Копировать", "Copy", "Copiar", "Kopieren", "复制", "Copier", "कॉपी करें"),
        ROW("log.copy_tip", "Скопировать весь лог в буфер обмена",
            "Copy the whole log to the clipboard", "Copiar todo el registro al portapapeles",
            "Das gesamte Protokoll in die Zwischenablage kopieren", "将完整日志复制到剪贴板",
            "Copier tout le journal dans le presse-papiers", "पूरा लॉग क्लिपबोर्ड पर कॉपी करें"),
        ROW("log.clear", "Очистить", "Clear", "Limpiar", "Leeren", "清空", "Effacer", "साफ़ करें"),
        ROW("log.clear_tip", "Очистить лог", "Clear the log", "Borrar el registro",
            "Protokoll löschen", "清空日志", "Effacer le journal", "लॉग साफ़ करें"),
        ROW("log.save", "Сохранить в файл", "Save to file", "Guardar en archivo",
            "In Datei speichern", "保存到文件", "Enregistrer dans un fichier", "फ़ाइल में सहेजें"),
        ROW("log.save_tip", "Сохранить лог в файл", "Save the log to a file",
            "Guardar el registro en un archivo", "Protokoll in eine Datei speichern",
            "将日志保存到文件", "Enregistrer le journal dans un fichier", "लॉग को फ़ाइल में सहेजें"),
        ROW("log.level", "Уровень", "Level", "Nivel", "Stufe", "级别", "Niveau", "स्तर"),
        ROW("log.filter.all", "Все", "All", "Todos", "Alle", "全部", "Tous", "सभी"),
        ROW("log.filter.info", "Инфо", "Info", "Info", "Info", "信息", "Info", "जानकारी"),
        ROW("log.filter.warn", "Предупреждение", "Warning", "Advertencia", "Warnung", "警告",
            "Avertissement", "चेतावनी"),
        ROW("log.filter.error", "Ошибка", "Error", "Error", "Fehler", "错误", "Erreur", "त्रुटि"),
        ROW("log.save_title", "Сохранить логи", "Save logs", "Guardar registros",
            "Protokolle speichern", "保存日志", "Enregistrer les journaux", "लॉग सहेजें"),
        ROW("log.save_filters", "Логи (*.log);;Текст (*.txt);;Все файлы (*)",
            "Logs (*.log);;Text (*.txt);;All files (*)",
            "Registros (*.log);;Texto (*.txt);;Todos los archivos (*)",
            "Protokolle (*.log);;Text (*.txt);;Alle Dateien (*)",
            "日志 (*.log);;文本 (*.txt);;所有文件 (*)",
            "Journaux (*.log);;Texte (*.txt);;Tous les fichiers (*)",
            "लॉग (*.log);; टेक्स्ट (*.txt);; सभी फ़ाइलें (*)"),
        ROW("log.save_failed_title", "Сохранение логов", "Saving logs", "Guardando registros",
            "Protokolle werden gespeichert", "正在保存日志", "Enregistrement des journaux",
            "लॉग सहेजे जा रहे हैं"),
        ROW("log.save_failed", "Не удалось записать файл: %1", "Could not write the file: %1",
            "No se pudo escribir el archivo: %1", "Die Datei konnte nicht geschrieben werden: %1",
            "无法写入文件：%1", "Impossible d'écrire le fichier : %1",
            "फ़ाइल लिखी नहीं जा सकी: %1"),

        // ---- strategy cards ----
        ROW("card.start", "Запустить", "Start", "Iniciar", "Starten", "启动", "Démarrer", "चलाएँ"),
        ROW("card.stop", "Остановить", "Stop", "Detener", "Stoppen", "停止", "Arrêter", "रोकें"),
        ROW("card.retry", "Повторить", "Retry", "Reintentar", "Wiederholen", "重试", "Réessayer",
            "पुनः प्रयास करें"),
        ROW("card.need_root", "нужен root", "root required", "se requiere root",
            "root erforderlich", "需要 root", "root requis", "root आवश्यक है"),
        ROW("card.need_admin", "нужен администратор", "administrator required",
            "se requiere administrador", "Administrator erforderlich", "需要管理员",
            "administrateur requis", "प्रशासक आवश्यक है"),
        ROW("card.kind.shell", "init-скрипт", "init script", "script init", "init-Skript",
            "init 脚本", "script init", "init स्क्रिप्ट"),
        ROW("card.kind.binary", "бинарник", "binary", "binario", "Binary", "二进制程序", "binaire",
            "बाइनरी"),
        ROW("card.desc.binary", "Прямой запуск %1", "Runs %1 directly", "Ejecuta %1 directamente",
            "Direktstart von %1", "直接运行 %1", "Exécution directe de %1", "%1 को सीधे चलाएँ"),
        ROW("card.desc.args", ", %1 арг.", ", %1 args", ", %1 arg.", ", %1 Argumente",
            "，%1 个参数", ", %1 arg.", ", %1 तर्क"),
        ROW("card.desc.batch", "Обёртка cmd.exe /c %1", "cmd.exe /c wrapper for %1",
            "Envoltorio cmd.exe /c para %1", "cmd.exe-/c-Wrapper für %1", "cmd.exe /c 封装：%1",
            "Enveloppe cmd.exe /c pour %1", "cmd.exe /c रैपर: %1"),
        ROW("card.desc.powershell", "Запуск через PowerShell: %1", "Runs through PowerShell: %1",
            "Ejecución mediante PowerShell: %1", "Start über PowerShell: %1",
            "通过 PowerShell 运行：%1", "Exécution via PowerShell : %1",
            "PowerShell से चलाएँ: %1"),
        ROW("card.desc.shell", "Init-скрипт zapret (%1 start/stop)",
            "zapret init script (%1 start/stop)", "Script init de zapret (%1 start/stop)",
            "zapret-Init-Skript (%1 start/stop)", "zapret init 脚本（%1 start/stop）",
            "Script init zapret (%1 start/stop)", "zapret init स्क्रिप्ट (%1 start/stop)"),
        ROW("card.desc.systemd", "Unit-файл systemd: %1", "systemd unit file: %1",
            "Archivo de unidad systemd: %1", "systemd-Unit-Datei: %1", "systemd 单元文件：%1",
            "Fichier d'unité systemd : %1", "systemd यूनिट फ़ाइल: %1"),
        ROW("card.menu.copy_command", "Скопировать команду", "Copy command", "Copiar el comando",
            "Befehl kopieren", "复制命令", "Copier la commande", "कमांड कॉपी करें"),
        ROW("card.menu.open_folder", "Открыть папку со стратегией", "Open the strategy's folder",
            "Abrir la carpeta de la estrategia", "Ordner der Strategie öffnen", "打开策略所在文件夹",
            "Ouvrir le dossier de la stratégie", "रणनीति का फ़ोल्डर खोलें"),

        // ---- backend status + action gating ----
        ROW("status.stopped", "Остановлено", "Stopped", "Detenido", "Gestoppt", "已停止", "Arrêté",
            "रोका गया"),
        ROW("status.starting", "Запуск: %1", "Starting: %1", "Iniciando: %1", "Startet: %1",
            "正在启动：%1", "Démarrage : %1", "चलाया जा रहा है: %1"),
        ROW("status.running", "Работает: %1", "Running: %1", "En ejecución: %1", "Läuft: %1",
            "运行中：%1", "En cours : %1", "चल रहा है: %1"),
        ROW("status.stopping", "Остановка…", "Stopping…", "Deteniendo…", "Stoppen…", "正在停止…",
            "Arrêt…", "रोका जा रहा है…"),
        ROW("status.error", "Ошибка", "Error", "Error", "Fehler", "错误", "Erreur", "त्रुटि"),
        ROW("gate.starting", "Идёт запуск — дождитесь его завершения.",
            "A start is in progress — wait for it to finish.",
            "Se está iniciando — espere a que termine.",
            "Ein Start läuft — bitte den Abschluss abwarten.", "正在启动—请等待其完成。",
            "Un démarrage est en cours — attendez sa fin.",
            "शुरू हो रहा है — पूरा होने की प्रतीक्षा करें।"),
        ROW("gate.stopping", "Идёт остановка — дождитесь её завершения.",
            "A stop is in progress — wait for it to finish.",
            "Se está deteniendo — espere a que termine.",
            "Ein Stopp läuft — bitte den Abschluss abwarten.", "正在停止—请等待其完成。",
            "Un arrêt est en cours — attendez sa fin.", "रोका जा रहा है — पूरा होने की प्रतीक्षा करें।"),
        ROW("gate.running", "Стратегия уже запущена — сначала остановите её.",
            "A strategy is already running — stop it first.",
            "Ya hay una estrategia en ejecución — deténgala primero.",
            "Es läuft bereits eine Strategie — stoppen Sie sie zuerst.", "已有策略在运行—请先停止它。",
            "Une stratégie est déjà en cours — arrêtez-la d'abord.",
            "पहले से एक रणनीति चल रही है — पहले उसे रोकें।"),
        ROW("hint.stop", "Остановить запущенную стратегию (Ctrl+Shift+X)",
            "Stop the running strategy (Ctrl+Shift+X)",
            "Detener la estrategia en ejecución (Ctrl+Shift+X)",
            "Laufende Strategie stoppen (Strg+Umschalt+X)", "停止正在运行的策略（Ctrl+Shift+X）",
            "Arrêter la stratégie en cours (Ctrl+Maj+X)",
            "चल रही रणनीति रोकें (Ctrl+Shift+X)"),
        ROW("hint.start", "Запустить выбранную стратегию (Ctrl+Shift+S)",
            "Start the chosen strategy (Ctrl+Shift+S)",
            "Iniciar la estrategia elegida (Ctrl+Shift+S)",
            "Gewählte Strategie starten (Strg+Umschalt+S)", "启动所选策略（Ctrl+Shift+S）",
            "Démarrer la stratégie choisie (Ctrl+Maj+S)", "चुनी रणनीति चलाएँ (Ctrl+Shift+S)"),

        // ---- tray + window ----
        ROW("tray.show", "Показать", "Show", "Mostrar", "Anzeigen", "显示", "Afficher", "दिखाएँ"),
        ROW("tray.start_last", "Запустить последнюю", "Start the last one", "Iniciar la última",
            "Zuletzt gestartet", "启动上一个", "Démarrer la dernière", "पिछली रणनीति चलाएँ"),
        ROW("tray.stop", "Остановить", "Stop", "Detener", "Stoppen", "停止", "Arrêter", "रोकें"),
        ROW("tray.quit", "Выход", "Quit", "Salir", "Beenden", "退出", "Quitter", "बाहर निकलें"),
        ROW("tray.minimized",
            "Окно свёрнуто в трей — zapret продолжает работать.\nДвойной клик по иконке открывает окно.",
            "The window is hidden in the tray — zapret keeps running.\nDouble-click the icon to open it again.",
            "La ventana está en la bandeja — zapret sigue funcionando.\nHaga doble clic en el icono para abrirla.",
            "Das Fenster liegt im Tray — zapret läuft weiter.\nDoppelklick auf das Symbol öffnet es wieder.",
            "窗口已缩到托盘—zapret 继续运行。\n双击图标即可重新打开窗口。",
            "La fenêtre est dans la barre système — zapret continue de tourner.\nDouble-cliquez sur l'icône pour l'ouvrir.",
            "विंडो ट्रे में छिपी है — zapret चलता रहेगा।\nखोलने के लिए आइकन पर डबल-क्लिक करें।"),
        ROW("zapret.not_found", "zapret не найден", "zapret not found", "no se encontró zapret",
            "zapret nicht gefunden", "未找到 zapret", "zapret introuvable", "zapret नहीं मिला"),





        // ---- MainWindow log lines + transient messages ----
        ROW("log.startup", "Muffin %1 запущен (бэкенд: %2)", "Muffin %1 started (backend: %2)",
            "Muffin %1 iniciado (backend: %2)", "Muffin %1 gestartet (Backend: %2)",
            "Muffin %1 已启动（后端：%2）", "Muffin %1 démarré (backend : %2)",
            "Muffin %1 शुरू हुआ (बैकएंड: %2)"),
        // Where the window is being drawn and what is still being composited inside it. The report this
        // answers is a ghost of the window left on the desktop after a restore, which cannot be reproduced
        // from inside the application: the log has to say which layer it was drawn on, and whether any
        // effect is holding a pixmap of the content at all.
        ROW("log.graphics", "Графический слой: %1 · эффектов отрисовки в окне: %2",
            "Graphics platform: %1 · drawing effects attached in this window: %2",
            "Plataforma gráfica: %1 · efectos de dibujo activos en esta ventana: %2",
            "Grafikplattform: %1 · aktive Zeichen-Effekte in diesem Fenster: %2",
            "图形平台：%1 · 此窗口内启用的绘图效果：%2",
            "Plateforme graphique : %1 · effets de dessin actifs dans cette fenêtre : %2",
            "ग्राफ़िक प्लेटफ़ॉर्म: %1 · इस विंडो में सक्रिय ड्रॉइंग प्रभाव: %2"),
        ROW("log.found_in", "Найдено стратегий: %1 в %2", "%1 strategies found in %2",
            "%1 estrategias encontradas en %2", "%1 Strategien in %2 gefunden",
            "在 %2 中找到 %1 个策略", "%1 stratégies trouvées dans %2", "%2 में %1 रणनीतियाँ मिलीं"),
        ROW("log.no_strategies", "Стратегии не найдены — укажите путь к zapret в настройках.",
            "No strategies found — set the zapret path in settings.",
            "No hay estrategias — indique la ruta de zapret en los ajustes.",
            "Keine Strategien gefunden — Pfad zu zapret in den Einstellungen setzen.",
            "未找到策略—请在设置中指定 zapret 路径。",
            "Aucune stratégie trouvée — indiquez le chemin de zapret dans les paramètres.",
            "कोई रणनीति नहीं मिली — सेटिंग्स में zapret पथ दें।"),
        ROW("log.tray_no_env",
            "Трей недоступен в этой среде: закрытие окна остановит zapret и выйдет из приложения.",
            "This desktop has no notification area: closing the window stops zapret and quits the application.",
            "Esta sesión no tiene área de notificación: cerrar la ventana detendrá zapret y saldrá de la aplicación.",
            "In dieser Umgebung gibt es keinen Benachrichtigungsbereich: Schließen stoppt zapret und beendet die Anwendung.",
            "此环境没有通知区域：关闭窗口会停止 zapret 并退出程序。",
            "Cet environnement n'a pas de zone de notification : fermer la fenêtre arrête zapret et quitte l'application.",
            "इस वातावरण में सूचना क्षेत्र नहीं है: विंडो बंद करने पर zapret रुकेगा और ऐप बंद होगा।"),
        ROW("msg.tray_unavailable", "Трей недоступен — закрытие окна завершит работу",
            "No tray here — closing the window will quit",
            "No hay bandeja: cerrar la ventana cerrará la aplicación",
            "Kein Tray: Schließen beendet die Anwendung", "无托盘—关闭窗口将退出程序",
            "Pas de zone de notification : fermer la fenêtre quittera l'application",
            "ट्रे नहीं है — विंडो बंद करने पर ऐप बंद होगा"),
        ROW("log.tray_available", "Трей доступен — сворачивание при закрытии включено.",
            "The tray is available — hide-on-close is on.",
            "La bandeja está disponible: ocultar al cerrar está activo.",
            "Tray verfügbar — Minimieren beim Schließen ist aktiv.", "托盘可用—关闭时缩到托盘已开启。",
            "La zone de notification est disponible : masquer à la fermeture est actif.",
            "ट्रे उपलब्ध है — बंद करते समय छिपाना चालू है।"),
        ROW("log.rescanned", "Пересканировано: %1 стратегий в %2", "Rescanned: %1 strategies in %2",
            "Reescaneado: %1 estrategias en %2", "Erneut gescannt: %1 Strategien in %2",
            "重新扫描：%2 中 %1 个策略", "Rescan effectué : %1 stratégies dans %2",
            "फिर से स्कैन: %2 में %1 रणनीतियाँ"),
        ROW("log.no_dir", "(не найдено)", "(not found)", "(no encontrada)", "(nicht gefunden)",
            "（未找到）", "(introuvable)", "(नहीं मिला)"),
        ROW("log.empty_dir", "В этой папке нет стратегий — проверьте путь в настройках.",
            "This folder holds no strategies — check the path in settings.",
            "Esta carpeta no tiene estrategias — revise la ruta en los ajustes.",
            "Dieser Ordner enthält keine Strategien — Pfad in den Einstellungen prüfen.",
            "此文件夹中没有策略—请检查设置中的路径。",
            "Ce dossier ne contient aucune stratégie — vérifiez le chemin dans les paramètres.",
            "इस फ़ोल्डर में कोई रणनीति नहीं — सेटिंग्स में पथ जाँचें।"),
        ROW("log.installed", "Установлен zapret: %1", "zapret installed: %1",
            "zapret instalado en: %1", "zapret installiert: %1", "已安装 zapret：%1",
            "zapret installé : %1", "zapret इंस्टॉल किया गया: %1"),
        ROW("log.start_requested", "Запрошен запуск: %1", "Start requested: %1",
            "Inicio solicitado: %1", "Start angefragt: %1", "已请求启动：%1",
            "Démarrage demandé : %1", "शुरू करने का अनुरोध: %1"),
        ROW("log.strategy_stopped", "Стратегия остановлена", "Strategy stopped",
            "Estrategia detenida", "Strategie gestoppt", "策略已停止", "Stratégie arrêtée",
            "रणनीति रोक दी गई"),
        ROW("log.close_stops",
            "Трей недоступен: закрытие окна останавливает zapret и завершает приложение.",
            "No tray available: closing the window stops zapret and ends the application.",
            "Sin bandeja: cerrar la ventana detiene zapret y cierra la aplicación.",
            "Kein Tray verfügbar: Schließen stoppt zapret und beendet die Anwendung.",
            "没有可用托盘：关闭窗口会停止 zapret 并结束应用。",
            "Aucune zone de notification : fermer la fenêtre arrête zapret et termine l'application.",
            "ट्रे उपलब्ध नहीं: विंडो बंद करने पर zapret और ऐप दोनों बंद होंगे।"),
        ROW("msg.no_saved_strategy", "Нет сохранённой стратегии для запуска",
            "There is no saved strategy to start", "No hay una estrategia guardada que iniciar",
            "Keine gespeicherte Strategie zum Starten vorhanden", "没有可启动的已保存策略",
            "Aucune stratégie enregistrée à démarrer", "चलाने के लिए कोई सहेजी रणनीति नहीं"),
        ROW("msg.pick_profile", "Выберите профиль, затем нажмите кнопку",
            "Pick a profile, then press the button", "Elija un perfil y pulse el botón",
            "Bitte ein Profil wählen und dann die Taste drücken", "请先选择档案，再按按钮",
            "Choisissez un profil puis appuyez sur le bouton", "पहले प्रोफ़ाइल चुनें, फिर बटन दबाएँ"),
        ROW("msg.found_count", "Найдено стратегий: %1", "Strategies found: %1",
            "Estrategias encontradas: %1", "Gefundene Strategien: %1", "找到策略：%1",
            "Stratégies trouvées : %1", "मिली रणनीतियाँ: %1"),
        ROW("error.strategy_not_found", "Стратегия не найдена: %1", "Strategy not found: %1",
            "Estrategia no encontrada: %1", "Strategie nicht gefunden: %1", "未找到策略：%1",
            "Stratégie introuvable : %1", "रणनीति नहीं मिली: %1"),
        ROW("error.backend_refused", "Бэкенд отклонил запуск.", "The backend refused the start.",
            "El backend rechazó el inicio.", "Das Backend hat den Start abgelehnt.",
            "后端拒绝了本次启动。", "Le backend a refusé le démarrage.",
            "बैकएंड ने शुरू करने से इनकार किया।"),

        // ---- settings page ----
        // The three card headings. They are words rather than rules because the page is grouped by what a
        // setting costs: a folder on disk, a look, a cadence of probes. Naming the groups is what lets the
        // eye skip the ones that are already right, and a heading in the wrong language is the one thing a
        // card layout cannot hide.
        ROW("settings.group.general", "Основные", "General", "General", "Allgemein", "常规",
            "Général", "सामान्य"),
        ROW("settings.group.interface", "Интерфейс", "Interface", "Interfaz", "Oberfläche", "界面",
            "Interface", "इंटरफ़ेस"),
        ROW("settings.group.network", "Сеть", "Network", "Red", "Netzwerk", "网络", "Réseau",
            "नेटवर्क"),
        ROW("settings.title", "Настройки Muffin", "Muffin settings", "Ajustes de Muffin",
            "Muffin-Einstellungen", "Muffin 设置", "Paramètres de Muffin", "Muffin सेटिंग्स"),
        ROW("settings.path", "Путь к zapret", "zapret path", "Ruta de zapret", "Pfad zu zapret",
            "zapret 路径", "Chemin de zapret", "zapret पथ"),
        ROW("settings.browse", "Обзор…", "Browse…", "Examinar…", "Durchsuchen…", "浏览…",
            "Parcourir…", "ब्राउज़ करें…"),
        // The folder glyph inside the path field says "Browse…" as its name and needs a sentence as its
        // tooltip: an icon-only control must state what pressing it opens.
        ROW("settings.browse_tip", "Выбрать папку с деревом zapret",
            "Choose the folder that holds the zapret tree",
            "Elegir la carpeta que contiene el árbol de zapret",
            "Ordner mit dem zapret-Baum auswählen", "选择包含 zapret 目录树的文件夹",
            "Choisir le dossier contenant l'arborescence zapret",
            "zapret ट्री वाला फ़ोल्डर चुनें"),
        ROW("settings.autodetect", "Автопоиск", "Auto-detect", "Búsqueda automática",
            "Automatisch suchen", "自动搜索", "Recherche auto", "स्वतः खोज"),
        ROW("settings.autodetect_tip",
            "Найти дерево zapret в $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret",
            "Find a zapret tree in $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret",
            "Buscar un árbol de zapret en $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret",
            "zapret-Baum in $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret finden",
            "在 $ZAPRET_ROOT、~/Documents/zapret、/opt/zapret 中查找 zapret 目录树",
            "Chercher une arborescence zapret dans $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret",
            "$ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret में zapret ट्री खोजें"),
        ROW("settings.download", "Скачать…", "Download…", "Descargar…", "Herunterladen…",
            "下载…", "Télécharger…", "डाउनलोड करें…"),
        ROW("settings.download_tip",
            "Загрузить последнюю версию zapret с GitHub в папку приложения",
            "Fetch the latest zapret release from GitHub into the application folder",
            "Obtener la última versión de zapret desde GitHub y guardarla en la carpeta de la aplicación",
            "Die neueste zapret-Version von GitHub in den Anwendungsordner laden",
            "从 GitHub 获取最新 zapret 版本并存入应用文件夹",
            "Récupérer la dernière version de zapret depuis GitHub dans le dossier de l'application",
            "GitHub से नवीनतम zapret संस्करण ऐप फ़ोल्डर में लाएँ"),
        ROW("settings.start_last", "Запускать последнюю стратегию при старте",
            "Start the last strategy on launch", "Iniciar la última estrategia al arrancar",
            "Zuletzt verwendete Strategie beim Start ausführen", "启动时运行上次使用的策略",
            "Démarrer la dernière stratégie au lancement", "लॉन्च पर पिछली रणनीति चलाएँ"),
        ROW("settings.tray", "Сворачивать в трей при закрытии", "Hide to tray on close",
            "Ocultar en la bandeja al cerrar", "Beim Schließen in den Tray minimieren",
            "关闭窗口时缩到托盘", "Masquer dans la barre système à la fermeture",
            "बंद करते समय ट्रे में छिपाएँ"),
        // The single-window policy. Offered only where there is a window to close, so the label says
        // which program it means and the tooltip says what the other half of the pair is for.
        ROW("settings.hide_warp", "Закрывать окно Cloudflare WARP", "Close the Cloudflare WARP window",
            "Cerrar la ventana de Cloudflare WARP", "Cloudflare-WARP-Fenster schließen",
            "关闭 Cloudflare WARP 窗口", "Fermer la fenêtre Cloudflare WARP",
            "Cloudflare WARP विंडो बंद करें"),
        ROW("settings.hide_warp_tip",
            "Установщик WARP открывает своё окно, а трей-приложение запускается при входе в систему. "
            "Muffin закрывает их: туннелем управляет это окно.",
            "The WARP installer opens a window of its own and its tray app starts at sign-in. Muffin "
            "closes both: this window is what drives the tunnel.",
            "El instalador de WARP abre su propia ventana y su aplicación de bandeja se inicia al "
            "iniciar sesión. Muffin las cierra: esta ventana es la que controla el túnel.",
            "Der WARP-Installer öffnet ein eigenes Fenster, und seine Tray-App startet bei der "
            "Anmeldung. Muffin schließt beides: Dieses Fenster steuert den Tunnel.",
            "WARP 安装程序会打开自己的窗口，托盘应用也会在登录时启动。Muffin 会关闭它们：隧道由本窗口控制。",
            "L'installateur WARP ouvre sa propre fenêtre et son application de zone de notification "
            "démarre à la session. Muffin les ferme : c'est cette fenêtre qui pilote le tunnel.",
            "WARP इंस्टॉलर अपनी विंडो खोलता है और ट्रे ऐप साइन-इन पर चलता है। Muffin दोनों बंद करता है: "
            "टनल इसी विंडो से नियंत्रित होता है।"),
        // The motion switch. Named for what it turns on rather than for the machines it exists for:
        // a user who never heard of "weak PCs" still has to recognise the word for the movement.
        ROW("settings.animations", "Включить анимации", "Enable animations",
            "Activar animaciones", "Animationen aktivieren", "启用动画",
            "Activer les animations", "एनिमेशन सक्षम करें"),
        ROW("settings.animations_tip",
            "Плавная смена страниц, появление карточек и пульсация кнопки. Выключите на слабом ПК.",
            "Smooth page changes, card entrances and the power button's pulse. Turn off on a slow PC.",
            "Cambios de página, entrada de tarjetas y pulso del botón suaves. "
            "Desactívelo en un equipo lento.",
            "Weiche Seitenwechsel, Karten-Einblendung und Puls der Power-Taste. "
            "Auf schwachen PCs deaktivieren.",
            "页面切换、卡片出现与电源按钮脉冲更平滑。在低配电脑上可关闭。",
            "Changements de page, apparition des cartes et pulsation du bouton fluides. "
            "Désactivez sur un PC peu puissant.",
            "पेज बदलने, कार्ड दिखने और पावर बटन की धड़कन के एनिमेशन। कमजोर पीसी पर इन्हें बंद करें।"),
        ROW("settings.tray_note",
            "В этой среде нет области уведомлений: закрытие окна остановит zapret и выйдет из приложения.\n"
            "Настройка сохранена — если панель запустится позже, трей появится сам.",
            "This desktop has no notification area: closing the window stops zapret and quits the application.\n"
            "The setting is saved — if the panel starts later, the tray appears on its own.",
            "Esta sesión no tiene área de notificación: cerrar la ventana detendrá zapret y saldrá.\n"
            "El ajuste queda guardado: si el panel arranca más tarde, la bandeja aparecerá sola.",
            "In dieser Umgebung gibt es keinen Benachrichtigungsbereich: Schließen stoppt zapret und beendet die Anwendung.\n"
            "Die Einstellung ist gespeichert — startet die Leiste später, erscheint der Tray von allein.",
            "此环境没有通知区域：关闭窗口会停止 zapret 并退出程序。\n设置已保存——若面板稍后启动，托盘会自动出现。",
            "Cet environnement n'a pas de zone de notification : fermer la fenêtre arrête zapret et quitte l'application.\n"
            "Le réglage est enregistré — si le panneau démarre plus tard, la zone de notification apparaîtra.",
            "इस वातावरण में सूचना क्षेत्र नहीं है: विंडो बंद करने पर zapret रुकेगा और ऐप बंद होगा।\n"
            "सेटिंग सहेजी गई — पैनल बाद में चालू हुआ तो ट्रे खुद दिख जाएगा।"),
        ROW("settings.theme", "Тема", "Theme", "Tema", "Thema", "主题", "Thème", "थीम"),
        // The row's label is translated; the entries under it are not, on purpose — a language is always
        // named in its own script so the person who cannot read the current UI can still find their own.
        ROW("settings.language", "Язык", "Language", "Idioma", "Sprache", "语言", "Langue",
            "भाषा"),
        ROW("settings.language_tip", "Сменить язык интерфейса сразу после сохранения",
            "Switch the interface language as soon as you save",
            "Cambiar el idioma de la interfaz al guardar",
            "Sprache der Oberfläche nach dem Speichern wechseln",
            "保存后立即切换界面语言", "Changer la langue de l'interface après l'enregistrement",
            "सहेजते ही इंटरफ़ेस की भाषा बदलें"),
        ROW("theme.dark", "Тёмная", "Dark", "Oscura", "Dunkel", "深色", "Sombre", "डार्क"),
        ROW("theme.light", "Светлая", "Light", "Clara", "Hell", "浅色", "Clair", "लाइट"),
        ROW("theme.system", "Системная", "System", "Del sistema", "System", "跟随系统",
            "Système", "सिस्टम"),
        ROW("settings.ping", "Интервал пинга", "Ping interval", "Intervalo de ping",
            "Ping-Intervall", "Ping 间隔", "Intervalle de ping", "पिंग अंतराल"),
        ROW("settings.ping_suffix", " сек", " s", " s", " s", " 秒", " s", " सेकंड"),
        // The watchdog's cadence. A field of the same shape as the ping interval on purpose: both are
        // one short number with a unit, and the page keeps that shape for both.
        ROW("settings.watchdog", "Интервал проверки обхода", "Bypass re-check interval",
            "Intervalo de verificación de evasión", "Bypass-Nachprüfintervall",
            "绕过复检间隔", "Intervalle de re-vérification du contournement",
            "बाईपास दोबारा-जाँच अंतराल"),
        ROW("settings.watchdog_suffix", " мин", " min", " min", " Min", " 分钟", " min",
            " मिनट"),
        ROW("settings.watchdog_tip",
            "Как часто проверять, что запущенный профиль всё ещё обходит блокировки",
            "How often to check that the running profile still bypasses blocks",
            "Con qué frecuencia comprobar si el perfil activo sigue eludiendo bloqueos",
            "Wie oft geprüft wird, dass das laufende Profil Sperren weiterhin umgeht",
            "多久检查一次运行中的配置文件仍能绕过屏蔽",
            "Fréquence à laquelle vérifier que le profil lancé contourne toujours les blocages",
            "कितनी बार जाँचें कि चलती प्रोफ़ाइल अब भी अवरोध तोड़ती है"),
        ROW("settings.save", "Сохранить", "Save", "Guardar", "Speichern", "保存", "Enregistrer",
            "सहेजें"),
        // The acknowledgement the Save button puts beside itself for two seconds. The tick is the whole
        // sentence: a page has no frame to close, so "it worked" has to be said where the press was.
        ROW("settings.saved", "Сохранено ✓", "Saved ✓", "Guardado ✓", "Gespeichert ✓",
            "已保存 ✓", "Enregistré ✓", "सहेजा गया ✓"),
        ROW("path.none", "Путь не выбран.", "No path selected.", "Sin ruta seleccionada.",
            "Kein Pfad gewählt.", "未选择路径。", "Aucun chemin sélectionné.",
            "कोई पथ नहीं चुना गया।"),
        ROW("path.found", "Найдено: %1, стратегий: %2", "Found: %1, strategies: %2",
            "Encontrado: %1, estrategias: %2", "Gefunden: %1, Strategien: %2",
            "找到：%1，策略数：%2", "Trouvé : %1, stratégies : %2", "मिला: %1, रणनीतियाँ: %2"),
        ROW("path.base", "Базовая папка: найдены %1", "Base folder: found %1",
            "Carpeta base: encontrados %1", "Basisordner: %1 gefunden", "基础文件夹：找到 %1",
            "Dossier de base : %1 trouvés", "बेस फ़ोल्डर: %1 मिले"),
        ROW("path.not_zapret",
            "Это не дерево zapret: нет ни bin/winws.exe + *.bat, ни init.d/nfqws.",
            "This is not a zapret tree: neither bin/winws.exe + *.bat nor init.d/nfqws.",
            "No es un árbol de zapret: ni bin/winws.exe + *.bat ni init.d/nfqws.",
            "Das ist kein zapret-Baum: weder bin/winws.exe + *.bat noch init.d/nfqws.",
            "这不是 zapret 目录树：既没有 bin/winws.exe + *.bat，也没有 init.d/nfqws。",
            "Ce n'est pas une arborescence zapret : ni bin/winws.exe + *.bat, ni init.d/nfqws.",
            "यह zapret ट्री नहीं है: न bin/winws.exe + *.bat, न init.d/nfqws।"),
        ROW("path.not_found_manual", "Zapret не найден — укажите путь вручную.",
            "Zapret not found — choose the path manually.",
            "Zapret no encontrado — indique la ruta manualmente.",
            "Zapret nicht gefunden — Pfad manuell wählen.", "未找到 Zapret—请手动指定路径。",
            "Zapret introuvable — indiquez le chemin manuellement.",
            "Zapret नहीं मिला — पथ स्वयं चुनें।"),
        ROW("settings.folder_title", "Папка с zapret", "Folder containing zapret",
            "Carpeta con zapret", "Ordner mit zapret", "包含 zapret 的文件夹",
            "Dossier contenant zapret", "zapret वाला फ़ोल्डर"),

        // ---- download dialog ----
        ROW("dl.info", "Сведения о релизе появятся после проверки GitHub.",
            "Release details appear once GitHub has been checked.",
            "Los datos del lanzamiento aparecerán tras consultar GitHub.",
            "Release-Details erscheinen nach der Abfrage bei GitHub.",
            "检查 GitHub 后会显示版本信息。",
            "Les informations de la version s'afficheront après vérification sur GitHub.",
            "GitHub जाँच के बाद रिलीज़ जानकारी दिखेगी।"),
        ROW("dl.waiting", "Ожидание.", "Waiting.", "En espera.", "Warten.", "等待中。",
            "En attente.", "प्रतीक्षा।"),
        ROW("dl.checking", "Проверяю последнюю версию на GitHub…",
            "Checking GitHub for the latest version…", "Consultando la última versión en GitHub…",
            "Neueste Version auf GitHub wird geprüft…", "正在 GitHub 上检查最新版本…",
            "Vérification de la dernière version sur GitHub…",
            "GitHub पर नवीनतम संस्करण जाँचा जा रहा है…"),
        ROW("dl.ready", "Готово к скачиванию.", "Ready to download.", "Listo para descargar.",
            "Bereit zum Herunterladen.", "可以下载了。", "Prêt à télécharger.",
            "डाउनलोड के लिए तैयार।"),
        ROW("dl.downloading", "Скачиваю…", "Downloading…", "Descargando…",
            "Wird heruntergeladen…", "正在下载…", "Téléchargement…", "डाउनलोड हो रहा है…"),
        ROW("dl.downloading_short", "Скачано %1…", "Downloaded %1…", "Descargados %1…",
            "%1 geladen…", "已下载 %1…", "Téléchargé %1…", "%1 डाउनलोड हुआ…"),
        ROW("dl.downloading_of", "Скачано %1 из %2…", "Downloaded %1 of %2…",
            "Descargados %1 de %2…", "%1 von %2 geladen…", "已下载 %2 中的 %1…",
            "Téléchargé %1 sur %2…", "%2 में से %1 डाउनलोड हुआ…"),
        ROW("dl.sha_ok", "SHA-256 совпал. Распаковываю…", "SHA-256 matched. Extracting…",
            "SHA-256 coincide. Extrayendo…", "SHA-256 passt. Entpacken…", "SHA-256 校验通过，正在解压…",
            "SHA-256 vérifié. Extraction…", "SHA-256 मेल खाया, निकाला जा रहा है…"),
        ROW("dl.installed_in", "Установлено в %1", "Installed in %1", "Instalado en %1",
            "Installiert in %1", "已安装到 %1", "Installé dans %1", "%1 में इंस्टॉल किया गया"),
        ROW("dl.cancelled", "Отменено.", "Cancelled.", "Cancelado.", "Abgebrochen.", "已取消。",
            "Annulé.", "रद्द किया गया।"),
        ROW("dl.cancelling", "Отменяю…", "Cancelling…", "Cancelando…", "Abbruch läuft…",
            "正在取消…", "Annulation…", "रद्द किया जा रहा है…"),
        ROW("dl.download_button", "Скачать", "Download", "Descargar", "Herunterladen", "下载",
            "Télécharger", "डाउनलोड करें"),
        ROW("dl.release", "Релиз %1\nФайл: %2, %3\nУстановить в: %4",
            "Release %1\nFile: %2, %3\nInstall into: %4",
            "Lanzamiento %1\nArchivo: %2, %3\nInstalar en: %4",
            "Release %1\nDatei: %2, %3\nInstallieren nach: %4", "版本 %1\n文件：%2，%3\n安装到：%4",
            "Version %1\nFichier : %2, %3\nInstaller dans : %4", "रिलीज़ %1\nफ़ाइल: %2, %3\nयहाँ इंस्टॉल करें: %4"),
        ROW("dl.unnamed", "(без имени)", "(unnamed)", "(sin nombre)", "(namenlos)", "（未命名）",
            "(sans nom)", "(बिना नाम)"),
        ROW("dl.no_sha", "\nSHA-256 для этого файла не опубликован — сверка невозможна.",
            "\nNo SHA-256 is published for this file — it cannot be verified.",
            "\nNo se ha publicado SHA-256 para este archivo: no se puede verificar.",
            "\nFür diese Datei ist kein SHA-256 veröffentlicht — Vergleich nicht möglich.",
            "\n此文件未公布 SHA-256，无法核对。",
            "\nAucun SHA-256 publié pour ce fichier — vérification impossible.",
            "\nइस फ़ाइल का SHA-256 प्रकाशित नहीं है — जाँच संभव नहीं।"),
        ROW("dl.sha_will_check", "\nSHA-256 будет сверен с опубликованным.",
            "\nThe SHA-256 will be checked against the published one.",
            "\nEl SHA-256 se comparará con el publicado.",
            "\nDer SHA-256 wird mit dem veröffentlichten abgeglichen.", "\nSHA-256 将与已公布值核对。",
            "\nLe SHA-256 sera comparé à celui publié.", "\nSHA-256 की जाँच प्रकाशित मान से होगी।"),
        ROW("dl.version_failed", "Не удалось узнать последнюю версию:\n\n%1",
            "Could not determine the latest version:\n\n%1",
            "No se pudo obtener la última versión:\n\n%1",
            "Neueste Version konnte nicht ermittelt werden:\n\n%1", "无法获取最新版本：\n\n%1",
            "Impossible d'obtenir la dernière version :\n\n%1", "नवीनतम संस्करण ज्ञात नहीं हुआ:\n\n%1"),
        ROW("dl.failed", "Скачивание не удалось:\n\n%1", "The download failed:\n\n%1",
            "La descarga falló:\n\n%1", "Herunterladen fehlgeschlagen:\n\n%1",
            "下载失败：\n\n%1", "Le téléchargement a échoué :\n\n%1", "डाउनलोड विफल:\n\n%1"),
        ROW("dl.installed_msg", "zapret установлен в:\n%1", "zapret was installed in:\n%1",
            "zapret se instaló en:\n%1", "zapret wurde installiert in:\n%1",
            "zapret 已安装到：\n%1", "zapret a été installé dans :\n%1",
            "zapret यहाँ इंस्टॉल हुआ:\n%1"),
        ROW("dl.root_note",
            "\n\nСистемная копия принадлежит root, поэтому запуск стратегий не будет требовать пароль.",
            "\n\nThe system copy belongs to root, so starting a strategy will not ask for a password.",
            "\n\nLa copia del sistema pertenece a root, así que iniciar estrategias no pedirá contraseña.",
            "\n\nDie Systemkopie gehört root, daher fragt der Start einer Strategie kein Passwort mehr ab.",
            "\n\n系统副本属于 root，因此启动策略不会再询问密码。",
            "\n\nLa copie système appartient à root : lancer une stratégie ne demandera plus de mot de passe.",
            "\n\nसिस्टम कॉपी root की है, इसलिए रणनीति चलाते समय पासवर्ड नहीं माँगा जाएगा।"),
        ROW("dl.replace_question",
            "В %1 уже есть файлы (%2).\n\n"
            "Скачивание установит туда новую копию zapret; старые файлы будут удалены после того, как "
            "проверка подтвердит новое дерево.\n\nПродолжить?",
            "%1 already holds files (%2).\n\n"
            "The download will put a new copy of zapret there; the old files are removed once the new "
            "tree has been verified.\n\nContinue?",
            "%1 ya contiene archivos (%2).\n\n"
            "La descarga instalará allí una copia nueva de zapret; los archivos viejos se borrarán cuando "
            "la verificación confirme el árbol nuevo.\n\n¿Continuar?",
            "In %1 liegen bereits Dateien (%2).\n\n"
            "Der Download installiert dort eine neue Kopie von zapret; die alten Dateien werden entfernt, "
            "sobald die Prüfung den neuen Baum bestätigt hat.\n\nFortfahren?",
            "%1 中已有文件（%2）。\n\n下载会在此安装新的 zapret 副本；校验确认新目录树后，旧文件将被删除。\n\n是否继续？",
            "%1 contient déjà des fichiers (%2).\n\n"
            "Le téléchargement y installera une nouvelle copie de zapret ; les anciens fichiers seront "
            "supprimés une fois le nouvel arbre vérifié.\n\nContinuer ?",
            "%1 में पहले से फ़ाइलें हैं (%2)。\n\n"
            "डाउनलोड वहाँ zapret की नई कॉपी इंस्टॉल करेगा; नई ट्री की जाँच होने पर पुरानी फ़ाइलें हटा दी "
            "जाएँगी।\n\nजारी रखें?"),
        ROW("dl.no_data_dir",
            "Эта система не даёт доступный каталог данных приложения, куда можно установить zapret.",
            "This system provides no accessible application data directory to install zapret into.",
            "Este sistema no ofrece un directorio de datos de la aplicación accesible donde instalar zapret.",
            "Dieses System stellt keinen zugänglichen Anwendungsdaten-Ordner bereit, in den zapret installiert werden könnte.",
            "此系统没有可用于安装 zapret 的应用数据目录。",
            "Ce système ne fournit aucun dossier de données d'application accessible où installer zapret.",
            "इस सिस्टम में zapret इंस्टॉल करने के लिए कोई सुलभ ऐप डेटा डायरेक्टरी नहीं है।"),

        // ---- sizes, ping chips ----
        ROW("size.unknown", "неизвестно", "unknown", "desconocido", "unbekannt", "未知",
            "inconnu", "अज्ञात"),
        ROW("size.bytes", "%1 Б", "%1 B", "%1 B", "%1 B", "%1 B", "%1 o", "%1 B"),
        ROW("size.kib", "%1 КиБ", "%1 KiB", "%1 KiB", "%1 KiB", "%1 KiB", "%1 Kio", "%1 KiB"),
        ROW("size.mib", "%1 МиБ", "%1 MiB", "%1 MiB", "%1 MiB", "%1 MiB", "%1 Mio", "%1 MiB"),
        ROW("size.gib", "%1 ГиБ", "%1 GiB", "%1 GiB", "%1 GiB", "%1 GiB", "%1 Gio", "%1 GiB"),
        ROW("ping.ms", "%1 мс", "%1 ms", "%1 ms", "%1 ms", "%1 ms", "%1 ms", "%1 ms"),
        ROW("ping.timeout", "таймаут", "timeout", "tiempo de espera agotado", "Zeitüberschreitung",
            "超时", "expiration du délai", "समय-आउट"),

    };
    return rows;
}

// lang -> (key -> text), built once from the rows above.
const QHash<QString, QHash<QString, QString>> &tables()
{
    static const QHash<QString, QHash<QString, QString>> built = [] {
        QHash<QString, QHash<QString, QString>> byLang;
        for (const QString &code : orderedCodes())
            byLang.insert(code, QHash<QString, QString>());
        for (const Row &r : catalog()) {
            for (int i = 0; i < kLangCount; ++i)
                byLang[orderedCodes().at(i)].insert(r.key, r.text[i]);
        }
        return byLang;
    }();
    return built;
}

} // namespace

LanguageManager &LanguageManager::instance()
{
    static LanguageManager manager;
    return manager;
}

LanguageManager::LanguageManager(QObject *parent)
    : QObject(parent)
{
}

QAction *i18nText(QAction *action, const QString &key)
{
    action->setProperty("i18nKey", key);
    action->setText(LanguageManager::tr(key));
    return action;
}

int retranslateTree(QObject *root)
{
    int rewritten = 0;
    if (!root)
        return 0;

    const auto widgets = root->findChildren<QWidget *>();
    for (QWidget *w : widgets) {
        const QString key = w->property("i18nKey").toString();
        if (!key.isEmpty()) {
            if (auto *button = qobject_cast<QAbstractButton *>(w)) {
                button->setText(LanguageManager::tr(key));
                ++rewritten;
            } else if (auto *label = qobject_cast<QLabel *>(w)) {
                label->setText(LanguageManager::tr(key));
                ++rewritten;
            }
        }
        const QString tip = w->property("i18nTip").toString();
        if (!tip.isEmpty()) {
            w->setToolTip(LanguageManager::tr(tip));
            ++rewritten;
        }
        const QString acc = w->property("i18nAcc").toString();
        if (!acc.isEmpty()) {
            w->setAccessibleName(LanguageManager::tr(acc));
            ++rewritten;
        }

        // A combo's entries carry their data (a level int, a theme name) independently of their text, so
        // the labels can be rewritten in place: indices and the current selection survive.
        if (auto *combo = qobject_cast<QComboBox *>(w)) {
            const QStringList keys = w->property("i18nItems").toStringList();
            for (int i = 0; i < keys.size() && i < combo->count(); ++i) {
                combo->setItemText(i, LanguageManager::tr(keys.at(i)));
                ++rewritten;
            }
        }
        if (auto *spin = qobject_cast<QSpinBox *>(w)) {
            const QString suffixKey = w->property("i18nSuffix").toString();
            if (!suffixKey.isEmpty()) {
                spin->setSuffix(LanguageManager::tr(suffixKey));
                ++rewritten;
            }
        }
        if (auto *edit = qobject_cast<QLineEdit *>(w)) {
            const QString ph = w->property("i18nPlaceholder").toString();
            if (!ph.isEmpty()) {
                edit->setPlaceholderText(LanguageManager::tr(ph));
                ++rewritten;
            }
        }
    }

    const auto actions = root->findChildren<QAction *>();
    for (QAction *action : actions) {
        const QString key = action->property("i18nKey").toString();
        if (!key.isEmpty()) {
            action->setText(LanguageManager::tr(key));
            ++rewritten;
        }
        // An action's tooltip is read as often as its text — a glyph inside a line edit is an action, and
        // it is icon-only, so the tooltip is the only words it ever shows. Leaving this out would keep
        // that sentence in the language the page was built in after every other label had moved.
        const QString tip = action->property("i18nTip").toString();
        if (!tip.isEmpty()) {
            action->setToolTip(LanguageManager::tr(tip));
            ++rewritten;
        }
        const QString acc = action->property("i18nAcc").toString();
        if (!acc.isEmpty()) {
            action->setProperty("accessibleName", LanguageManager::tr(acc));
            ++rewritten;
        }
    }
    return rewritten;
}

const QStringList &LanguageManager::codes()
{
    return orderedCodes();
}

bool LanguageManager::isSupported(const QString &code)
{
    return orderedCodes().contains(code);
}

QString LanguageManager::nativeName(const QString &code)
{
    return nativeNames().value(code, code);
}

QIcon LanguageManager::flagIcon(const QString &code)
{
    // Flags are colour artwork, not currentColor glyphs: they are loaded straight from the resource so
    // both themes get the same recognisable stripes. Theme::icon() would flatten them to one tint.
    return QIcon(QStringLiteral(":/icons/flags/%1.svg").arg(code));
}

QString LanguageManager::currentCode() const
{
    return m_code;
}

void LanguageManager::setCode(const QString &code)
{
    if (!isSupported(code)) {
        qWarning("LanguageManager: unknown language '%s' — keeping '%s'",
                 qUtf8Printable(code), qUtf8Printable(m_code));
        return;
    }
    if (code == m_code)
        return;
    m_code = code;
    emit languageChanged(m_code);
}

QString LanguageManager::translate(const QString &language, const QString &key)
{
    const QHash<QString, QString> &table = tables().value(language);
    const QString found = table.value(key);
    if (!found.isEmpty())
        return found;
    // English is the pivot language of the catalog: a row added for one language only still says
    // something readable everywhere else, and uitest is what catches that it should not stay that way.
    return tables().value(QStringLiteral("en")).value(key, key);
}

QString LanguageManager::tr(const QString &key)
{
    return translate(instance().currentCode(), key);
}

QString LanguageManager::tr(const QString &key, const QString &arg1)
{
    return translate(instance().currentCode(), key).replace(QStringLiteral("%1"), arg1);
}

QStringList LanguageManager::keys()
{
    QStringList out;
    out.reserve(catalog().size());
    for (const Row &r : catalog())
        out << r.key;
    out.sort();
    out.removeDuplicates();
    return out;
}

} // namespace zapret
