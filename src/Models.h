#pragma once
#include <QList>
#include <QString>

struct ModelInfo {
    QString name;
    QString repo;
    QString firmwareUrl;
    QString ext;        // расширение файлов ядер: .kg1 / .kg2 / .kg3
};

inline const QList<ModelInfo>& models() {
    // Все прошивки и ядра теперь лежат в одном репозитории karabas-go.
    // Ядра общие для всех моделей и имеют расширение .kg.
    static const QList<ModelInfo> list = {
        { QStringLiteral("Karabas Go"),
          QStringLiteral("karabas-go"),
          QStringLiteral("https://raw.githubusercontent.com/andykarpov/karabas-go/master/firmware/firmware.uf2"),
          QStringLiteral(".kg") },
        { QStringLiteral("Karabas Go Mini rev A-F"),
          QStringLiteral("karabas-go"),
          QStringLiteral("https://raw.githubusercontent.com/andykarpov/karabas-go/master/firmware/karabas_mini.uf2"),
          QStringLiteral(".kg") },
        { QStringLiteral("Karabas Go Mini rev G"),
          QStringLiteral("karabas-go"),
          QStringLiteral("https://raw.githubusercontent.com/andykarpov/karabas-go/master/firmware/karabas_minig.uf2"),
          QStringLiteral(".kg") },
    };
    return list;
}
