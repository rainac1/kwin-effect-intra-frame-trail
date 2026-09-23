/*
    SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
    SPDX-License-Identifier: GPL-2.0-or-later

    Verifies that the built plugin is discoverable exactly the way KWin's
    PluginEffectLoader discovers effects: KPluginMetaData::findPlugins() over the
    "kwin/effects/plugins" namespace. This catches plugin id, namespace and
    embedded-metadata regressions without needing a running compositor.
*/

#include <KPluginMetaData>
#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <qt-plugin-root>\n", argv[0]);
        return 2;
    }

    QCoreApplication::addLibraryPath(QString::fromLocal8Bit(argv[1]));

    const QList<KPluginMetaData> plugins =
        KPluginMetaData::findPlugins(QStringLiteral("kwin/effects/plugins"));

    if (plugins.isEmpty()) {
        std::fprintf(stderr, "FAIL: no effect plugins found under %s/kwin/effects/plugins\n", argv[1]);
        return 1;
    }

    const KPluginMetaData *trail = nullptr;
    int trailCount = 0;
    for (const KPluginMetaData &metadata : plugins) {
        std::printf("found plugin: id=%s name=%s version=%s\n",
                    qPrintable(metadata.pluginId()),
                    qPrintable(metadata.name()),
                    qPrintable(metadata.version()));
        if (metadata.pluginId() == QStringLiteral("trail")) {
            trail = &metadata;
            ++trailCount;
        }
    }

    if (!trail) {
        std::fprintf(stderr, "FAIL: plugin id 'trail' not found\n");
        return 1;
    }
    if (trailCount != 1) {
        std::fprintf(stderr, "FAIL: plugin id 'trail' found %d times\n", trailCount);
        return 1;
    }

    // The id must be what kwinrc [Plugins] trailEnabled refers to. KWin derives
    // it from the file name unless the metadata provides one, so the plugin must
    // be installed as trail.so and not libtrail.so.
    if (trail->pluginId() != QStringLiteral("trail")) {
        std::fprintf(stderr, "FAIL: unexpected plugin id '%s'\n", qPrintable(trail->pluginId()));
        return 1;
    }
    // name() is localized; accept either name declared in metadata.json, which
    // also proves the embedded metadata is ours.
    const QString name = trail->name();
    if (name != QStringLiteral("Pointer Trail") && name != QStringLiteral("指针轨迹")) {
        std::fprintf(stderr, "FAIL: unexpected plugin name '%s'\n", qPrintable(name));
        return 1;
    }
    if (trail->category() != QStringLiteral("Appearance")) {
        std::fprintf(stderr, "FAIL: unexpected plugin category '%s'\n", qPrintable(trail->category()));
        return 1;
    }
    if (trail->isEnabledByDefault()) {
        std::fprintf(stderr, "FAIL: effect must not be enabled by default\n");
        return 1;
    }

    std::printf("plugin 'trail' is discoverable\n");
    return 0;
}
