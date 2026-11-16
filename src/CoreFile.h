#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

// Информация из заголовка ядра kgo2 (.kg).
struct CoreFileInfo {
    QString fileName;
    QString id;
    QString name;
    QString build;      // строка версии, например "26111345"
    int board = 0;      // HW_ID: 1 Go, 2 Mini, 3 Mini G
    int type = -1;      // 0 boot, 1 osd, 2 fileloader, 255 hidden
    bool visible = false;
    int order = 0;
    int link = 1;       // 1 — MCU link v1, 2 — link v2
};

enum class CoreParseResult {
    Ok,
    NotForBoard,        // в файле нет сборки для этой платы (например, pong.kg для Mini)
    Error,
};

// Читает версию и идентификатор ядра из kgo2-файла для указанной платы.
// В одном файле может быть несколько секций CORE — по одной на плату,
// поэтому board обязателен. why заполняется для Error.
CoreParseResult parseCoreFile(const QString& path, int board, CoreFileInfo* info, QString* why);

// Разбирает ответ платы на AT+KARABAS=list:
//   +KARABAS:<file>;<id>;<name>;<build>
//   ...
//   OK
// Возвращает ядра в порядке ответа. Пустой список при OK — на карте нет ядер.
// error заполняется, если пришёл ERROR, нет OK или строка повреждена.
QList<CoreFileInfo> parseCoreListResponse(const QByteArray& raw, int board, QString* error);

// Сравнение версий: <0 — installed старее downloaded, 0 — равны, >0 — installed новее.
// Версии обычно восьмизначные числа; если это не так, сравниваются как строки.
int compareCoreBuilds(const QString& installed, const QString& downloaded);
