// Where the archive is, as opposed to where this executable is.

#pragma once

#include <QString>
#include <QStringList>

namespace AppPaths {

// The archive root for a given executable directory.
QString archiveRootFor(const QString& applicationDirPath);

// archiveRootFor(QCoreApplication::applicationDirPath()).
QString archiveRoot();

// The strikers.ini the application edits when no path was given on the command line: strikers.ini
// in the archive root.
QString defaultIniPath();

// The strikers.ini.example to seed a new file from, or an empty string if there is none.
QString findExample(const QString& iniPath, const QString& archiveRoot);

// The game executable in the archive root, or an empty string with `reason` filled in.
QString findGame(const QString& archiveRoot, QString* reason);

QString findDataBesideGame(const QString& archiveRoot);

// The game's user folder: SDL's preference path for "Super Mario Strikers", or user_dir from strikers.ini.
QString userFolder(const QString& configured = QString());

// The game's user folder when portable = 1.
QString portableUserFolder();

bool iniIsPortable(const QString& iniPath);

// Window size and window language go to an ini in portableUserFolder(), or the system default.
void keepAppSettings(bool portable);

// Moves `from` into user/ and cache/ under `root`, leaving behind any file already there.
bool moveToPortable(const QString& from, const QString& root, QString* error);

// mods/<kind> under `root` (the archive root or the user folder), matching the game's PortModsFolder.
QString modsFolder(const QString& root, const QString& kind);

} // namespace AppPaths
