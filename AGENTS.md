# Правила проекта: Hybrid Driver Manager

## Стек и сборка
- Язык: C++20 (Windows 10/11).
- UI: Dear ImGui (ветка docking) + DirectX 11 + Win32 backend. Не использовать тяжелые UI-фреймворки (Qt, wxWidgets, WebView).
- Сборка: CMake со статической линковкой CRT (/MT для MSVC или -static для MinGW), чтобы итоговый .exe запускался на голой Windows без установки доп. рантаймов и DLL.
- Системные библиотеки: d3d11, dxgi, setupapi, newdev, ole32, oleaut32, srclient, cfgmgr32.
- Git (локальный CLI): `%USERPROFILE%\Documents\Codex\Soft\.tools\git\cmd\git.exe`

## Критические правила кода
1. Кириллица и кодировка: Все исходные файлы сохранять в UTF-8. При инициализации ImGui обязательно грузить C:\Windows\Fonts\segoeui.ttf с GetGlyphRangesCyrillic(). Для вызова Win32 API использовать Unicode-версии функций (*W, std::wstring) с корректной конвертацией UTF-8 <-> UTF-16 (MultiByteToWideChar / WideCharToMultiByte).
2. Неблокирующий UI: Любые обращения к SetupAPI, Windows Update COM API и pnputil.exe выполнять строго в фоновых потоках (std::thread), передавая состояние в UI через потокобезопасную очередь/мьютекс.
3. Безопасность COM: В каждом фоновом потоке, работающем с IUpdateSession или IFileOpenDialog, корректно вызывать CoInitializeEx и CoUninitialize.
4. Проверка сборки: После любых изменений в коде или CMakeLists.txt обязательно запускать компиляцию и проверять, что .exe собирается без ошибок.
## Требования безопасности, репутации и совместимости (АВ / Античиты)

- Права доступа: В app.manifest использовать asInvoker (не requireAdministrator). Запрос прав администратора выполнять только при нажатии кнопки установки драйвера через отдельный вызов с повышенными правами (ShellExecuteExW с "runas").
- Метаданные PE: Обязательно включить файл ресурсов resources/version.rc с заполненным блоком VERSIONINFO (название продукта, версия, описание, авторские права).
- Нативные вызовы: Для установки .inf отдавать предпочтение нативному API (DiInstallDriverW из newdev.dll) вместо скрытого запуска дочерних консольных процессов.
- Управление ресурсами: Строго закрывать все дескрипторы HDEVINFO (SetupDiDestroyDeviceInfoList) и COM-интерфейсы сразу после завершения операции. Не использовать упаковщики бинарников (UPX и аналоги).