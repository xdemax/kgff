# Karabas Flasher

[Русский](#русский) | [English](#english)

---

## Русский

Программа для обновления **прошивки** и **ядер** плат семейства Karabas Go. Работает в Windows, Linux и macOS.

Раньше для обновления нужно было вручную скачать файлы с GitHub, перевести плату в режим загрузчика,
скопировать прошивку и заменить ядра на SD-карте. Karabas Flasher делает всё это в одном окне.

**Поддерживаемые платы:** Karabas Go, Karabas Go Mini.

## Что умеет программа

- **Скачивает** актуальные прошивки и ядра из репозиториев автора плат на GitHub.
- **Обновляет прошивку** платы (микроконтроллер RP2040).
- **Заменяет ядра** (файлы `.kg1`, `.kg2`, `.kg3`) на SD-карте платы. Старые ядра не удаляются, а переносятся в резервную папку.
- **Открывает SD-карту платы** как обычный диск, чтобы вы могли работать с её файлами напрямую.
- **Копирует дополнительный контент** (ROM-ы, образы дисков и т.п.) из архива `extra.7z` на SD-карту. Файлы, которые уже есть на карте, перед заменой переносятся в папку `backup_*`. Перед копированием проверяется свободное место, а перед распаковкой — распакованный размер архива (лимит 2 ГБ). Симлинки из архива не копируются.

Прошивку и ядра можно обновлять вместе или по отдельности: например, только ядра.

> **Важно.** Обновление ядер и прямой доступ к первой карте памяти появились в прошивках от 18.10.2026 и позже.
> Возможность использовать вторую карту памяти (режим MSC 2) появилась в прошивках от 27.10.2026.
> Автоопределение модели устройства и версии прошивки появилось в прошивках от 29.10.2026.
> Если вы не уверены, какая прошивка установлена на плате, сначала обновите прошивку — без обновления ядер.
> И только после успешного обновления прошивки можно обновлять ядра.

## Как пользоваться

1. Подключите Karabas к компьютеру кабелем USB.
2. Выберите свою модель в списке **Select Device Model** или нажмите кнопку с лупой — программа сама
   спросит у платы версию (`AT+KARABAS=version`) и выберет нужную модель.
3. Отметьте, что нужно обновить:
   - **Update firmware**: прошивка платы (включено по умолчанию);
   - **Update cores**: ядра на SD-карте;
   - **Download extra content**: дополнительный контент (ROM-ы, образы дисков и т.п.).

   Можно отметить любые галки в любом сочетании.
4. Нажмите **Download via GitHub**. Программа скачает всё отмеченное. Ядер много, и скорость зависит от вашего интернета;
   ход загрузки виден в журнале внизу окна.
5. В списке **Serial port** выберите порт платы. В списке показываются только USB-устройства; нужный порт появляется,
   когда плата подключена (если его нет, нажмите **Refresh ports**). Пункт «(not selected - flash without reset)» нужен,
   только если плата уже переведена в режим загрузчика вручную.
6. Нажмите **FLASH!** и дождитесь в журнале строки «Process completed successfully».

Во время работы не отключайте плату и не закрывайте окно. Если окно всё же закрыть, программа спросит подтверждение и остановится в безопасный момент.

### Автоопределение модели

Кнопка с лупой рядом со списком моделей отправляет плате команду `AT+KARABAS=version` и ждёт ответ
до двух секунд. Ответ имеет вид `+KARABAS:<firmware>;<board>;<core id>;<core build>`, где `board` —
идентификатор платы: `1` — Karabas Go, `2` — Karabas Go Mini rev A-F, `3` — Karabas Go Mini rev G.
Полученный ответ выводится в журнал, а в списке моделей автоматически выбирается нужная.

### Работа с SD-картой вручную

Кнопка **Open SD card >** переводит плату в режим USB-кард-ридера: SD-карта появляется в системе как обычный диск,
и программа открывает её в файловом менеджере. Значок **«?»** рядом с кнопкой показывает подсказку. Чтобы выйти из этого
режима, безопасно извлеките карту средствами системы.

Переключатель **1 / 2** рядом с кнопкой выбирает карту: **1** — основная SD-карта с ядрами ПЛИС, **2** — дополнительная
карта для прочих файлов. Обновление ядер всегда работает с картой №1, независимо от положения переключателя.

Если плата уже находится в режиме кард-ридера, повторное нажатие **Open SD card >** не отправляет команду ещё раз:
программа видит смонтированный диск Карабаса и подсказывает, как выйти из режима. Диск определяется по содержимому
(ядра `.kg1`/`.kg2`/`.kg3` в корне или папка `backup_*`), поэтому случайно вставленная флешка не будет принята за плату.

## Что происходит внутри

**Обновление прошивки.** Программа отправляет на порт платы специальный сигнал (соединение на скорости 1200 бод), после которого RP2040
перезапускается в режиме загрузчика и появляется в системе как диск `RPI-RP2`. Программа копирует на него файл прошивки,
и плата перезагружается уже с новой прошивкой.

**Обновление ядер.** Программа командой переводит плату в режим кард-ридера, находит появившийся диск SD-карты, создаёт на нём
папку `backup_ГГГГ-ММ-ДД_ЧЧ-ММ-СС` и переносит в неё старые ядра нужной модели, а затем копирует новые. В резервную папку попадают
только релизные ядра (те, что входят в набор с GitHub); пользовательские и тестовые ядра, положенные на карту вручную, остаются
на месте. После этого карта безопасно извлекается из системы, и плата возвращается в обычный режим. Если на каком-то шаге что-то
идёт не так, изменения откатываются: старые ядра возвращаются на место.

**Проверки.** Перед записью программа проверяет, что файл прошивки действительно является образом UF2 (включая family ID RP2040,
номера блоков и размеры полезной нагрузки во всех блоках), что скачан полный набор ядер и что найден именно диск платы, а не другая
флешка. Если к компьютеру подключено несколько подходящих дисков, программа остановится и попросит оставить только плату.

**Ошибки порта.** Если порт не открылся или запись в него не удалась (нет прав, порт занят другой программой), программа
сообщает об этом сразу и не ждёт появления диска. Если порт реально исчез (например, отключили кабель) и диск `RPI-RP2`
уже виден, прошивка продолжается без 1200-бод-сброса — но только в ветке без MSC-сеанса, где имя порта не могло измениться.

## Установка

Готовые сборки лежат на странице [Releases](https://github.com/xdemax/kgff/releases).

| Система | Что скачать | Как запустить |
|---|---|---|
| **Windows** | архив `…-windows.zip` | Распакуйте и запустите `KarabasFlasher.exe`. Если Windows покажет предупреждение SmartScreen, нажмите «Подробнее» и «Выполнить в любом случае» (сборка не подписана) |
| **macOS** | образ `…-macos.dmg` | Перетащите приложение в «Программы». При первом запуске система может заблокировать неподписанное приложение: откройте «Системные настройки», «Конфиденциальность и безопасность», «Всё равно открыть». Либо выполните в терминале `xattr -dr com.apple.quarantine /Applications/KarabasFlasher.app`. Разрешите доступ к съёмным томам, когда система спросит |
| **Ubuntu 24.04 и новее** | пакет `…ubuntu24.04.deb` | `sudo apt install ./имя-файла.deb`: зависимости поставятся сами |
| **Другие Linux** (в том числе Ubuntu 22.04) | `…AppImage` | `chmod +x файл.AppImage` и запустить. Если не запускается, установите пакет `libfuse2` |
| **Arch / Manjaro** | исходники из репозитория | `cd packaging/arch/git && makepkg -si` |

**Linux: доступ к порту.** При первом подключении программа может сообщить, что у неё нет прав на порт платы, и предложит
установить правило доступа. Подтвердите: система один раз запросит пароль администратора. Установка из `.deb` или `PKGBUILD` ставит
это правило автоматически.

Для работы на Linux в системе должны быть udisks2 и polkit (на обычном рабочем столе они уже установлены).
Подробности по пакетам: [docs/DEPENDENCIES_linux.txt](docs/DEPENDENCIES_linux.txt).

## Если что-то не работает

- **Порта платы нет в списке.** Проверьте кабель (нужен кабель с передачей данных, не только для зарядки) и нажмите **Refresh ports**.
- **Linux: «Доступ запрещён» к порту.** Согласитесь на установку правила доступа (см. выше) или добавьте себя в группу `dialout`
  (в Arch и Manjaro `uucp`) и перезайдите в систему.
- **Не находится SD-карта или диск `RPI-RP2`.** Подождите несколько секунд и повторите. Отключите лишние флешки и карты
  и убедитесь, что система вообще показывает диск платы.
- **Windows: порт платы остался в списке после перехода в режим кард-ридера.** Это нормально: Windows не сразу убирает
  устройство из списка. Повторное нажатие **Open SD card >** не отправит команду ещё раз — программа увидит смонтированный
  диск Карабаса и подскажет, как выйти из режима.
- **После прошивки в журнале нет сообщения об извлечении диска.** Так и должно быть: RP2040 перезагружается сам, и диск
  `RPI-RP2` исчезает без участия программы.
- **Скачивание идёт медленно.** Файлы берутся с GitHub; скорость определяется вашим соединением и ничем в программе не регулируется.
- **Журнал для разбора проблем.** Запустите программу с ключом `--log`: журнал запишется в файл
  (Linux `~/.local/share/KarabasFlasher/flasher_log.txt`, Windows `%APPDATA%\KarabasFlasher\flasher_log.txt`,
  macOS `~/Library/Application Support/KarabasFlasher/flasher_log.txt`). Версию программы показывает `--version` и заголовок окна.

## Для разработчиков

Сборка из исходников, устройство проекта, релизы: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). История версий: [CHANGELOG.md](CHANGELOG.md).
Бинарники `7za` для всех трёх платформ входят в репозиторий (`resources/7za/`), отдельно скачивать их не нужно.

## Лицензия

Код распространяется по лицензии **MIT** (файл `LICENSE`). Программа использует Qt 6 по лицензии LGPL-3.0
(подробности в `THIRD_PARTY.md`). Прошивки и ядра, которые программа скачивает, принадлежат их авторам и в проект не входят.

---

## English

[Русский](#русский) | [English](#english)

A tool for updating the **firmware** and **cores** of Karabas Go family boards. Runs on Windows, Linux and macOS.

Previously, updating required manually downloading files from GitHub, putting the board into bootloader mode,
copying the firmware and replacing cores on the SD card. Karabas Flasher does all of this in a single window.

**Supported boards:** Karabas Go, Karabas Go Mini.

### What the program does

- **Downloads** the latest firmware and cores from the board author's GitHub repositories.
- **Copies extra content** (ROMs, disk images, etc.) from the `extra.7z` archive to the SD card. Files that already exist on the card are moved to a `backup_*` folder before being replaced. Free space is checked before copying, and the unpacked archive size is checked before extraction (2 GB limit). Symbolic links from the archive are not copied.
- **Updates the board firmware** (RP2040 microcontroller).
- **Replaces cores** (`.kg1`, `.kg2`, `.kg3` files) on the board's SD card. Old cores are not deleted but moved to a backup folder.
- **Opens the board's SD card** as a regular drive so you can work with its files directly.

Firmware and cores can be updated together or separately — for example, cores only.

> **Important.** Updating cores and direct access to the first memory card appeared in firmware dated 18.10.2026 and newer.
> The ability to use a second memory card (MSC mode 2) was added in firmware dated 27.10.2026.
> Automatic detection of the device model and firmware version appeared in firmware dated 29.10.2026.
> If you are not sure which firmware is installed on your board, update the firmware first — without updating cores.
> Only after the firmware has been updated successfully can you update the cores.

### How to use

1. Connect the Karabas to your computer with a USB cable.
2. Select your model in the **Select Device Model** list, or click the magnifier button — the program will
   query the board version (`AT+KARABAS=version`) and pick the right model automatically.
3. Check what you want to update:
   - **Update firmware**: board firmware (enabled by default);
   - **Update cores**: cores on the SD card;
   - **Download extra content**: extra content (ROMs, disk images, etc.).

   You can check any combination of these.
4. Click **Download via GitHub**. The program downloads everything that is checked. There are many cores, and the speed
   depends on your internet connection; progress is shown in the log at the bottom of the window.
5. In the **Serial port** list, select the board's port. Only USB devices are shown; the required port appears
   when the board is connected (if it is missing, click **Refresh ports**). The "(not selected - flash without reset)"
   item is only needed if the board has already been put into bootloader mode manually.
6. Click **FLASH!** and wait for the "Process completed successfully" line in the log.

Do not disconnect the board or close the window while an operation is in progress. If you do close the window,
the program will ask for confirmation and stop at a safe point.

### Automatic model detection

The magnifier button next to the model list sends the `AT+KARABAS=version` command to the board and waits
up to two seconds for a reply. The reply has the form `+KARABAS:<firmware>;<board>;<core id>;<core build>`,
where `board` is the board identifier: `1` — Karabas Go, `2` — Karabas Go Mini rev A-F, `3` — Karabas Go Mini rev G.
The reply is written to the log, and the matching model is selected in the list automatically.

On Windows the program raises the DTR/RTS lines explicitly after opening the port: the RP2040 USB-CDC device
only transmits data when DTR is asserted, as with a terminal. Without this the board does not answer the
version request.

### Working with the SD card manually

The **Open SD card >** button switches the board into USB card reader mode: the SD card appears in the system
as a regular drive, and the program opens it in the file manager. The **"?"** icon next to the button shows a hint.
To leave this mode, safely eject the card using your system's tools.

The **1 / 2** switch next to the button selects the card: **1** — the main SD card with FPGA cores, **2** — an
additional card for other files. Core updates always use card 1, regardless of the switch position.

If the board is already in card reader mode, pressing **Open SD card >** again does not send the command a second
time: the program sees the mounted Karabas drive and tells you how to leave the mode. The drive is identified by
its contents (`.kg1`/`.kg2`/`.kg3` cores in the root or a `backup_*` folder), so a flash drive inserted by accident
is not mistaken for the board.

### What happens under the hood

**Firmware update.** The program sends a special signal to the board's port (a connection at 1200 baud), after which the
RP2040 reboots into bootloader mode and appears in the system as an `RPI-RP2` drive. The program copies the firmware file
to it, and the board reboots with the new firmware.

**Cores update.** The program switches the board into card reader mode, finds the SD card drive that appears, creates a
`backup_YYYY-MM-DD_HH-MM-SS` folder on it, moves the old cores of the selected model there, and then copies the new ones.
Only release cores (those that are part of the GitHub set) are moved to the backup folder; custom and test cores placed on
the card manually are left in place. After that, the card is safely ejected from the system, and the board returns to
normal mode. If something goes wrong at any step, the changes are rolled back: the old cores are restored.

**Checks.** Before writing, the program verifies that the firmware file is really a UF2 image (including the RP2040
family ID, block numbers and payload sizes in every block), that a complete set of cores has been downloaded, and that
the drive found is the board's drive and not another flash drive. If several suitable drives are connected to the
computer, the program stops and asks you to leave only the board.

**Port errors.** If the port could not be opened or written to (no permission, port busy), the program reports it
immediately and does not wait for a drive to appear. If the port has really disappeared (e.g. the cable was unplugged)
and the `RPI-RP2` drive is already visible, flashing continues without the 1200 bps reset — but only in the branch
without an MSC session, where the port name could not have changed.

### Installation

Ready-made builds are available on the [Releases](https://github.com/xdemax/kgff/releases) page.

| System | What to download | How to run |
|---|---|---|
| **Windows** | `…-windows.zip` archive | Unpack and run `KarabasFlasher.exe`. If Windows shows a SmartScreen warning, click "More info" and "Run anyway" (the build is not signed) |
| **macOS** | `…-macos.dmg` image | Drag the app to "Applications". On first launch, the system may block the unsigned app: open "System Settings", "Privacy & Security", "Open Anyway". Alternatively, run `xattr -dr com.apple.quarantine /Applications/KarabasFlasher.app` in the terminal. Allow access to removable volumes when the system asks |
| **Ubuntu 24.04 and newer** | `…ubuntu24.04.deb` package | `sudo apt install ./file-name.deb`: dependencies are installed automatically |
| **Other Linux** (including Ubuntu 22.04) | `…AppImage` | `chmod +x file.AppImage` and run. If it does not start, install the `libfuse2` package |
| **Arch / Manjaro** | sources from the repository | `cd packaging/arch/git && makepkg -si` |

**Linux: port access.** On first connection, the program may report that it has no permission to access the board's port
and offer to install an access rule. Confirm: the system will ask for the administrator password once. Installing from
`.deb` or `PKGBUILD` installs this rule automatically.

On Linux, udisks2 and polkit must be present (they are already installed on a typical desktop).
Package details: [docs/DEPENDENCIES_linux.txt](docs/DEPENDENCIES_linux.txt).

### Troubleshooting

- **The board's port is not in the list.** Check the cable (a data cable is required, not just a charging one) and click **Refresh ports**.
- **Linux: "Access denied" to the port.** Agree to install the access rule (see above) or add yourself to the `dialout`
  group (in Arch and Manjaro `uucp`) and log in again.
- **The SD card or `RPI-RP2` drive is not found.** Wait a few seconds and try again. Disconnect extra flash drives and cards
  and make sure the system shows the board's drive at all.
- **Windows: the board's port stays in the list after switching to card reader mode.** This is normal: Windows does not
  remove the device from the list immediately. Pressing **Open SD card >** again will not send the command a second time —
  the program sees the mounted Karabas drive and tells you how to leave the mode.
- **There is no "drive ejected" message after flashing.** That is expected: the RP2040 reboots by itself and the
  `RPI-RP2` drive disappears without the program's help.
- **Download is slow.** Files come from GitHub; the speed is determined by your connection and is not regulated by the program.
- **Log for troubleshooting.** Run the program with the `--log` switch: the log will be written to a file
  (Linux `~/.local/share/KarabasFlasher/flasher_log.txt`, Windows `%APPDATA%\KarabasFlasher\flasher_log.txt`,
  macOS `~/Library/Application Support/KarabasFlasher/flasher_log.txt`). The program version is shown by `--version` and the window title.

### For developers

Building from source, project structure, releases: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). Version history: [CHANGELOG.md](CHANGELOG.md).
The `7za` binaries for all three platforms are bundled in the repository (`resources/7za/`); no separate download is needed.

### License

The code is distributed under the **MIT** license (`LICENSE` file). The program uses Qt 6 under the LGPL-3.0 license
(details in `THIRD_PARTY.md`). Firmware and cores downloaded by the program belong to their authors and are not part of the project.
