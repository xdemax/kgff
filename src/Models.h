#pragma once
#include <QList>
#include <QString>

// Репозиторий, где лежат все прошивки и ядра (общие для всех моделей).
inline const QString& coresRepo() {
    static const QString repo = QStringLiteral("karabas-go");
    return repo;
}

// Расширение файлов ядер: одно для всех моделей.
inline const QString& coresExt() {
    static const QString ext = QStringLiteral(".kg");
    return ext;
}

struct ModelInfo {
    QString name;
    QString firmwareUrl;
};

inline const QList<ModelInfo>& models() {
    // Все прошивки и ядра лежат в одном репозитории karabas-go.
    // Ядра общие для всех моделей и имеют расширение .kg.
    static const QList<ModelInfo> list = {
        { QStringLiteral("Karabas Go"),
          QStringLiteral("https://raw.githubusercontent.com/andykarpov/karabas-go/master/firmware/karabas_go.uf2") },
        { QStringLiteral("Karabas Go Mini rev A-F"),
          QStringLiteral("https://raw.githubusercontent.com/andykarpov/karabas-go/master/firmware/karabas_mini.uf2") },
        { QStringLiteral("Karabas Go Mini rev G"),
          QStringLiteral("https://raw.githubusercontent.com/andykarpov/karabas-go/master/firmware/karabas_minig.uf2") },
    };
    return list;
}
