# Karabas Flasher: информация для разработчиков

Сборка из исходников, установка зависимостей, пакеты, релизы и устройство проекта. Описание для пользователей - в [README.md](../README.md).

Одна кодовая база на Qt6 для Linux, Windows и macOS. Общий код (интерфейс, сеть, логика прошивки)
одинаков везде; всё, что зависит от ОС при работе с дисками, вынесено в `src/Platform.h`
и реализовано отдельно для каждой системы:

| ОС      | Файл                      | Статус |
|---------|---------------------------|--------|
| Linux   | `src/platform_linux.cpp`  | проверен на железе |
| Windows | `src/platform_windows.cpp`| проверен на железе |
| macOS   | `src/platform_macos.cpp`  | проверен на железе |

## Установка на Ubuntu / Debian (.deb)

Готовый пакет лежит в разделе Releases на GitHub (для Ubuntu 24.04 и новее). `apt` сам доставит все
зависимости (Qt 6, udisks2 и т.д.) и установит правило доступа к порту:

    sudo apt install ./karabas-flasher_1.0.3_amd64_ubuntu24.04.deb

**Ubuntu 22.04 и старше:** в их репозиториях нет Qt SerialPort (`libqt6serialport6`), поэтому `.deb` не подойдёт - 
используйте AppImage (Qt внутри). На Debian 12 `.deb` не проверялся: он собран на Ubuntu 24.04 и может требовать более новую glibc.
Запуск: пункт «Karabas Flasher» в меню приложений или команда `KarabasFlasher`.
Для остальных дистрибутивов - сборка из исходников (ниже).

## Другие дистрибутивы

**AppImage (любой дистрибутив).** В релизе есть один файл `KarabasFlasher-<версия>-x86_64.AppImage` с Qt внутри:

    chmod +x KarabasFlasher-1.0.3-x86_64.AppImage
    ./KarabasFlasher-1.0.3-x86_64.AppImage

На некоторых системах нужен пакет `libfuse2` (иначе: `./KarabasFlasher-...AppImage --appimage-extract-and-run`).
Системные части (udisks2, polkit, systemd) должны быть установлены - на обычном рабочем столе они уже есть.
Правило доступа к порту AppImage в систему поставить не может: при ошибке «Доступ запрещён» программа сама
предложит установить его через окно с паролем. Нативного Wayland в образе нет, окно работает через XWayland.

**Arch / Manjaro (PKGBUILD).** Пакет собирается у вас из исходников, зависимости ставятся автоматически:

    cd packaging/arch/git && makepkg -si          # последняя версия из git
    cd packaging/arch/release && updpkgsums && makepkg -si    # версия по тегу vX.Y.Z

Пакет ставит правило udev, пункт меню и иконку. Для сборки из git репозиторий на GitHub должен быть публичным.
Файлы подходят и для публикации в AUR (нужна учётная запись на aur.archlinux.org).

## Сборка

Нужны CMake, компилятор C++17 и Qt 6 с модулями Base и SerialPort.

Распаковка `extra.zip` выполняется библиотекой `minizip-ng`, которая собирается
автоматически через `FetchContent` (исходники скачиваются с GitHub при первой
конфигурации и кэшируются в `build/_deps/`). Внешние архиваторы (`7za`, `7z`,
`libarchive`) не нужны. Для сборки требуется сеть при первой конфигурации CMake;
если `zlib` не найден в системе (типично для Windows), он тоже скачивается через
`FetchContent`.

### Linux

Debian / Ubuntu:

    sudo apt install build-essential cmake qt6-base-dev qt6-serialport-dev udisks2 eject zlib1g-dev

Fedora: `sudo dnf install gcc-c++ cmake qt6-qtbase-devel qt6-qtserialport-devel udisks2 zlib-devel`
Arch:   `sudo pacman -S base-devel cmake qt6-base qt6-serialport udisks2 zlib`

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    ./build/KarabasFlasher --log

### Windows

Установите Qt 6 (комплект MSVC или MinGW) и откройте `CMakeLists.txt` в Qt Creator, либо:

    cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.x.x/<kit>
    cmake --build build --config Release

Для раздачи соберите папку с DLL: `windeployqt build\Release\KarabasFlasher.exe`.
Без подписи кода SmartScreen может показать предупреждение.

### macOS

    brew install cmake qt
    cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

Упаковка: `macdeployqt build/KarabasFlasher.app -dmg`. Homebrew-Qt собран под одну архитектуру,
поэтому бинарник будет для вашего процессора (arm64 или Intel). Универсальная сборка требует
универсального Qt. Без подписи и нотаризации Gatekeeper блокирует первый запуск: откройте
приложение через «Системные настройки - Конфиденциальность и безопасность - Всё равно открыть».
Начиная с macOS Ventura система может запросить доступ к съёмным томам.

`minizip-ng` и `zlib` собираются через `FetchContent`; отдельно ставить их не нужно.

Ключ `--log` включает запись лога в файл `flasher_log.txt` рядом с исполняемым файлом программы.
Если каталог с программой недоступен для записи (Program Files, `/usr/bin`, `.app` в `/Applications`),
лог сохраняется в пользовательский каталог данных: `~/.local/share/KarabasFlasher/flasher_log.txt` (Linux),
`%APPDATA%\KarabasFlasher\flasher_log.txt` (Windows), `~/Library/Application Support/KarabasFlasher/` (macOS).

## Лицензия

Код проекта распространяется по лицензии **MIT** (файл `LICENSE`). Программа использует Qt 6 по лицензии
LGPL-3.0 (подключается динамически; подробности в `THIRD_PARTY.md`). Прошивки и ядра, которые программа
скачивает с GitHub, принадлежат их авторам и в проект не входят.

## Версии и релизы

Версия задаётся **в одном месте** - `project(KarabasFlasher VERSION X.Y.Z ...)` в `CMakeLists.txt`.
Оттуда она попадает в заголовок окна, в журнал (вместе с описанием из git, например `v0.1.0-3-gabc1234`),
в `--version` и в версию macOS-приложения. Схема `MAJOR.MINOR.PATCH`: исправление ошибки - PATCH,
новая возможность - MINOR, несовместимое изменение - MAJOR. Все три системы проверены на железе,
поэтому используются версии `1.x` (текущая - `1.0.3`).

Как выпустить версию:

    # 1. поднять версию в CMakeLists.txt (project(... VERSION 1.1.0 ...)) и дописать CHANGELOG.md
    git add -A && git commit -m "Release 1.1.0"
    git push
    # 2. поставить тег (строго v + номер из CMakeLists.txt) и отправить его
    git tag v1.1.0
    git push origin v1.1.0

По тегу Actions соберёт три системы и создаст релиз на GitHub с файлами `.dmg`, `.zip`, `.deb` и `.AppImage`.
Если тег не совпадает с версией в `CMakeLists.txt`, выпуск остановится с сообщением об ошибке.
Описание из git считается при конфигурации CMake; при сборке из zip без git будет `no-git`.

## Автоматическая сборка (GitHub Actions)

В `.github/workflows/build.yml` описана сборка под три системы. Положите проект в репозиторий на
GitHub (`git init && git add . && git commit -m init && git push`), откройте вкладку **Actions** -
сборка запустится сама. Готовые файлы лежат в разделе **Artifacts** завершённого запуска:
`KarabasFlasher-macos` (.dmg, universal arm64 + Intel), `KarabasFlasher-windows` (zip с DLL Qt),
`KarabasFlasher-linux-ubuntu24.04` (.deb), `KarabasFlasher-appimage` (AppImage с Qt внутри).
Заодно это проверка компиляции на всех системах. `minizip-ng` и `zlib` собираются через
`FetchContent` прямо в CI, поэтому отдельные пакеты для них не нужны.

Сборки не подписаны. На macOS скачанный из браузера `.dmg` получает карантин: после перетаскивания в
«Программы» выполните `xattr -dr com.apple.quarantine /Applications/KarabasFlasher.app`
или откройте приложение через «Всё равно открыть» в настройках безопасности.

## Настройка системы

### Linux

1. **Доступ к порту.** При ошибке «Доступ запрещён» программа сама предложит установить правило
   udev (`TAG+="uaccess"`) для вашего устройства; пароль администратора запросит polkit (`pkexec`)
   один раз, перелогин не нужен. Если `pkexec` недоступен - добавьте себя в группу `dialout`
   (в Arch и Manjaro - `uucp`) и перелогиньтесь:

       sudo usermod -aG dialout $USER

   При установке из пакета правило `packaging/70-karabas.rules` ставится в `lib/udev/rules.d`
   автоматически (`cmake --install`), и пароль не нужен вообще.

2. **ModemManager** может опрашивать `/dev/ttyACM*` AT-командами и мешать. Найдите VID:PID (`lsusb`)
   и создайте `/etc/udev/rules.d/70-karabas.rules`:

       ATTRS{idVendor}=="XXXX", ATTRS{idProduct}=="YYYY", ENV{ID_MM_DEVICE_IGNORE}="1"

   затем `sudo udevadm control --reload && sudo udevadm trigger`.

3. **udisks2** нужен для монтирования и извлечения SD-карты (`udisksctl` и `gdbus`/`busctl`).
   Извлечение выполняется D-Bus-вызовом udisks `Drive.Eject` (SCSI-eject с правами системы),
   запасной вариант - утилита `eject`. `udisksctl power-off` не используется: он отключает
   питание USB-порта, и Карабас не возвращается в обычный режим.

### Windows и macOS

Дополнительная настройка не нужна: драйвер CDC в Windows 10 и новее встроенный, на macOS порт
виден как `/dev/cu.usbmodem*`.

## Протокол обмена с платой

Программа общается с платой AT-командами по последовательному порту (115200 8N1):

| Команда | Ответ | Назначение |
|---|---|---|
| `AT+KARABAS=version` | `+KARABAS:<firmware>;<board>;<core id>;<core build>` | версия прошивки и идентификатор платы |
| `AT+KARABAS=msc` | `OK` | режим кард-ридера, карта №1 (основная, с ядрами) |
| `AT+KARABAS=msc2` | `OK` | режим кард-ридера, карта №2 (дополнительная) |

Идентификатор платы в ответе на `version`: `1` — Karabas Go, `2` — Karabas Go Mini rev A-F,
`3` — Karabas Go Mini rev G. Ответ `ERROR` означает, что прошивка не поддерживает команду.

## Как программа находит диск

Диск не ищется по букве, пути или метке. Перед командой MSC делается снимок съёмных дисков с
готовым носителем, а SD - это диск, у которого носитель появился после команды. Загрузчик
RP2040 ищется по метке `RPI-RP2`. Если подходящих дисков несколько, программа останавливается
с сообщением, а не угадывает.

На Windows `RPI-RP2` ищется среди **всех** логических дисков, а не только `DRIVE_REMOVABLE`:
RP2040 в режиме загрузчика иногда определяется как `DRIVE_FIXED`, и фильтр по типу терял диск.
Метка тома читается с несколькими повторными попытками: сразу после появления буквы Windows
может ещё не дочитать её.

## Проверка диска перед записью

Перед записью на SD-карту вызывается `Worker::confirmDrive()`:

- на диске есть ядра `.kg1`/`.kg2`/`.kg3` или папка `backup_*` — запись начинается без вопросов;
- диск пустой — запись начинается с предупреждением в журнале;
- файлы есть, но признаков Карабаса нет — рабочий поток шлёт сигнал `confirmUnrecognizedDrive`
  через `Qt::BlockingQueuedConnection`, GUI показывает диалог с узлом, меткой, размером и списком
  файлов, ответ возвращается обратно. При отказе диск не извлекается (он чужой), а `abortReason`
  равен `unrecognized drive`.

## Защита от чужой платы в BOOTSEL

Если порт Карабаса ещё жив, а `Platform::bootloaderDisks()` уже не пуст, значит диск `RPI-RP2`
принадлежит другой RP2040-плате (у живого Карабаса в нормальном режиме его быть не может).
Проверка `Worker::foreignBootloaderPresent()` выполняется в обеих ветках прошивки (с MSC-сеансом
и без него) и прерывает операцию с сообщением «Another RP2040 board is in BOOTSEL mode».

## Дополнительный контент (`extra.zip`)

Архив `extra.zip` скачивается с GitHub и распаковывается встроенной библиотекой `minizip-ng`
(собирается через `FetchContent`). Внешние архиваторы не используются. При распаковке:

- проверяется CRC каждого файла (`mz_zip_reader_entry_close`) и целостность каталога
  (`MZ_END_OF_LIST`);
- считается фактически записанный объём (лимит 2 ГБ), а не только заявленный в каталоге;
- проверяется свободное место в кэше перед распаковкой;
- имена с компонентом `..`, абсолютные пути и обратные слеши отсекаются;
- симлинки из архива не копируются на SD-карту.

Содержимое верхней папки `extra/` (если она есть в архиве) копируется в корень SD-карты без
вложенной папки. Остальные элементы корня копируются как есть. Файлы, которых нет в архиве,
на карте не трогаются.

## Диагностика

Все три платформы проверены на железе: скачивание прошивки и ядер, прошивка RP2040, обновление
ядер на SD-карте, режим кард-ридера. При проблемах запустите программу с `--log` и пришлите файл лога.
