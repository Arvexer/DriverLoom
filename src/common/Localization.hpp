#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <string_view>

namespace hdm {

enum class Lang : int {
    Ru = 0,
    Uk = 1,
    En = 2,
};

inline std::atomic<int> g_currentLang{static_cast<int>(Lang::Ru)};

[[nodiscard]] inline Lang GetLang() noexcept {
    const int v = g_currentLang.load(std::memory_order_relaxed);
    if (v == static_cast<int>(Lang::Uk)) {
        return Lang::Uk;
    }
    if (v == static_cast<int>(Lang::En)) {
        return Lang::En;
    }
    return Lang::Ru;
}

inline void SetLang(Lang lang) noexcept {
    g_currentLang.store(static_cast<int>(lang), std::memory_order_relaxed);
}

inline void DetectSystemLanguage() noexcept {
    const LANGID langId = GetUserDefaultUILanguage();
    const WORD primary = PRIMARYLANGID(langId);
    if (primary == LANG_UKRAINIAN) {
        SetLang(Lang::Uk);
    } else if (primary == LANG_RUSSIAN) {
        SetLang(Lang::Ru);
    } else {
        SetLang(Lang::En);
    }
}

[[nodiscard]] inline const char* Tr(const char* ru, const char* uk, const char* en) noexcept {
    switch (GetLang()) {
    case Lang::Uk:
        return uk;
    case Lang::En:
        return en;
    case Lang::Ru:
    default:
        return ru;
    }
}

namespace detail {

struct DynRule {
    std::string_view ru;
    std::string_view uk;
    std::string_view en;
};

inline void ReplaceAllInPlace(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty() || from == to) {
        return;
    }
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

} // namespace detail

// Translates dynamic backend status badges, categories, phases, and tooltips on the fly
// without requiring background scanners to re-run when switching language.
[[nodiscard]] inline std::string TrDyn(const std::string& text) {
    const Lang lang = GetLang();
    if (lang == Lang::Ru || text.empty()) {
        return text;
    }

    // Exact matches first (fast path for table cells and badges).
    static constexpr detail::DynRule kExactRules[] = {
        {"Успешно", "Успішно", "Success"},
        {"Работает", "Працює", "Running / OK"},
        {"Без драйвера", "Без драйвера", "No driver"},
        {"Требуется перезагрузка", "Потрібне перезавантаження", "Reboot required"},
        {"Неизвестное устройство", "Невідомий пристрій", "Unknown device"},
        {"Базовый монитор (monitor.inf)", "Базовий монітор (monitor.inf)", "Generic monitor (monitor.inf)"},
        {"Не настроено (Код 1)", "Не налаштовано (Код 1)", "Not configured (Code 1)"},
        {"Нет драйвера (Код 28)", "Немає драйвера (Код 28)", "No driver (Code 28)"},
        {"Отключено (Код 22)", "Вимкнено (Код 22)", "Disabled (Code 22)"},
        {"Требуется перезагрузка (Код 14)", "Потрібне перезавантаження (Код 14)", "Reboot required (Code 14)"},
        {"Требуется переустановка (Код 18)", "Потрібна переінсталяція (Код 18)", "Reinstall required (Code 18)"},
        {"Запуск невозможен (Код 10)", "Запуск неможливий (Код 10)", "Cannot start (Code 10)"},
        {"Устройство отсутствует (Код 24)", "Пристрій відсутній (Код 24)", "Device not present (Code 24)"},
        {"Неподписанный драйвер (Код 52)", "Непідписаний драйвер (Код 52)", "Unsigned driver (Code 52)"},

        // Diagnostics & storage roles
        {"Видеокарта", "Відеокарта", "Graphics Card"},
        {"Wi-Fi адаптер", "Wi-Fi адаптер", "Wi-Fi Adapter"},
        {"Сеть (LAN)", "Мережа (LAN)", "Network (LAN)"},
        {"Аудио", "Аудіо", "Audio"},
        {"Bluetooth", "Bluetooth", "Bluetooth"},
        {"NVMe контроллер", "NVMe контролер", "NVMe Controller"},
        {"Контроллер накопителей", "Контролер накопичувачів", "Storage Controller"},
        {"SATA / AHCI контроллер", "SATA / AHCI контролер", "SATA / AHCI Controller"},
        {"SATA / AHCI", "SATA / AHCI", "SATA / AHCI"},
        {"Чипсет / плата", "Чипсет / плата", "Chipset / Board"},
        {"Контроллер платы", "Контролер плати", "Board Controller"},
        {"Накопитель (NVMe)", "Накопичувач (NVMe)", "Drive (NVMe)"},
        {"Накопитель (SATA / Диск)", "Накопичувач (SATA / Диск)", "Drive (SATA / Disk)"},
        {"Актуален", "Актуальний", "Up to date"},
        {"Работает (TRIM активен)", "Працює (TRIM активний)", "Healthy (TRIM active)"},
        {"Работает (TRIM отключён)", "Працює (TRIM вимкнено)", "Healthy (TRIM disabled)"},

        // Services & restore points
        {"Остановлена", "Зупинена", "Stopped"},
        {"Запускается", "Запускається", "Starting"},
        {"Останавливается", "Зупиняється", "Stopping"},
        {"Приостановлена", "Призупинена", "Paused"},
        {"Неизвестно", "Невідомо", "Unknown"},
        {"Недоступна", "Недоступна", "Unavailable"},
        {"Готова (по требованию)", "Готова (на вимогу)", "Ready (On demand)"},
        {"Авто", "Авто", "Automatic"},
        {"Вручную", "Вручну", "Manual"},
        {"Отключена", "Вимкнена", "Disabled"},
        {"Системная", "Системна", "System"},
        {"Плановая точка", "Планова точка", "Scheduled point"},
        {"Системная точка", "Системна точка", "System point"},

        // Windows Power Schemes
        {"Сбалансированная", "Збалансована", "Balanced"},
        {"Высокая производительность", "Висока продуктивність", "High performance"},
        {"Экономия энергии", "Економія енергії", "Power saver"},
        {"Максимальная производительность", "Максимальна продуктивність", "Ultimate Performance"},

        // Windows Update statuses
        {"Доступен", "Доступне", "Available"},
        {"Загружен (готов к установке)", "Завантажено (готове до встановлення)", "Downloaded (Ready)"},
        {"Скрыто (заблокировано)", "Приховано (заблоковано)", "Hidden (Blocked)"},
        {"Отменено", "Скасовано", "Cancelled"},
        {"Пропущено", "Пропущено", "Skipped"},
        {"Тайм-аут установки", "Тайм-аут встановлення", "Install timeout"},
        {"Ошибка установки", "Помилка встановлення", "Install failed"},
        {"Установлено (нужна перезагрузка)", "Встановлено (потрібне перезавантаження)", "Installed (Reboot required)"},
        {"Установлено", "Встановлено", "Installed"},
        {"Установлен", "Встановлений", "Installed"},

        // Component statuses & version labels
        {"Отсутствует", "Відсутній", "Missing"},
        {"Не установлен", "Не встановлено", "Not installed"},
        {"Не найден", "Не знайдено", "Not found"},
        {"Недоступно (aka.ms)", "Недоступно (aka.ms)", "Unavailable (aka.ms)"},
        {"Недоступно", "Недоступно", "Unavailable"},
        {"ПО не установлено", "ПЗ не встановлено", "Software not installed"},
        {"Драйвер не установлен", "Драйвер не встановлено", "Driver not installed"},
        {"Базовый драйвер (monitor.inf)", "Базовий драйвер (monitor.inf)", "Generic driver (monitor.inf)"},
        {"Рекомендуется для игр", "Рекомендується для ігор", "Recommended for games"},
        {"Рекомендуется пакет чипсета AMD", "Рекомендується пакет чипсета AMD", "AMD Chipset package recommended"},
        {"Официальный помощник Intel", "Офіційний помічник Intel", "Official Intel Assistant"},
        {"Официальный портал AMD", "Офіційний портал AMD", "Official AMD Portal"},
        {"Поиск...", "Пошук...", "Checking..."},
        {"По запросу", "За запитом", "On demand"},
        {"Портал поддержки", "Портал підтримки", "Support Portal"},
        {"Портал AMD", "Портал AMD", "AMD Portal"},
        {"Онлайн-установщик", "Онлайн-інсталятор", "Web Installer"},
        {"Онлайн", "Онлайн", "Online"},
        {"Сайт производителя", "Сайт виробника", "Vendor Website"},
        {"Сайт LunarG", "Сайт LunarG", "LunarG Website"},
        {"Сайт Razer", "Сайт Razer", "Razer Website"},
        {"Каталог MS", "Каталог MS", "MS Catalog"},
        {"Synapse 4 (актуальный)", "Synapse 4 (актуальний)", "Synapse 4 (Latest)"},
        {"Synapse 4 (онлайн)", "Synapse 4 (онлайн)", "Synapse 4 (Online)"},
        {"Microsoft Store / сайт", "Microsoft Store / сайт", "Microsoft Store / Website"},
        {"Веб / Сайт Wooting", "Веб / Сайт Wooting", "Web / Wooting Website"},
        {"Веб-конфигуратор", "Веб-конфігуратор", "Web Configurator"},
        {"Сайт Glorious", "Сайт Glorious", "Glorious Website"},
        {"Сайт Pulsar", "Сайт Pulsar", "Pulsar Website"},
        {"Сайт Lamzu", "Сайт Lamzu", "Lamzu Website"},
        {"Сайт Endgame Gear", "Сайт Endgame Gear", "Endgame Gear Website"},
        {"Сайт Creative", "Сайт Creative", "Creative Website"},
        {"Сайт Bloody", "Сайт Bloody", "Bloody Website"},
        {"Сайт Turtle Beach", "Сайт Turtle Beach", "Turtle Beach Website"},
        {"Частично", "Частково", "Partial"},
        {"Контроллеры Intel", "Контролери Intel", "Intel Controllers"},
        {"Обнаружено в системе", "Виявлено в системі", "Detected on system"},
        {"Чипсет AMD (PCI\\VEN_1022)", "Чипсет AMD (PCI\\VEN_1022)", "AMD Chipset (PCI\\VEN_1022)"},
        {"Игры и системные библиотеки Windows", "Ігри та системні бібліотеки Windows", "Games & Windows System Libraries"},

        // Component titles & purpose descriptions
        {"Razer Synapse (Официальное ПО)",
         "Razer Synapse (Офіційне ПЗ)",
         "Razer Synapse (Official Software)"},
        {"Wootility (клавиатуры Wooting)",
         "Wootility (клавіатури Wooting)",
         "Wootility (Wooting Keyboards)"},
        {"Keychron Launcher (веб-конфигуратор)",
         "Keychron Launcher (веб-конфігуратор)",
         "Keychron Launcher (Web Configurator)"},
        {"AMD Chipset Drivers (Чипсет / PCI / PSP / SMBus)",
         "AMD Chipset Drivers (Чипсет / PCI / PSP / SMBus)",
         "AMD Chipset Drivers (Chipset / PCI / PSP / SMBus)"},
        {"Официальная поддержка монитора",
         "Офіційна підтримка монітора",
         "Official Monitor Support"},
        {"Библиотеки C++ 2013 (msvcr120.dll / msvcp120.dll). Используются играми прошлых лет (2013–2018 гг.) и старыми игровыми движками.",
         "Бібліотеки C++ 2013 (msvcr120.dll / msvcp120.dll). Використовуються іграми минулих років (2013–2018 рр.) та старими ігровими рушіями.",
         "C++ 2013 runtime libraries (msvcr120.dll / msvcp120.dll) used by older games (2013–2018) and legacy engines."},
        {"Библиотеки C++ 2012 (msvcr110.dll / msvcp110.dll). Требуются для игр 2012–2016 гг. и ряда системных утилит.",
         "Бібліотеки C++ 2012 (msvcr110.dll / msvcp110.dll). Потрібні для ігор 2012–2016 рр. і низки системних утиліт.",
         "C++ 2012 runtime libraries (msvcr110.dll / msvcp110.dll) needed for 2012–2016 games and legacy system utilities."},
        {"Библиотеки C++ 2010 (msvcr100.dll / msvcp100.dll). Часто требуются для классических игр (2010–2015 гг.) и старых лаунчеров.",
         "Бібліотеки C++ 2010 (msvcr100.dll / msvcp100.dll). Часто потрібні для класичних ігор (2010–2015 рр.) і старих лаунчерів.",
         "C++ 2010 runtime libraries (msvcr100.dll / msvcp100.dll) often required by classic games (2010–2015) and legacy launchers."},
        {"Дополнительные игровые библиотеки DirectX (d3dx9_43.dll, d3dx10_43.dll, d3dx11_43.dll, XInput 1.3, XAudio 2.7), отсутствующие в чистой Windows 10/11.",
         "Додаткові ігрові бібліотеки DirectX (d3dx9_43.dll, d3dx10_43.dll, d3dx11_43.dll, XInput 1.3, XAudio 2.7), відсутні у чистій Windows 10/11.",
         "Legacy DirectX gaming libraries (d3dx9_43.dll, d3dx10_43.dll, d3dx11_43.dll, XInput 1.3, XAudio 2.7) not preinstalled on Windows 10/11."},
        {"Среда выполнения .NET 8 (LTS) для десктопных программ, игровых мод-менеджеров, оверлеев и системных утилит.",
         "Середовище виконання .NET 8 (LTS) для десктопних програм, ігрових мод-менеджерів, оверлеїв і системних утиліт.",
         ".NET 8 (LTS) desktop runtime for Windows apps, game mod managers, overlays, and system utilities."},
        {"Актуальная среда выполнения .NET 9 для современных утилит Windows, лаунчеров, менеджеров модов и инструментов периферии.",
         "Актуальне середовище виконання .NET 9 для сучасних утиліт Windows, лаунчерів, менеджерів модів та інструментів периферії.",
         ".NET 9 desktop runtime required by modern Windows utilities, launchers, mod managers, and peripheral tools."},
        {"Актуальная LTS-среда выполнения .NET 10 для новейших программ, утилит Windows, лаунчеров и инструментов настройки.",
         "Актуальне LTS-середовище виконання .NET 10 для новітніх програм, утиліт Windows, лаунчерів та інструментів налаштування.",
         ".NET 10 (LTS) desktop runtime for the latest Windows apps, launchers, and configuration tools."},
        {"Классическая платформа .NET в составе Windows. Требуется для большого числа игр, конфигураторов оборудования и драйверных панелей.",
         "Класична платформа .NET у складі Windows. Потрібна для великої кількості ігор, конфігураторів обладнання та панелей драйверів.",
         "Built-in Windows .NET Framework platform required by many games, hardware configurators, and driver control panels."},
        {"Встраиваемый браузерный движок Microsoft. Нужен для работы современных лаунчеров, меню настроек периферии и виджетов Windows 11.",
         "Вбудовуваний браузерний рушій Microsoft. Потрібен для роботи сучасних лаунчерів, меню налаштувань периферії та віджетів Windows 11.",
         "Microsoft embedded web runtime needed by modern launchers, peripheral control panels, and Windows 11 widgets."},
        {"Системный загрузчик графического API Vulkan. Отвечает за запуск игр на движке Vulkan, эмуляторов и трансляторов DXVK.",
         "Системний завантажувач графічного API Vulkan. Відповідає за запуск ігор на рушії Vulkan, емуляторів і трансляторів DXVK.",
         "Vulkan graphics API loader required to run Vulkan games, emulators, and DXVK translation layers."},
        {"Библиотека пространственного 3D-звука OpenAL. Нужна для корректной работы звука в классических и инди-играх.",
         "Бібліотека просторового 3D-звуку OpenAL. Потрібна для коректної роботи звуку в класичних та інді-іграх.",
         "OpenAL spatial 3D audio library needed for proper sound output in classic and indie games."},
        {"Официальный инструмент Intel для подбора и обновления драйверов графики, Wi-Fi, Bluetooth и чипсета Intel.",
         "Офіційний інструмент Intel для підбору й оновлення драйверів графіки, Wi-Fi, Bluetooth та чипсета Intel.",
         "Official Intel utility for detecting and updating Intel graphics, Wi-Fi, Bluetooth, and chipset drivers."},
        {"Настройка мышей, клавиатур, гарнитур, веб-камер и микрофонов Razer: DPI, частота опроса, макросы, подсветка Chroma и обновление прошивки.",
         "Налаштування мишей, клавіатур, гарнітур, веб-камер і мікрофонів Razer: DPI, частота опитування, макроси, підсвітка Chroma та оновлення прошивки.",
         "Configure Razer mice, keyboards, headsets, webcams, and microphones: DPI, polling rate, macros, Chroma lighting, and firmware updates."},
        {"Управление периферией Logitech G: настройка сенсора HERO, переназначение кнопок, игровые профили, эквалайзер гарнитур и обновление прошивки.",
         "Керування периферією Logitech G: налаштування сенсора HERO, перепризначення кнопок, ігрові профілі, еквалайзер гарнітур та оновлення прошивки.",
         "Manage Logitech G gear: HERO sensor tuning, key remapping, game profiles, headset EQ, and firmware updates."},
        {"Официальный пакет SteelSeries: пространственный звук Sonar (игры/чат/микрофон), настройка клавиатур/мышей, OLED-дисплеев и прошивки.",
         "Офіційний пакет SteelSeries: просторовий звук Sonar (ігри/чат/мікрофон), налаштування клавіатур/мишей, OLED-дисплеїв і прошивки.",
         "Official SteelSeries suite: Sonar spatial audio (game/chat/mic), mouse and keyboard settings, OLED displays, and firmware updates."},
        {"Официальное ПО для периферии HyperX: настройка DPI мышей, эквалайзера/микрофона гарнитур, макросов и подсветки.",
         "Офіційне ПЗ для периферії HyperX: налаштування DPI мишей, еквалайзера/мікрофона гарнітур, макросів і підсвітки.",
         "Official HyperX peripheral software: mouse DPI, headset EQ/microphone tuning, macros, and RGB lighting."},
        {"Центр управления устройствами Corsair: профили клавиатур и мышей, синхронизация подсветки, мониторинг и обновление прошивки.",
         "Центр керування пристроями Corsair: профілі клавіатур і мишей, синхронізація підсвітки, моніторинг та оновлення прошивки.",
         "Corsair device control hub: keyboard and mouse profiles, RGB lighting sync, hardware monitoring, and firmware updates."},
        {"Фирменный софт ASUS для материнских плат и периферии: подсветка Aura Sync, управление вентиляторами и профили (по желанию).",
         "Фірмове ПЗ ASUS для материнських плат і периферії: підсвітка Aura Sync, керування вентиляторами та профілі (за бажанням).",
         "Official ASUS software for motherboards and peripherals: Aura Sync lighting, fan control, and hardware profiles (optional)."},
        {"Конфигуратор аналоговых клавиатур Wooting: настройка Rapid Trigger, точки срабатывания клавиш, профилей и прошивки.",
         "Конфігуратор аналогових клавіатур Wooting: налаштування Rapid Trigger, точки спрацювання клавіш, профілів і прошивки.",
         "Wooting analog keyboard configurator: Rapid Trigger tuning, actuation points, profiles, and firmware updates."},
        {"Веб-конфигуратор клавиатур Keychron (QMK/VIA): переназначение клавиш, макросы, подсветка и обновление прошивки прямо из браузера.",
         "Веб-конфігуратор клавіатур Keychron (QMK/VIA): перепризначення клавіш, макроси, підсвітка й оновлення прошивки просто з браузера.",
         "Keychron (QMK/VIA) web configurator: key remapping, macros, lighting, and firmware updates directly in your browser."},
        {"Официальное ПО Glorious: настройка DPI, времени отклика (debounce), подсветки, макросов и прошивки мышей/клавиатур.",
         "Офіційне ПЗ Glorious: налаштування DPI, часу відгуку (debounce), підсвітки, макросів і прошивки мишей/клавіатур.",
         "Official Glorious software: DPI, debounce time, RGB lighting, macros, and mouse/keyboard firmware updates."},
        {"Утилита настройки мышей и клавиатур Pulsar: частота опроса до 4K/8K, DPI, LoD и обновление прошивки.",
         "Утиліта налаштування мишей і клавіатур Pulsar: частота опитування до 4K/8K, DPI, LoD і оновлення прошивки.",
         "Pulsar mouse and keyboard utility: 4K/8K polling rate, DPI, lift-off distance (LoD), and firmware updates."},
        {"Официальная утилита Lamzu для настройки параметров сенсора, частоты опроса, макросов и обновления прошивки.",
         "Офіційна утиліта Lamzu для налаштування параметрів сенсора, частоти опитування, макросів і оновлення прошивки.",
         "Official Lamzu utility for sensor tuning, polling rate, macros, and firmware updates."},
        {"Конфигуратор периферии Endgame Gear: точная настройка DPI, дебаунса, Motion Sync и прошивки.",
         "Конфігуратор периферії Endgame Gear: точне налаштування DPI, дебаунсу, Motion Sync і прошивки.",
         "Endgame Gear peripheral configurator: precise DPI, debounce, Motion Sync, and firmware updates."},
        {"Управление звуковыми картами и гарнитурами Creative: эквалайзер, Acoustic Engine, Scout Mode и драйверы.",
         "Керування звуковими картами та гарнітурами Creative: еквалайзер, Acoustic Engine, Scout Mode і драйвери.",
         "Manage Creative sound cards and headsets: equalizer, Acoustic Engine, Scout Mode, and drivers."},
        {"Настройка игровых гарнитур и ЦАП EPOS / Sennheiser: объемный звук 7.1, шумоподавление микрофона и прошивка.",
         "Налаштування ігрових гарнітур і ЦАП EPOS / Sennheiser: об'ємний звук 7.1, шумозаглушення мікрофона та прошивка.",
         "Configure EPOS / Sennheiser gaming headsets and DACs: 7.1 surround sound, mic noise cancellation, and firmware updates."},
        {"Официальное ПО для мышей и клавиатур Bloody: профили чувствительности, макросы и настройка отклика.",
         "Офіційне ПЗ для мишей і клавіатур Bloody: профілі чутливості, макроси та налаштування відгуку.",
         "Official Bloody mouse and keyboard software: sensitivity profiles, macros, and response tuning."},
        {"Единый центр управления гарнитурами, мышами и клавиатурами Turtle Beach / ROCCAT.",
         "Єдиний центр керування гарнітурами, мишами та клавіатурами Turtle Beach / ROCCAT.",
         "Unified control center for Turtle Beach / ROCCAT headsets, mice, and keyboards."},
        {"ПО для микрофонов Wave, контроллеров Stream Deck и карт захвата Elgato: микширование звука и профили сцен.",
         "ПЗ для мікрофонів Wave, контролерів Stream Deck і карт захоплення Elgato: мікшування звуку та профілі сцен.",
         "Elgato software for Wave microphones, Stream Deck controllers, and capture cards: audio mixing and scene profiles."},
        {"Системный драйвер печати Windows",
         "Системний драйвер друку Windows",
         "Windows Print Subsystem"},

        // Phase messages
        {"Анализ библиотек Windows, мониторов и периферии...",
         "Аналіз бібліотек Windows, моніторів та периферії...",
         "Analyzing Windows runtimes, monitors and peripherals..."},
        {"Локальная проверка завершена",
         "Локальну перевірку завершено",
         "Local check complete"},
        {"Поиск актуальных версий...",
         "Пошук актуальних версій...",
         "Checking latest online versions..."},
        {"Актуальные версии проверены",
         "Актуальні версії перевірено",
         "Online versions checked"},
        {"Установка отменена", "Встановлення скасовано", "Installation cancelled"},
        {"Ошибка загрузки", "Помилка завантаження", "Download failed"},
        {"Проверка подписи CAB-пакета...", "Перевірка підпису CAB-пакета...", "Verifying CAB signature..."},
        {"Подпись CAB не подтверждена", "Підпис CAB не підтверджено", "CAB signature verification failed"},
        {"Распаковка драйвера (.cab)...", "Розпакування драйвера (.cab)...", "Extracting driver (.cab)..."},
        {"Ошибка распаковки CAB", "Помилка розпакування CAB", "CAB extraction failed"},
        {"Распаковка архива...", "Розпакування архіву...", "Extracting archive..."},
        {"Ошибка распаковки", "Помилка розпакування", "Extraction failed"},
        {"Установщик не найден", "Інсталятор не знайдено", "Installer not found"},
        {"Проверка цифровой подписи Authenticode...",
         "Перевірка цифрового підпису Authenticode...",
         "Verifying Authenticode signature..."},
        {"Подпись не подтверждена", "Підпис не підтверджено", "Signature verification failed"},
        {"Установка завершена", "Встановлення завершено", "Installation complete"},
        {"Установка завершена с предупреждением",
         "Встановлення завершено з попередженням",
         "Installation finished with warnings"},
        {"Пакетное обновление компонентов...",
         "Пакетне оновлення компонентів...",
         "Batch updating components..."},
        {"Создание точки восстановления...",
         "Створення точки відновлення...",
         "Creating restore point..."},
        {"Недостаточно данных для поиска драйвера",
         "Недостатньо даних для пошуку драйвера",
         "Insufficient hardware data for driver lookup"},
        {"Драйвер в Microsoft Update Catalog не найден",
         "Драйвер у Microsoft Update Catalog не знайдено",
         "Driver not found in Microsoft Update Catalog"},
        {"Отмена операции Windows Update...",
         "Скасування операції Windows Update...",
         "Cancelling Windows Update operation..."},
        {"Подключение к службе Windows Update...",
         "Підключення до служби Windows Update...",
         "Connecting to Windows Update service..."},
        {"Ошибка создания IUpdateSession",
         "Помилка створення IUpdateSession",
         "Failed to create IUpdateSession"},
        {"Ошибка создания IUpdateSearcher",
         "Помилка створення IUpdateSearcher",
         "Failed to create IUpdateSearcher"},
        {"Поиск обновлений на сервере Windows Update...",
         "Пошук оновлень на сервері Windows Update...",
         "Searching Windows Update server..."},
        {"Поиск отменён", "Пошук скасовано", "Search cancelled"},
        {"Тайм-аут поиска обновлений", "Тайм-аут пошуку оновлень", "Update search timed out"},
        {"Ошибка получения результатов поиска",
         "Помилка отримання результатів пошуку",
         "Failed to retrieve search results"},
        {"Обновления не найдены", "Оновлення не знайдено", "No updates found"},
        {"Подготовка к загрузке и установке...",
         "Підготовка до завантаження та встановлення...",
         "Preparing download and installation..."},
        {"Ожидание подтверждения UAC...",
         "Очікування підтвердження UAC...",
         "Waiting for UAC confirmation..."},
        {"Запуск установки с правами администратора...",
         "Запуск встановлення з правами адміністратора...",
         "Running elevated installation..."},
        {"Установка Windows Update отменена",
         "Встановлення Windows Update скасовано",
         "Windows Update installation cancelled"},
        {"Установка завершена (требуется перезагрузка)",
         "Встановлення завершено (потрібне перезавантаження)",
         "Installation complete (Reboot required)"},
        {"Ошибка установки обновлений Windows Update",
         "Помилка встановлення оновлень Windows Update",
         "Windows Update installation failed"},
        {"Операция Windows Update завершена",
         "Операцію Windows Update завершено",
         "Windows Update operation complete"},
        {"Скрытие выбранных обновлений...",
         "Приховування вибраних оновлень...",
         "Hiding selected updates..."},
        {"Возврат скрытых обновлений...",
         "Повернення прихованих оновлень...",
         "Restoring hidden updates..."},
        {"Выбранные обновления скрыты",
         "Вибрані оновлення приховано",
         "Selected updates hidden"},
        {"Обновления возвращены в список",
         "Оновлення повернуто до списку",
         "Updates restored to list"},
        {"Не удалось скрыть обновления",
         "Не вдалося приховати оновлення",
         "Failed to hide updates"},
        {"Не удалось вернуть обновления",
         "Не вдалося повернути оновлення",
         "Failed to restore updates"},
        {"Подготовка к установке драйверов...",
         "Підготовка до встановлення драйверів...",
         "Preparing driver installation..."},
        {"Установка .inf через DiInstallDriverW...",
         "Встановлення .inf через DiInstallDriverW...",
         "Installing .inf via DiInstallDriverW..."},
        {"Выполнение установки с правами администратора...",
         "Виконання встановлення з правами адміністратора...",
         "Running elevated installation..."},
        {"Подготовка экспорта драйверов...",
         "Підготовка експорту драйверів...",
         "Preparing driver backup export..."},
        {"Нет пакетов для экспорта.", "Немає пакетів для експорту.", "No packages to export."},
        {"Удаление неиспользуемых пакетов из DriverStore...",
         "Видалення невикористаних пакетів з DriverStore...",
         "Removing unused packages from DriverStore..."},
        {"Очистка кэша загрузок Windows Update...",
         "Очищення кешу завантажень Windows Update...",
         "Clearing Windows Update download cache..."},
        {"Проверка игровых библиотек, мониторов и ПО периферии завершена.",
         "Перевірку ігрових бібліотек, моніторів та ПЗ периферії завершено.",
         "Gaming runtimes, monitors, and peripheral software scan complete."},
        {"Онлайн-проверка актуальных версий завершена.",
         "Онлайн-перевірку актуальних версій завершено.",
         "Online version check complete."},
        {"Запуск нативного сканирования PnP-устройств и EDID (SetupAPI)...",
         "Запуск нативного сканування PnP-пристроїв та EDID (SetupAPI)...",
         "Starting native PnP device and EDID scan (SetupAPI)..."},
        {"Операция завершена", "Операцію завершено", "Operation complete"},
        {"Операция завершена с предупреждениями",
         "Операцію завершено з попередженнями",
         "Operation finished with warnings"},
    };

    for (const auto& rule : kExactRules) {
        if (text == rule.ru) {
            return std::string(lang == Lang::Uk ? rule.uk : rule.en);
        }
    }

    // Composite / parameterized strings: longest prefixes and fragments replaced cleanly in order.
    std::string out = text;
    static constexpr detail::DynRule kFragmentRules[] = {
        {"Сканирование завершено: устройств — ",
         "Сканування завершено: пристроїв — ",
         "Scan complete: devices — "},
        {", мониторов — ", ", моніторів — ", ", monitors — "},
        {", требуют внимания — ", ", потребують уваги — ", ", needing attention — "},
        {"Диагностика системы: отключённых критичных служб — ",
         "Діагностика системи: вимкнених критичних служб — ",
         "System diagnostics: disabled critical services — "},
        {"Диагностика системы: отключённых критических служб — ",
         "Діагностика системи: вимкнених критичних служб — ",
         "System diagnostics: disabled critical services — "},
        {", контроллеров с замечаниями — ",
         ", контролерів із зауваженнями — ",
         ", controllers needing attention — "},
        {"Отчёт о системе сохранён: ", "Звіт про систему збережено: ", "System report saved: "},
        {"Установлен (d3dx9/10/11)", "Встановлено (d3dx9/10/11)", "Installed (d3dx9/10/11)"},
        {"Требуется установка (4.8.1)", "Потрібне встановлення (4.8.1)", "Install required (4.8.1)"},
        {"Доступно обновление (", "Доступне оновлення (", "Update available ("},
        {"Базовый драйвер Windows (", "Базовий драйвер Windows (", "Generic Windows Driver ("},
        {"Утилита установлена (", "Утиліту встановлено (", "Utility installed ("},
        {"Установлено (", "Встановлено (", "Installed ("},
        {"Установлен (", "Встановлено (", "Installed ("},
        {"Не установлен (", "Не встановлено (", "Not installed ("},
        {"Устаревший драйвер (", "Застарілий драйвер (", "Outdated driver ("},
        {"Вендор-драйвер (", "Драйвер виробника (", "Vendor driver ("},
        {"Драйверы AMD (", "Драйвери AMD (", "AMD Drivers ("},
        {"Актуален (", "Актуальний (", "Up to date ("},
        {"Ошибка (Код ", "Помилка (Код ", "Error (Code "},
        {"Код ошибки ", "Код помилки ", "Error code "},
        {" г.)", " р.)", ")"},

        // Role prefixes & controller descriptions
        {"Wi-Fi адаптер: ", "Wi-Fi адаптер: ", "Wi-Fi Adapter: "},
        {"Сеть (LAN): ", "Мережа (LAN): ", "Network (LAN): "},
        {"Аудио платы: ", "Аудіо плати: ", "Onboard Audio: "},
        {" / Драйвер монитора", " / Драйвер монітора", " / Monitor Driver"},

        // Dynamic phase prefixes
        {"Скачивание: ", "Завантаження: ", "Downloading: "},
        {"Установка драйвера ", "Встановлення драйвера ", "Installing driver "},
        {"Установка: ", "Встановлення: ", "Installing: "},
        {"Поиск драйвера в Microsoft Update Catalog: ",
         "Пошук драйвера у Microsoft Update Catalog: ",
         "Searching Microsoft Update Catalog: "},
        {"Установлен драйвер ", "Встановлено драйвер ", "Installed driver "},
        {"Найдено обновлений: ", "Знайдено оновлень: ", "Updates found: "},
        {"Включение службы ", "Увімкнення служби ", "Enabling service "},
        {" пропущен: используется системой или устройством ",
         " пропущено: використовується системою або пристроєм ",
         " skipped: in use by system or device "},

        // Monitored service display names & purposes
        {"Центр обновления Windows", "Служба Windows Update", "Windows Update Service"},
        {"Установщик модулей Windows", "Інсталятор модулів Windows", "Windows Modules Installer"},
        {"Оркестратор обновлений (UsoSvc)", "Оркестратор оновлень (UsoSvc)", "Update Orchestrator (UsoSvc)"},
        {"Фоновая служба передачи (BITS)", "Фонова служба передавання (BITS)", "Background Intelligent Transfer (BITS)"},
        {"Службы криптографии", "Служби криптографії", "Cryptographic Services"},
        {"Служба установки устройств", "Служба встановлення пристроїв", "Device Install Service"},
        {"Теневое копирование тома (VSS)", "Тіньове копіювання тома (VSS)", "Volume Shadow Copy (VSS)"},
        {"Поиск, скачивание и установка драйверов и обновлений Windows Update.",
         "Пошук, завантаження та встановлення драйверів і оновлень Windows Update.",
         "Finds, downloads, and installs Windows Update drivers and OS updates."},
        {"Установка системных компонентов, обновлений ОС, .NET и хранилища CBS.",
         "Встановлення системних компонентів, оновлень ОС, .NET та сховища CBS.",
         "Installs Windows features, OS updates, .NET components, and CBS packages."},
        {"Координирует сеансы скачивания и установки через Windows Update.",
         "Координує сеанси завантаження та встановлення через Windows Update.",
         "Coordinates Windows Update download and installation sessions."},
        {"Отвечает за надёжную докачку пакетов Windows Update и системных файлов.",
         "Відповідає за надійне дозавантаження пакетів Windows Update і системних файлів.",
         "Handles resilient background downloads for Windows Update packages."},
        {"Проверяет цифровые подписи драйверов (.cat), сертификаты и каталоги обновлений.",
         "Перевіряє цифрові підписи драйверів (.cat), сертифікати та каталоги оновлень.",
         "Verifies driver catalog signatures (.cat), certificates, and update stores."},
        {"Устанавливает и настраивает PnP-драйверы (.inf) и периферию.",
         "Встановлює та налаштовує PnP-драйвери (.inf) і периферію.",
         "Installs and configures PnP drivers (.inf) and connected hardware."},
        {"Обнаружение подключаемого оборудования, мониторов, USB и PCI-контроллеров.",
         "Виявлення обладнання, моніторів, USB та PCI-контролерів.",
         "Detects connected hardware, monitors, USB devices, and PCI controllers."},
        {"Необходимо для создания точек восстановления системы перед установкой драйверов.",
         "Необхідно для створення точок відновлення системи перед встановленням драйверів.",
         "Required to create System Restore points before driver installation."},
        {"Встроенная защита Windows (может быть отключена сторонним антивирусом).",
         "Вбудований захист Windows (може бути вимкнений стороннім антивірусом).",
         "Built-in Windows security (may be disabled by a third-party AV)."},

        // Purpose descriptions in Components tab
        {"Официальный WHQL-драйвер беспроводного контроллера из Microsoft Update Catalog (.cab).",
         "Офіційний WHQL-драйвер бездротового контролера з Microsoft Update Catalog (.cab).",
         "Official WHQL wireless controller driver from Microsoft Update Catalog (.cab)."},
        {"Официальный WHQL-драйвер проводного сетевого контроллера из Microsoft Update Catalog (.cab).",
         "Офіційний WHQL-драйвер дротового мережевого контролера з Microsoft Update Catalog (.cab).",
         "Official WHQL Ethernet controller driver from Microsoft Update Catalog (.cab)."},
        {"Официальный WHQL-драйвер адаптера Bluetooth из Microsoft Update Catalog (.cab).",
         "Офіційний WHQL-драйвер адаптера Bluetooth з Microsoft Update Catalog (.cab).",
         "Official WHQL Bluetooth adapter driver from Microsoft Update Catalog (.cab)."},
        {"Официальный WHQL-драйвер встроенного аудиокодека платы из Microsoft Update Catalog (.cab).",
         "Офіційний WHQL-драйвер вбудованого аудіокодека плати з Microsoft Update Catalog (.cab).",
         "Official WHQL onboard audio codec driver from Microsoft Update Catalog (.cab)."},
        {"Официальный пакет драйверов чипсета AMD Ryzen / AM4 / AM5 (энергосбережение, USB, PCIe, GPIO, PSP).",
         "Офіційний пакет драйверів чипсета AMD Ryzen / AM4 / AM5 (енергозбереження, USB, PCIe, GPIO, PSP).",
         "Official AMD Ryzen / AM4 / AM5 chipset driver package (power plan, USB, PCIe, GPIO, PSP)."},
        {"Официальный драйвер видеокарты NVIDIA: отвечает за FPS и совместимость в новых играх, работу DLSS, G-SYNC, NVENC и аппаратное ускорение Windows.",
         "Офіційний драйвер відеокарти NVIDIA: відповідає за FPS і сумісність в іграх, DLSS, G-SYNC, NVENC та апаратне прискорення Windows.",
         "Official NVIDIA GPU driver: gaming performance, DLSS, G-SYNC, NVENC, and Windows hardware acceleration."},
        {"Официальный пакет драйверов AMD Radeon: отвечает за игровую производительность, FSR, Anti-Lag, FreeSync и аппаратное ускорение.",
         "Офіційний пакет драйверів AMD Radeon: відповідає за ігрову продуктивність, FSR, Anti-Lag, FreeSync та апаратне прискорення.",
         "Official AMD Radeon driver suite: gaming performance, FSR, Anti-Lag, FreeSync, and hardware acceleration."},
        {"Фирменная утилита и INF-драйвер монитора: отвечает за точный цветовой профиль (ICC), управление яркостью/режимами OSD с ПК и поддержку высокой герцовки/HDR.",
         "Фірмова утиліта та INF-драйвер монітора: відповідає за точний колірний профіль (ICC), керування яскравістю/режимами OSD з ПК і підтримку високої герцовки/HDR.",
         "Official monitor utility & INF driver: accurate ICC color profile, PC OSD/brightness control, high refresh rate & HDR."},
        {"64-битные библиотеки среды выполнения C/C++ (MSVC). Обязательны для запуска большинства современных игр, лаунчеров и программ на Windows.",
         "64-бітні бібліотеки середовища виконання C/C++ (MSVC). Обов'язкові для запуску більшості сучасних ігор, лаунчерів і програм у Windows.",
         "64-bit C/C++ (MSVC) runtime libraries required by most modern Windows games, launchers, and apps."},
        {"32-битные библиотеки среды выполнения C/C++ (MSVC). Необходимы для 32-битных игр, игровых лаунчеров (Steam и др.) и оверлеев.",
         "32-бітні бібліотеки середовища виконання C/C++ (MSVC). Необхідні для 32-бітних ігор, ігрових лаунчерів (Steam тощо) та оверлеїв.",
         "32-bit C/C++ (MSVC) runtime libraries needed for 32-bit games, game launchers (Steam, etc.), and overlays."},
    };

    for (const auto& rule : kFragmentRules) {
        detail::ReplaceAllInPlace(out, rule.ru, lang == Lang::Uk ? rule.uk : rule.en);
    }

    if (lang == Lang::En) {
        auto replaceByteUnit = [](std::string& text, std::string_view from, std::string_view to) {
            size_t pos = 0;
            while ((pos = text.find(from, pos)) != std::string::npos) {
                const bool precededByDigit =
                    (pos > 0 && text[pos - 1] >= '0' && text[pos - 1] <= '9');
                const size_t afterPos = pos + from.size();
                const unsigned char nextCh =
                    (afterPos < text.size()) ? static_cast<unsigned char>(text[afterPos]) : 0;
                const bool followedByWordChar =
                    (afterPos < text.size()) &&
                    ((nextCh >= 'A' && nextCh <= 'Z') ||
                     (nextCh >= 'a' && nextCh <= 'z') ||
                     (nextCh >= '0' && nextCh <= '9') ||
                     (nextCh >= 0x80));
                if (precededByDigit && !followedByWordChar) {
                    text.replace(pos, from.size(), to);
                    pos += to.size();
                } else {
                    pos += from.size();
                }
            }
        };
        replaceByteUnit(out, " ГБ", " GB");
        replaceByteUnit(out, " МБ", " MB");
        replaceByteUnit(out, " КБ", " KB");
        replaceByteUnit(out, " Б", " B");
    }
    return out;
}

} // namespace hdm
