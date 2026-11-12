#include "updater.h"
#include "nethost.h"

#include "appdirs.h"

#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

#ifndef DAEDALCAD_VERSION
#define DAEDALCAD_VERSION "0.0.0"
#endif
#ifndef DAEDALCAD_GITHUB_REPO
#define DAEDALCAD_GITHUB_REPO ""
#endif

int compareVersions(const QString& a, const QString& b) {
  const QStringList pa = a.split(QRegularExpression(QStringLiteral("[.-]")));
  const QStringList pb = b.split(QRegularExpression(QStringLiteral("[.-]")));
  for (int i = 0; i < std::max(pa.size(), pb.size()); ++i) {
    const QString x = i < pa.size() ? pa[i] : QString();
    const QString y = i < pb.size() ? pb[i] : QString();
    bool okX = false;
    bool okY = false;
    const qlonglong nx = x.toLongLong(&okX);
    const qlonglong ny = y.toLongLong(&okY);
    if (okX && okY) {
      if (nx != ny) return nx < ny ? -1 : 1;
    } else if (x != y) {
      // "1.0" is newer than "1.0-beta": a missing part beats a text part.
      if (x.isEmpty()) return okY ? -1 : 1;
      if (y.isEmpty()) return okX ? 1 : -1;
      return x < y ? -1 : 1;
    }
  }
  return 0;
}

QString currentAppVersion() { return QStringLiteral(DAEDALCAD_VERSION); }

namespace {

QString githubRepo() { return QStringLiteral(DAEDALCAD_GITHUB_REPO).trimmed(); }

// Where to ask: GitHub's API for the latest release of the repository. DAEDALCAD_GITHUB_API points
// it at a stand-in for api.github.com (for testing).
struct Source {
  QString api;
  QString latestPath;
  QString problem;  // why it cannot check, or empty
};

Source source() {
  Source out;
  QString api = qEnvironmentVariable("DAEDALCAD_GITHUB_API").trimmed();
  while (api.endsWith(QLatin1Char('/'))) api.chop(1);
  out.api = api.isEmpty() ? QStringLiteral("https://api.github.com") : withScheme(api);
  out.latestPath = QStringLiteral("/repos/%1/releases/latest").arg(githubRepo());
  if (githubRepo().isEmpty()) out.problem = QStringLiteral("This build has no GitHub repository to get updates from.");
  return out;
}

QNetworkRequest requestFor(const QUrl& url, const QByteArray& accept) {
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("DaedalCAD/%1").arg(currentAppVersion()));
  request.setRawHeader("Accept", accept);
  request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(30000);
  return request;
}

QString checkError(QNetworkReply* reply) {
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (status == 404) return QStringLiteral("There is no release on GitHub yet, or the repository is not public.");
  if (status == 403 || status == 429)
    return QStringLiteral("GitHub's limit on update checks from this network is reached for now. Try again in an hour.");
  if (status == 0) return QStringLiteral("Could not reach GitHub (%1).").arg(reply->errorString());
  return QStringLiteral("GitHub answered %1.").arg(status);
}

// It ends up in a file name: only what a version looks like (no slashes or dots-only names).
QString checkedVersion(const QString& version) {
  static const QRegularExpression versionShape(QStringLiteral("^[0-9A-Za-z][0-9A-Za-z._-]{0,39}$"));
  if (!versionShape.match(version).hasMatch() || version.contains(QStringLiteral(".."))) return {};
  return version;
}

// Only https, or a stand-in on the local network.
bool safeDownloadUrl(const QString& url) {
  return url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) || isLocalNetworkHost(url);
}

// A GitHub release: the tag is the version, the text the notes, the setup.exe asset the installer
// (only with the SHA-256 GitHub lists for it, so it can be checked).
UpdateInfo parseGithubRelease(const QJsonObject& o) {
  UpdateInfo info;
  QString tag = o.value(QStringLiteral("tag_name")).toString().trimmed();
  if (tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) tag.remove(0, 1);
  info.version = checkedVersion(tag);
  info.notes = o.value(QStringLiteral("body")).toString().trimmed();
  info.added = o.value(QStringLiteral("published_at")).toString().left(10);
  info.pageUrl = o.value(QStringLiteral("html_url")).toString();
  if (!info.pageUrl.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) && !isLocalNetworkHost(info.pageUrl)) info.pageUrl.clear();
  if (info.version.isEmpty()) return info;
  const QString wanted = QStringLiteral("DaedalCAD-%1-setup.exe").arg(info.version);
  QJsonObject installer;
  for (const QJsonValue& value : o.value(QStringLiteral("assets")).toArray()) {
    const QJsonObject asset = value.toObject();
    const QString name = asset.value(QStringLiteral("name")).toString();
    if (name.compare(wanted, Qt::CaseInsensitive) == 0) installer = asset;
    else if (installer.isEmpty() && name.endsWith(QStringLiteral("-setup.exe"), Qt::CaseInsensitive)) installer = asset;
    if (name.endsWith(QStringLiteral("-windows-x64.zip"), Qt::CaseInsensitive)) info.hasZip = true;
  }
  const QString digest = installer.value(QStringLiteral("digest")).toString();
  const QString url = installer.value(QStringLiteral("browser_download_url")).toString();
  static const QRegularExpression sha256Shape(QStringLiteral("^sha256:([0-9a-fA-F]{64})$"));
  const QRegularExpressionMatch match = sha256Shape.match(digest);
  if (match.hasMatch() && safeDownloadUrl(url)) {
    info.hasInstaller = true;
    info.installerSha256 = match.captured(1).toLower();
    info.installerBytes = static_cast<qint64>(installer.value(QStringLiteral("size")).toDouble());
    info.installerUrl = url;
  }
  return info;
}

// Asks for the newest version; calls back on the GUI thread.
void fetchLatest(QObject* context, const Source& src, std::function<void(bool ok, UpdateInfo info, QString error)> done) {
  auto* manager = new QNetworkAccessManager(context);
  QNetworkReply* reply = manager->get(requestFor(QUrl(src.api + src.latestPath), "application/vnd.github+json"));
  QObject::connect(reply, &QNetworkReply::finished, context, [manager, reply, done] {
    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 200) {
      const QJsonDocument json = QJsonDocument::fromJson(body);
      if (json.isObject()) done(true, parseGithubRelease(json.object()), {});
      else done(false, {}, QStringLiteral("GitHub sent something unexpected."));
    } else {
      done(false, {}, checkError(reply));
    }
    reply->deleteLater();
    manager->deleteLater();
  });
}

QString sha256Of(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(&file);
  return QString::fromLatin1(hash.result().toHex());
}

QString mb(qint64 bytes) { return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1); }

class UpdatesDialog : public QDialog {
 public:
  UpdatesDialog(QWidget* parent, const UpdateHooks& hooksIn) : QDialog(parent), hooks(hooksIn) {
    setWindowTitle(QStringLiteral("Updates"));
    resize(560, 460);
    QSettings settings;
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setText(isInstalledCopy()
                       ? QStringLiteral("This is DaedalCAD %1, installed. Updates install themselves; your projects in Documents\\DaedalCAD are not touched.")
                             .arg(currentAppVersion())
                       : QStringLiteral("This is DaedalCAD %1, portable (run from its own folder). It can tell you about new versions; "
                                        "installed copies also update themselves.")
                             .arg(currentAppVersion()));
    layout->addWidget(intro);

    auto* where = new QLabel(QStringLiteral("New versions come from the releases on github.com/%1.").arg(githubRepo().toHtmlEscaped()), this);
    where->setWordWrap(true);
    where->setStyleSheet(QStringLiteral("color: #99a5b3;"));
    layout->addWidget(where);
    atStart = new QCheckBox(QStringLiteral("Check for updates when DaedalCAD starts"), this);
    atStart->setChecked(settings.value(QStringLiteral("updateCheckAtStart"), true).toBool());
    layout->addWidget(atStart);

    auto* checkRow = new QHBoxLayout();
    checkButton = new QPushButton(QStringLiteral("Check now"), this);
    checkRow->addWidget(checkButton);
    checkRow->addStretch(1);
    layout->addLayout(checkRow);
    result = new QLabel(this);
    result->setWordWrap(true);
    result->setTextFormat(Qt::RichText);
    layout->addWidget(result);
    notes = new QPlainTextEdit(this);
    notes->setReadOnly(true);
    notes->hide();
    layout->addWidget(notes, 1);
    progress = new QProgressBar(this);
    progress->hide();
    layout->addWidget(progress);
    auto* buttons = new QHBoxLayout();
    installButton = new QPushButton(isInstalledCopy() ? QStringLiteral("Install update") : QStringLiteral("Open release page"), this);
    installButton->setObjectName(QStringLiteral("updateInstall"));
    installButton->hide();
    skipButton = new QPushButton(QStringLiteral("Skip this version"), this);
    skipButton->hide();
    closeButton = new QPushButton(QStringLiteral("Close"), this);
    buttons->addStretch(1);
    buttons->addWidget(skipButton);
    buttons->addWidget(installButton);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);
    if (!hooks.launch) hooks.launch = [](const QString& program, const QStringList& args) { return QProcess::startDetached(program, args); };
    if (!hooks.quit) hooks.quit = [] { QApplication::quit(); };

    connect(checkButton, &QPushButton::clicked, this, [this] { check(); });
    connect(installButton, &QPushButton::clicked, this, [this] { install(); });
    connect(skipButton, &QPushButton::clicked, this, [this] {
      QSettings().setValue(QStringLiteral("updateSkip"), info.version);
      result->setText(QStringLiteral("DaedalCAD %1 is skipped. The start-up check stays quiet about it; Check now still shows it.").arg(info.version.toHtmlEscaped()));
      installButton->hide();
      skipButton->hide();
    });
    connect(closeButton, &QPushButton::clicked, this, [this] {
      if (downloading) cancelled = true;
      else accept();
    });
    connect(atStart, &QCheckBox::toggled, this, [this] { QSettings().setValue(QStringLiteral("updateCheckAtStart"), atStart->isChecked()); });
  }

  void check(bool thenInstall = false) {
    const Source src = source();
    if (!src.problem.isEmpty()) {
      result->setText(src.problem);
      return;
    }
    checkButton->setEnabled(false);
    result->setText(QStringLiteral("Asking GitHub …"));
    QPointer<UpdatesDialog> self(this);
    fetchLatest(this, src, [self, thenInstall](bool ok, UpdateInfo latest, QString error) {
      if (!self) return;
      self->checkButton->setEnabled(true);
      if (!ok) {
        self->result->setText(error.toHtmlEscaped());
        return;
      }
      self->showResult(latest);
      if (thenInstall && self->installButton->isVisible()) self->install();
    });
  }

 private:
  void showResult(const UpdateInfo& latest) {
    info = latest;
    notes->hide();
    installButton->hide();
    skipButton->hide();
    if (info.version.isEmpty()) {
      result->setText(QStringLiteral("The latest GitHub release has a tag that is not a version (such as v0.9.8)."));
      return;
    }
    if (compareVersions(info.version, currentAppVersion()) <= 0) {
      result->setText(QStringLiteral("You have the newest version (%1).").arg(currentAppVersion().toHtmlEscaped()));
      return;
    }
    QString text = QStringLiteral("<b>DaedalCAD %1 is available</b>%2. You have %3.")
                       .arg(info.version.toHtmlEscaped(), info.added.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(info.added),
                            currentAppVersion().toHtmlEscaped());
    if (isInstalledCopy() && !info.hasInstaller)
      text += QStringLiteral("<br>The release has no DaedalCAD-%1-setup.exe that GitHub lists a checksum for, so it cannot update "
                             "itself. Download it from the release page.")
                  .arg(info.version.toHtmlEscaped());
    result->setText(text);
    if (!info.notes.isEmpty()) {
      notes->setPlainText(info.notes);
      notes->show();
    }
    installButton->setVisible(info.hasInstaller || !info.pageUrl.isEmpty());
    if (isInstalledCopy() && info.hasInstaller) installButton->setText(QStringLiteral("Install update (%1)").arg(mb(info.installerBytes)));
    else installButton->setText(QStringLiteral("Open release page"));
    skipButton->show();
  }

  void install() {
    if (!isInstalledCopy() || !info.hasInstaller) {
      if (!info.pageUrl.isEmpty()) QDesktopServices::openUrl(QUrl(info.pageUrl));
      return;
    }
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString target = QDir(folder).absoluteFilePath(QStringLiteral("DaedalCAD-%1-setup.exe").arg(info.version));
    // Already downloaded and intact: no need to fetch it again.
    if (!(QFileInfo::exists(target) && sha256Of(target).compare(info.installerSha256, Qt::CaseInsensitive) == 0)) {
      if (!download(target)) return;
    }
    if (hooks.beforeQuit && !hooks.beforeQuit()) {
      result->setText(QStringLiteral("Update postponed. The installer is downloaded; press Install update again when you are ready."));
      return;
    }
    const QStringList args{QStringLiteral("/SILENT"), QStringLiteral("/SUPPRESSMSGBOXES"), QStringLiteral("/NORESTART"),
                           QStringLiteral("/CLOSEAPPLICATIONS")};
    if (!hooks.launch(target, args)) {
      result->setText(QStringLiteral("Could not start the installer (%1).").arg(QDir::toNativeSeparators(target).toHtmlEscaped()));
      return;
    }
    result->setText(QStringLiteral("Installing DaedalCAD %1. It starts again by itself.").arg(info.version.toHtmlEscaped()));
    QSettings().setValue(QStringLiteral("updateInstalling"), info.version);
    hooks.quit();
  }

  bool download(const QString& target) {
    const QString partial = target + QStringLiteral(".part");
    QFile out(partial);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      result->setText(QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(partial).toHtmlEscaped()));
      return false;
    }
    QNetworkAccessManager manager;
    // The asset's own address, which redirects to GitHub's file storage. The checksum comes from
    // GitHub too, so only over https (or a stand-in on the local network, for testing).
    if (!safeDownloadUrl(info.installerUrl)) {
      result->setText(QStringLiteral("Updates are only installed over https."));
      return false;
    }
    QNetworkRequest request = requestFor(QUrl(info.installerUrl), "application/octet-stream");
    request.setTransferTimeout(60000);
    QNetworkReply* reply = manager.get(request);
    downloading = true;
    cancelled = false;
    progress->setRange(0, 100);
    progress->setValue(0);
    progress->show();
    installButton->setEnabled(false);
    checkButton->setEnabled(false);
    closeButton->setText(QStringLiteral("Cancel"));
    result->setText(QStringLiteral("Downloading DaedalCAD %1 …").arg(info.version.toHtmlEscaped()));
    QEventLoop loop;
    connect(reply, &QNetworkReply::readyRead, &loop, [&] {
      if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) out.write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, &loop, [&](qint64 got, qint64 total) {
      if (total <= 0) total = info.installerBytes;
      if (total > 0) progress->setValue(static_cast<int>(100 * got / total));
      if (cancelled) reply->abort();
    });
    QTimer poll;
    connect(&poll, &QTimer::timeout, &loop, [&] {
      if (cancelled) reply->abort();
    });
    poll.start(100);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray rest = reply->readAll();
    if (status == 200) out.write(rest);
    out.close();
    downloading = false;
    progress->hide();
    installButton->setEnabled(true);
    checkButton->setEnabled(true);
    closeButton->setText(QStringLiteral("Close"));
    bool ok = false;
    if (cancelled) {
      result->setText(QStringLiteral("Download cancelled."));
    } else if (status != 200 || reply->error() != QNetworkReply::NoError) {
      result->setText(QStringLiteral("The download from GitHub did not work (%1). Try again, or use the release page.")
                          .arg(status ? QString::number(status) : reply->errorString())
                          .toHtmlEscaped());
    } else if (sha256Of(partial).compare(info.installerSha256, Qt::CaseInsensitive) != 0) {
      result->setText(QStringLiteral("The download is damaged (its checksum does not match). Try again."));
    } else {
      QFile::remove(target);
      ok = QFile::rename(partial, target);
      if (!ok) result->setText(QStringLiteral("Could not save the installer in %1.").arg(QDir::toNativeSeparators(target).toHtmlEscaped()));
    }
    if (!ok) QFile::remove(partial);
    reply->deleteLater();
    return ok;
  }

  UpdateHooks hooks;
  UpdateInfo info;
  QCheckBox* atStart = nullptr;
  QPushButton* checkButton = nullptr;
  QLabel* result = nullptr;
  QPlainTextEdit* notes = nullptr;
  QProgressBar* progress = nullptr;
  QPushButton* installButton = nullptr;
  QPushButton* skipButton = nullptr;
  QPushButton* closeButton = nullptr;
  bool downloading = false;
  bool cancelled = false;
};

void openDialog(QWidget* parent, const UpdateHooks& hooks, bool checkNow, bool thenInstall) {
  UpdatesDialog dialog(parent, hooks);
  if (checkNow) QTimer::singleShot(0, &dialog, [&dialog, thenInstall] { dialog.check(thenInstall); });
  dialog.exec();
}

}  // namespace

void openUpdatesDialog(QWidget* parent, const UpdateHooks& hooks) { openDialog(parent, hooks, true, false); }

void checkForUpdatesAtStartup(QWidget* parent, const UpdateHooks& hooks) {
  QSettings settings;
  // Just updated: say so once.
  const QString installing = settings.value(QStringLiteral("updateInstalling")).toString();
  if (!installing.isEmpty()) {
    settings.remove(QStringLiteral("updateInstalling"));
    if (compareVersions(currentAppVersion(), installing) >= 0)
      QMessageBox::information(parent, QStringLiteral("DaedalCAD updated"), QStringLiteral("DaedalCAD is now version %1.").arg(currentAppVersion()));
  }
  // Left over from updates through a DaedalSync server (the password was kept as plain text).
  for (const char* old : {"updateHost", "updateLogin", "updatePassword", "updateSource"}) settings.remove(QLatin1String(old));
  if (!settings.value(QStringLiteral("updateCheckAtStart"), true).toBool()) return;
  const Source src = source();
  if (!src.problem.isEmpty()) return;
  QPointer<QWidget> window(parent);
  fetchLatest(parent, src, [window, hooks](bool ok, UpdateInfo info, QString) {
    if (!window || !ok || info.version.isEmpty()) return;
    if (compareVersions(info.version, currentAppVersion()) <= 0) return;
    if (QSettings().value(QStringLiteral("updateSkip")).toString() == info.version) return;
    if (isInstalledCopy() && !info.hasInstaller) return;
    QMessageBox box(window);
    box.setWindowTitle(QStringLiteral("Update available"));
    box.setIcon(QMessageBox::Information);
    box.setText(QStringLiteral("DaedalCAD %1 is available. You have %2.").arg(info.version, currentAppVersion()));
    if (!info.notes.isEmpty()) box.setInformativeText(info.notes);
    QPushButton* now = box.addButton(isInstalledCopy() ? QStringLiteral("Update now") : QStringLiteral("Show me"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    QPushButton* skip = box.addButton(QStringLiteral("Skip this version"), QMessageBox::DestructiveRole);
    box.setDefaultButton(now);
    box.exec();
    if (box.clickedButton() == skip) {
      QSettings().setValue(QStringLiteral("updateSkip"), info.version);
    } else if (box.clickedButton() == now) {
      openDialog(window, hooks, true, isInstalledCopy());
    }
  });
}
