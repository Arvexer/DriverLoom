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
    } else if (primary == LANG_ENGLISH) {
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
        {"Портал поддержки", "Портал підтримки", "Support Portal"},
        {"Портал AMD", "Портал AMD", "AMD Portal"},
        {"Онлайн-установщик", "Онлайн-інсталятор", "Web Installer"},
        {"Онлайн", "Онлайн", "Online"},
        {"Сайт производителя", "Сайт виробника", "Vendor Website"},
        {"Сайт LunarG", "Сайт LunarG", "LunarG Website"},
        {"Сайт Razer", "Сайт Razer", "Razer Website"},
        {"Каталог MS", "Каталог MS", "MS Catalog"},
        {"Synapse 4 (актуальный)", "Synapse 4 (актуальний)", "Synapse 4 (Latest)"},
        {"Обнаружено в системе", "Виявлено в системі", "Detected on system"},
        {"Чипсет AMD (PCI\\VEN_1022)", "Чипсет AMD (PCI\\VEN_1022)", "AMD Chipset (PCI\\VEN_1022)"},
        {"Игры и системные библиотеки Windows", "Ігри та системні бібліотеки Windows", "Games & Windows System Libraries"},

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
        detail::ReplaceAllInPlace(out, " ГБ", " GB");
        detail::ReplaceAllInPlace(out, " МБ", " MB");
        detail::ReplaceAllInPlace(out, " КБ", " KB");
        detail::ReplaceAllInPlace(out, " Б", " B");
    }
    return out;
}

} // namespace hdm
