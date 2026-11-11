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
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
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
#include <QUrlQuery>
#include <QVBoxLayout>

#include <memory>

#ifndef DAEDALCAD_VERSION
#define DAEDALCAD_VERSION "0.0.0"
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

QString hostUrl(QString host) {
  host = host.trimmed();
  while (host.endsWith(QLatin1Char('/'))) host.chop(1);
  if (host.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) host.chop(3);
  while (host.endsWith(QLatin1Char('/'))) host.chop(1);
  if (host.isEmpty()) return {};
  return withScheme(host);
}

struct Server {
  QString host;
  QByteArray auth;
  QString latestPath;
  QString downloadPath;
  QString how;      // shown in the window
  QString problem;  // why it cannot check, or empty
};

// Where to ask, and how to sign in: the update host (or else the CloudSync host); the CloudSync key
// when there is one, otherwise the download login.
Server server() {
  QSettings settings;
  Server out;
  out.host = hostUrl(settings.value(QStringLiteral("updateHost")).toString());
  if (out.host.isEmpty()) out.host = hostUrl(settings.value(QStringLiteral("cloudHost")).toString());
  const QString key = settings.value(QStringLiteral("cloudKey")).toString().trimmed();
  const QString login = settings.value(QStringLiteral("updateLogin")).toString().trimmed();
  const QString password = settings.value(QStringLiteral("updatePassword")).toString();
  if (out.host.isEmpty()) {
    out.problem = QStringLiteral("Fill in the server, or set up CloudSync.");
  } else if (!key.isEmpty()) {
    out.auth = QByteArray("Bearer ") + key.toUtf8();
    out.latestPath = QStringLiteral("/v1/app/latest");
    out.downloadPath = QStringLiteral("/v1/app/download");
    out.how = QStringLiteral("with your CloudSync key");
  } else if (!login.isEmpty()) {
    out.auth = QByteArray("Basic ") + (login + QLatin1Char(':') + password).toUtf8().toBase64();
    out.latestPath = QStringLiteral("/downloads/latest");
    out.downloadPath = QStringLiteral("/download");
    out.how = QStringLiteral("with the download login %1").arg(login);
  } else {
    out.problem = QStringLiteral("Fill in a download login, or set up CloudSync with your key.");
  }
  return out;
}

QNetworkRequest requestFor(const Server& s, const QString& path, const QUrlQuery& query = {}) {
  QUrl url(s.host + path);
  url.setQuery(query);
  QNetworkRequest request(url);
  request.setRawHeader("Authorization", s.auth);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(30000);
  return request;
}

QString serverError(QNetworkReply* reply, const QByteArray& body) {
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QString message = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toString();
  if (status == 401) return QStringLiteral("The server did not accept the key or login.");
  if (status == 429) return QStringLiteral("Too many wrong passwords. Try again later.");
  if (!message.isEmpty()) return message;
  if (status == 0) return QStringLiteral("Could not reach the server (%1).").arg(reply->errorString());
  return QStringLiteral("The server answered %1.").arg(status);
}

UpdateInfo parseInfo(const QJsonObject& o) {
  UpdateInfo info;
  info.version = o.value(QStringLiteral("version")).toString();
  // It ends up in a file name: only what a version looks like (no slashes or dots-only names).
  static const QRegularExpression versionShape(QStringLiteral("^[0-9A-Za-z][0-9A-Za-z._-]{0,39}$"));
  if (!versionShape.match(info.version).hasMatch() || info.version.contains(QStringLiteral(".."))) info.version.clear();
  info.notes = o.value(QStringLiteral("notes")).toString();
  info.added = o.value(QStringLiteral("added")).toString().left(10);
  const QJsonObject installer = o.value(QStringLiteral("installer")).toObject();
  info.hasInstaller = !installer.isEmpty();
  info.installerBytes = static_cast<qint64>(installer.value(QStringLiteral("bytes")).toDouble());
  info.installerSha256 = installer.value(QStringLiteral("sha256")).toString();
  info.hasZip = !o.value(QStringLiteral("zip")).toObject().isEmpty();
  return info;
}

// Asks for the newest version; calls back on the GUI thread.
void fetchLatest(QObject* context, const Server& s, std::function<void(bool ok, UpdateInfo info, QString error)> done) {
  auto* manager = new QNetworkAccessManager(context);
  QNetworkReply* reply = manager->get(requestFor(s, s.latestPath));
  QObject::connect(reply, &QNetworkReply::finished, context, [manager, reply, done] {
    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 200) {
      const QJsonDocument json = QJsonDocument::fromJson(body);
      if (json.isObject()) done(true, parseInfo(json.object()), {});
      else done(false, {}, QStringLiteral("The server sent something unexpected."));
    } else {
      done(false, {}, serverError(reply, body));
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

    auto* form = new QFormLayout();
    hostEdit = new QLineEdit(settings.value(QStringLiteral("updateHost")).toString(), this);
    hostEdit->setPlaceholderText(settings.value(QStringLiteral("cloudHost")).toString().isEmpty()
                                     ? QStringLiteral("https://your-server")
                                     : QStringLiteral("same as CloudSync (%1)").arg(settings.value(QStringLiteral("cloudHost")).toString()));
    loginEdit = new QLineEdit(settings.value(QStringLiteral("updateLogin")).toString(), this);
    passwordEdit = new QLineEdit(settings.value(QStringLiteral("updatePassword")).toString(), this);
    passwordEdit->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Server"), hostEdit);
    keyNote = new QLabel(this);
    keyNote->setWordWrap(true);
    keyNote->setStyleSheet(QStringLiteral("color: #99a5b3;"));
    form->addRow(QString(), keyNote);
    form->addRow(QStringLiteral("Download login"), loginEdit);
    form->addRow(QStringLiteral("Password"), passwordEdit);
    layout->addLayout(form);
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
    installButton = new QPushButton(isInstalledCopy() ? QStringLiteral("Install update") : QStringLiteral("Open download page"), this);
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
    for (QLineEdit* edit : {hostEdit, loginEdit, passwordEdit}) connect(edit, &QLineEdit::textEdited, this, [this] { save(); });
    connect(atStart, &QCheckBox::toggled, this, [this] { save(); });
    save();
  }

  void check(bool thenInstall = false) {
    save();
    const Server s = server();
    if (!s.problem.isEmpty()) {
      result->setText(s.problem);
      return;
    }
    checkButton->setEnabled(false);
    result->setText(QStringLiteral("Asking %1 …").arg(s.host.toHtmlEscaped()));
    QPointer<UpdatesDialog> self(this);
    fetchLatest(this, s, [self, thenInstall](bool ok, UpdateInfo latest, QString error) {
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
  void save() {
    QSettings settings;
    settings.setValue(QStringLiteral("updateHost"), hostEdit->text().trimmed());
    settings.setValue(QStringLiteral("updateLogin"), loginEdit->text().trimmed());
    settings.setValue(QStringLiteral("updatePassword"), passwordEdit->text());
    settings.setValue(QStringLiteral("updateCheckAtStart"), atStart->isChecked());
    const bool key = !settings.value(QStringLiteral("cloudKey")).toString().trimmed().isEmpty();
    keyNote->setText(key ? QStringLiteral("Signs in with your CloudSync key; no download login needed.")
                         : QStringLiteral("No CloudSync key here: sign in with a download login (the name and password for the downloads page)."));
    loginEdit->setEnabled(!key);
    passwordEdit->setEnabled(!key);
  }

  void showResult(const UpdateInfo& latest) {
    info = latest;
    notes->hide();
    installButton->hide();
    skipButton->hide();
    if (info.version.isEmpty()) {
      result->setText(QStringLiteral("The server has no versions yet."));
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
      text += QStringLiteral("<br>There is no installer for it on the server yet, so it cannot update itself. Upload the setup.exe on the admin page.");
    result->setText(text);
    if (!info.notes.isEmpty()) {
      notes->setPlainText(info.notes);
      notes->show();
    }
    installButton->setVisible(!isInstalledCopy() || info.hasInstaller);
    if (isInstalledCopy() && info.hasInstaller) installButton->setText(QStringLiteral("Install update (%1)").arg(mb(info.installerBytes)));
    skipButton->show();
  }

  void install() {
    const Server s = server();
    if (!isInstalledCopy()) {
      QDesktopServices::openUrl(QUrl(s.host + QStringLiteral("/downloads")));
      return;
    }
    // The checksum comes from the same server as the installer, so over plain http anyone on the
    // way could swap both. Only run installers fetched over https, or from the local network.
    if (!s.host.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) && !isLocalNetworkHost(s.host)) {
      result->setText(QStringLiteral("Updates are only installed over https. Change the server address to https://… "
                                     "(your proxy's SSL address), or download the installer from the downloads page."));
      return;
    }
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString target = QDir(folder).absoluteFilePath(QStringLiteral("DaedalCAD-%1-setup.exe").arg(info.version));
    // Already downloaded and intact: no need to fetch it again.
    if (!(QFileInfo::exists(target) && sha256Of(target).compare(info.installerSha256, Qt::CaseInsensitive) == 0)) {
      if (!download(s, target)) return;
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

  bool download(const Server& s, const QString& target) {
    const QString partial = target + QStringLiteral(".part");
    QFile out(partial);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      result->setText(QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(partial).toHtmlEscaped()));
      return false;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("version"), info.version);
    query.addQueryItem(QStringLiteral("kind"), QStringLiteral("installer"));
    QNetworkAccessManager manager;
    QNetworkRequest request = requestFor(s, s.downloadPath, query);
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
      result->setText(serverError(reply, rest).toHtmlEscaped());
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
  QLineEdit* hostEdit = nullptr;
  QLineEdit* loginEdit = nullptr;
  QLineEdit* passwordEdit = nullptr;
  QLabel* keyNote = nullptr;
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
  if (!settings.value(QStringLiteral("updateCheckAtStart"), true).toBool()) return;
  const Server s = server();
  if (!s.problem.isEmpty()) return;  // not set up: stay quiet
  QPointer<QWidget> window(parent);
  fetchLatest(parent, s, [window, hooks](bool ok, UpdateInfo info, QString) {
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
