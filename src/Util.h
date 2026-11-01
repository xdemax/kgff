#pragma once
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

// Имя файла, которое безопасно подставлять в путь и URL.
bool isSafeFileName(const QString& name);

// Рабочая папка приложения (в кэше пользователя, а не в общем /tmp).
QString workDir();

// Полные пути файлов в папке dir с расширением ext (без учёта регистра).
QStringList listFilesWithExt(const QString& dir, const QString& ext);

// Окружение для запуска системных утилит (udisksctl, gdbus, pkexec...). Внутри AppImage
// переменная LD_LIBRARY_PATH указывает на библиотеки из образа, и они могут сломать системные
// программы, поэтому такие пути из неё убираются. Вне AppImage окружение не меняется.
QProcessEnvironment cleanChildEnvironment();

// То же окружение, но с принудительной C-локалью: для утилит, чей текстовый вывод
// разбирается (udisksctl). Для xdg-open и 7za использовать cleanChildEnvironment().
QProcessEnvironment cleanChildEnvironmentCLocale();

// Открыть папку в файловом менеджере системы (Проводник, Finder, xdg-open).
bool openInFileManager(const QString& path);

// Копирование с fsync: данные гарантированно записаны на носитель.
bool copyFileSynced(const QString& src, const QString& dst, QString* err);

// Признак тома Карабаса в MSC-режиме: в корне есть ядра (.kg1/.kg2/.kg3) или папка
// резервной копии backup_*. Метка тома не используется: у пользователей она может
// быть любой. Нужно, чтобы отличить Карабас от случайно вставленной флешки.
bool isKarabasVolume(const QString& mountPoint);
