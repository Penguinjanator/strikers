#include "apppaths.h"

#include "inifile.h"
#include "port/disc.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>

namespace {

const char* const kExampleName = "strikers.ini.example";
const char* const kIniName = "strikers.ini";

// The two sentences below end up in the Play button's tooltip, so they are player-facing and want a
// context of their own rather than QObject's.
class Text
{
    Q_DECLARE_TR_FUNCTIONS(AppPaths)
};

} // namespace

QString AppPaths::archiveRootFor(const QString& applicationDirPath)
{
    QString dir = QDir::fromNativeSeparators(applicationDirPath);
    while (dir.size() > 1 && dir.endsWith(QLatin1Char('/')))
        dir.chop(1);

#if defined(Q_OS_MACOS)
    // Only the bundle shape, and only exactly it: a folder that happens to be called MacOS is not a
    // bundle, and stripping three levels off one would send every path here somewhere arbitrary.
    const QString suffix = QStringLiteral("/Contents/MacOS");
    if (dir.endsWith(suffix))
    {
        dir.chop(suffix.size());       // ... /strikers-settings.app
        const int slash = dir.lastIndexOf(QLatin1Char('/'));
        if (slash > 0)
            dir = dir.left(slash);     // ... /   <- the archive root
    }
#endif

    return dir;
}

QString AppPaths::archiveRoot()
{
    return archiveRootFor(QCoreApplication::applicationDirPath());
}

QString AppPaths::defaultIniPath()
{
    return archiveRoot() + QLatin1Char('/') + QLatin1String(kIniName);
}

QString AppPaths::findExample(const QString& iniPath, const QString& archiveRoot)
{
    QStringList dirs;
    const QString beside = QFileInfo(iniPath).absolutePath();
    if (!beside.isEmpty())
        dirs << beside;
    if (!archiveRoot.isEmpty() && !dirs.contains(archiveRoot))
        dirs << archiveRoot;

    for (const QString& dir : dirs)
    {
        const QString candidate = dir + QLatin1Char('/') + QLatin1String(kExampleName);
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return QString();
}

QString AppPaths::findGame(const QString& archiveRoot, QString* reason)
{
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("strikers.exe");
#else
    const QString name = QStringLiteral("strikers");
#endif
    const QString path = archiveRoot + QLatin1Char('/') + name;
    const QFileInfo info(path);
    if (!info.exists())
    {
        if (reason != nullptr)
            *reason = Text::tr("%1 is not in %2.")
                          .arg(name, QDir::toNativeSeparators(archiveRoot));
        return QString();
    }
    if (!info.isExecutable())
    {
        if (reason != nullptr)
            *reason = Text::tr("%1 is not executable.")
                          .arg(QDir::toNativeSeparators(path));
        return QString();
    }
    return path;
}

QString AppPaths::userFolder(const QString& configured)
{
    QString dir = QDir::fromNativeSeparators(configured.trimmed());
    if (dir.isEmpty())
    {
        const QString app = QStringLiteral("Super Mario Strikers");
#if defined(Q_OS_WIN)
        dir = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA")) + QLatin1Char('/') + app;
#elif defined(Q_OS_MACOS)
        dir = QDir::homePath() + QStringLiteral("/Library/Application Support/") + app;
#else
        QString base = qEnvironmentVariable("XDG_DATA_HOME");
        if (base.isEmpty())
            base = QDir::homePath() + QStringLiteral("/.local/share");
        dir = base + QLatin1Char('/') + app;
#endif
    }
    while (dir.size() > 1 && dir.endsWith(QLatin1Char('/')))
        dir.chop(1);
    return dir;
}

QString AppPaths::portableUserFolder() { return archiveRoot() + QStringLiteral("/user"); }

bool AppPaths::iniIsPortable(const QString& iniPath)
{
    IniFile ini;
    if (!QFileInfo::exists(iniPath) || !ini.load(iniPath, nullptr))
        return false;
    const QString v = ini.value(QStringLiteral("PORTABLE")).trimmed().toLower();
    return !v.isEmpty() && v != QLatin1String("0") && v != QLatin1String("false") &&
           v != QLatin1String("no") && v != QLatin1String("off");
}

void AppPaths::keepAppSettings(bool portable)
{
    // Every QSettings() constructed after this uses the format set here.
    if (portable)
    {
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, portableUserFolder());
        QSettings::setDefaultFormat(QSettings::IniFormat);
    }
    else
        QSettings::setDefaultFormat(QSettings::NativeFormat);
}

bool AppPaths::moveToPortable(const QString& from, const QString& root, QString* error)
{
    // A game folder inside the one being moved would be carried into its own user/.
    if (movedPath(root, from) != root)
    {
        if (error != nullptr)
            *error = QDir::toNativeSeparators(root);
        return false;
    }

    const QDir src(from);
    QStringList files;
    QStringList dirs;
    QDirIterator it(from, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        (it.fileInfo().isDir() ? dirs : files) << path;
    }

    for (const QString& path : files)
    {
        const QString rel = src.relativeFilePath(path);
        if (QFileInfo(rel).fileName() == QLatin1String(".DS_Store"))
        {
            QFile::remove(path);
            continue;
        }
        const bool cache = rel.startsWith(QLatin1String("dawn_cache.db")) ||
                           rel.startsWith(QLatin1String("pipeline_cache.db"));
        const QString to =
            root + (cache ? QStringLiteral("/cache/") : QStringLiteral("/user/")) + rel;
        if (QFileInfo::exists(to))
            continue;
        // QFile::rename copies and removes when the two are on different drives.
        if (!QDir().mkpath(QFileInfo(to).absolutePath()) || !QFile::rename(path, to))
        {
            if (error != nullptr)
                *error = QDir::toNativeSeparators(path);
            return false;
        }
    }

    // Deepest first, so a parent is empty by the time it is reached.
    std::sort(dirs.begin(), dirs.end(),
              [](const QString& a, const QString& b) { return a.size() > b.size(); });
    for (const QString& dir : dirs)
        QDir().rmdir(dir);
    QDir().rmdir(from);
    return true;
}

QString AppPaths::movedPath(const QString& path, const QString& from)
{
    const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const QString f = QDir::cleanPath(QDir::fromNativeSeparators(from));
#if defined(Q_OS_WIN)
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    if (p.compare(f, cs) == 0)
        return QStringLiteral("user");
    if (!p.startsWith(f + QLatin1Char('/'), cs))
        return path;
    return QStringLiteral("user") + p.mid(f.size());
}

QString AppPaths::modsFolder(const QString& root, const QString& kind)
{
    return root + QStringLiteral("/mods/") + kind;
}

QString AppPaths::findDataBesideGame(const QString& archiveRoot)
{
    const QDir root(archiveRoot);
    if (QFileInfo::exists(root.filePath(QStringLiteral("files/common.ini"))))
        return root.filePath(QStringLiteral("files"));

    const QStringList names = root.entryList(QDir::Files | QDir::NoDotAndDotDot,
                                             QDir::Name | QDir::IgnoreCase);
    for (const QString& name : names)
    {
        if (!name.startsWith(QLatin1Char('.'))
            && port_disc_looks_like_image(QFile::encodeName(name).constData()))
            return root.filePath(name);
    }
    return QString();
}
