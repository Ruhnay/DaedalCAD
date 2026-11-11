#include "cloudsync.h"
#include "appdirs.h"
#include "nethost.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace {

// ---------------------------------------------------------------------------------------------------
// Small helpers

QString prettyBytes(qint64 bytes) {
  if (bytes < 1024) return QString::number(bytes) + QStringLiteral(" B");
  double value = static_cast<double>(bytes);
  const char* units[] = {"KiB", "MiB", "GiB", "TiB"};
  int unit = -1;
  do {
    value /= 1024.0;
    ++unit;
  } while (value >= 1024.0 && unit < 3);
  return QString::number(value, 'f', value >= 10.0 ? 0 : 1) + QLatin1Char(' ') + QString::fromLatin1(units[unit]);
}

QString niceDate(const QString& iso) {
  const QDateTime when = QDateTime::fromString(iso, Qt::ISODateWithMs);
  if (!when.isValid()) return iso;
  return when.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString cloudHost(QString host) {
  host = host.trimmed();
  while (host.endsWith(QLatin1Char('/'))) host.chop(1);
  if (host.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) host.chop(3);
  while (host.endsWith(QLatin1Char('/'))) host.chop(1);
  if (host.isEmpty()) return {};
  return withScheme(host);
}

QString rememberedHash(const QString& host, const QString& name) {
  QSettings settings;
  const QJsonObject all = QJsonDocument::fromJson(settings.value(QStringLiteral("cloudSyncHashes")).toByteArray()).object();
  return all.value(host).toObject().value(name).toString();
}

void rememberHash(const QString& host, const QString& name, const QString& sha) {
  QSettings settings;
  QJsonObject all = QJsonDocument::fromJson(settings.value(QStringLiteral("cloudSyncHashes")).toByteArray()).object();
  QJsonObject one = all.value(host).toObject();
  if (sha.isEmpty()) one.remove(name);
  else one.insert(name, sha);
  all.insert(host, one);
  settings.setValue(QStringLiteral("cloudSyncHashes"), QJsonDocument(all).toJson(QJsonDocument::Compact));
}

// The same cleaning the main window applies to project names, so folder names match.
QString cleanProjectName(QString name) {
  name = name.trimmed();
  name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
  name.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
  if (name == QLatin1String(".") || name == QLatin1String("..")) return {};
  return name;
}

QByteArray pngBytes(const QImage& image) {
  if (image.isNull()) return {};
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}

QString sha256Hex(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// Which update requests and decisions the start-up popup has already shown, per server.
QSet<QString> seenIds(const QString& host, const QString& kind) {
  QSettings settings;
  const QJsonObject all = QJsonDocument::fromJson(settings.value(QStringLiteral("cloudSyncSeen")).toByteArray()).object();
  QSet<QString> out;
  for (const QJsonValue& value : all.value(host).toObject().value(kind).toArray()) out.insert(value.toString());
  return out;
}

void markSeen(const QString& host, const QString& kind, const QStringList& ids) {
  if (ids.isEmpty()) return;
  QSettings settings;
  QJsonObject all = QJsonDocument::fromJson(settings.value(QStringLiteral("cloudSyncSeen")).toByteArray()).object();
  QJsonObject one = all.value(host).toObject();
  QJsonArray list = one.value(kind).toArray();
  QSet<QString> have;
  for (const QJsonValue& value : list) have.insert(value.toString());
  for (const QString& id : ids)
    if (!have.contains(id)) list.append(id);
  while (list.size() > 2000) list.removeFirst();
  one.insert(kind, list);
  all.insert(host, one);
  settings.setValue(QStringLiteral("cloudSyncSeen"), QJsonDocument(all).toJson(QJsonDocument::Compact));
}

bool cloudEnabled() {
  QSettings settings;
  const bool configured = !settings.value(QStringLiteral("cloudHost")).toString().trimmed().isEmpty() &&
                          !settings.value(QStringLiteral("cloudKey")).toString().trimmed().isEmpty();
  return configured && settings.value(QStringLiteral("cloudEnabled"), true).toBool();
}

// ---------------------------------------------------------------------------------------------------
// The link between a local project and a public one: <project>/cloudsync.json (never uploaded).

struct PublicLink {
  QString folder;  // local project folder
  QString name;    // local project name
  QString host;
  QString publicId;
  QString title;
  int baseVersion = 0;
  QString role;  // "owner" or "update"
};

bool readLink(const QString& folder, PublicLink* link) {
  QFile file(QDir(folder).absoluteFilePath(QStringLiteral("cloudsync.json")));
  if (!file.open(QIODevice::ReadOnly)) return false;
  const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
  link->folder = folder;
  link->name = QFileInfo(folder).fileName();
  link->host = obj.value(QStringLiteral("host")).toString();
  link->publicId = obj.value(QStringLiteral("publicId")).toString();
  link->title = obj.value(QStringLiteral("title")).toString();
  link->baseVersion = obj.value(QStringLiteral("baseVersion")).toInt();
  link->role = obj.value(QStringLiteral("role")).toString();
  return !link->publicId.isEmpty();
}

bool writeLink(const PublicLink& link) {
  QJsonObject obj;
  obj.insert(QStringLiteral("host"), link.host);
  obj.insert(QStringLiteral("publicId"), link.publicId);
  obj.insert(QStringLiteral("title"), link.title);
  obj.insert(QStringLiteral("baseVersion"), link.baseVersion);
  obj.insert(QStringLiteral("role"), link.role);
  QSaveFile file(QDir(link.folder).absoluteFilePath(QStringLiteral("cloudsync.json")));
  if (!file.open(QIODevice::WriteOnly)) return false;
  file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
  return file.commit();
}

// ---------------------------------------------------------------------------------------------------
// Blocking calls for the window (the UI keeps painting while one runs).

struct Reply {
  int status = 0;
  QByteArray body;
  QString error;
  QByteArray sha256;
  int version = 0;
  bool ok() const { return error.isEmpty() && status >= 200 && status < 300; }
  QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

class Net {
 public:
  QString host;
  QString key;

  Reply call(const QByteArray& method, const QString& path, const QByteArray& body = {}, const QByteArray& type = {}) {
    QNetworkRequest request(QUrl(host + path));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + key.toUtf8());
    request.setTransferTimeout(180000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!type.isEmpty()) request.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(type));
    QNetworkReply* reply = nullptr;
    if (method == "GET") reply = manager.get(request);
    else if (method == "PUT") reply = manager.put(request, body);
    else if (method == "POST") reply = manager.post(request, body);
    else if (method == "DELETE") reply = manager.deleteResource(request);
    else reply = manager.sendCustomRequest(request, method, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    Reply out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body = reply->readAll();
    out.sha256 = reply->rawHeader("X-Sha256").trimmed();
    out.version = reply->rawHeader("X-Version").toInt();
    if (reply->error() != QNetworkReply::NoError && out.status == 0) {
      out.error = reply->errorString();
    } else if (out.status >= 400) {
      out.error = QJsonDocument::fromJson(out.body).object().value(QStringLiteral("error")).toString();
      if (out.error.isEmpty()) out.error = QStringLiteral("Server answered %1").arg(out.status);
    }
    reply->deleteLater();
    return out;
  }
  Reply json(const QByteArray& method, const QString& path, const QJsonObject& body) {
    return call(method, path, QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json");
  }
  static QString enc(const QString& text) { return QString::fromUtf8(QUrl::toPercentEncoding(text)); }

 private:
  QNetworkAccessManager manager;
};

QLabel* previewLabel(QWidget* parent, const QSize& size) {
  auto* label = new QLabel(QStringLiteral("No preview"), parent);
  label->setFixedSize(size);
  label->setAlignment(Qt::AlignCenter);
  label->setStyleSheet(QStringLiteral("QLabel { background: #12151a; color: #6f7883; border: 1px solid #2a3340; }"));
  return label;
}

void showImage(QLabel* label, const QByteArray& png) {
  QPixmap pixmap;
  if (png.isEmpty() || !pixmap.loadFromData(png, "PNG")) {
    label->setPixmap(QPixmap());
    label->setText(QStringLiteral("No preview"));
    return;
  }
  label->setText(QString());
  label->setPixmap(pixmap.scaled(label->size() - QSize(2, 2), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

// ---------------------------------------------------------------------------------------------------

class CloudSyncWindow : public QDialog {
 public:
  CloudSyncWindow(QWidget* parent, const CloudSyncHooks& hooks) : QDialog(parent), hooks(hooks) {
    setWindowTitle(QStringLiteral("CloudSync"));
    resize(1100, 780);
    auto* layout = new QVBoxLayout(this);
    QSettings settings;

    hostEdit = new QLineEdit(settings.value(QStringLiteral("cloudHost")).toString(), this);
    hostEdit->setPlaceholderText(QStringLiteral("https://sync.example.com"));
    keyEdit = new QLineEdit(settings.value(QStringLiteral("cloudKey")).toString(), this);
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setPlaceholderText(QStringLiteral("User key"));
    auto* showKey = new QCheckBox(QStringLiteral("Show"), this);
    connect(showKey, &QCheckBox::toggled, this, [this](bool on) { keyEdit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
    enabledBox = new QCheckBox(QStringLiteral("Enable CloudSync: check for update requests when DaedalCAD starts"), this);
    enabledBox->setChecked(settings.value(QStringLiteral("cloudEnabled"), true).toBool());
    auto* form = new QFormLayout();
    form->addRow(QStringLiteral("Host"), hostEdit);
    auto* keyRow = new QHBoxLayout();
    keyRow->addWidget(keyEdit, 1);
    keyRow->addWidget(showKey);
    form->addRow(QStringLiteral("User key"), keyRow);
    form->addRow(QString(), enabledBox);
    layout->addLayout(form);

    account = new QLabel(QStringLiteral("Not connected."), this);
    account->setWordWrap(true);
    auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    auto* accountRow = new QHBoxLayout();
    accountRow->addWidget(account, 1);
    accountRow->addWidget(refreshButton);
    layout->addLayout(accountRow);

    tabs = new QTabWidget(this);
    tabs->addTab(buildPersonal(), QStringLiteral("Personal"));
    tabs->addTab(buildPublic(), QStringLiteral("Public"));
    tabs->addTab(buildRequests(), QStringLiteral("Update requests"));
    layout->addWidget(tabs, 1);

    statusLine = new QLabel(QStringLiteral("Nothing is sent until you press a button."), this);
    statusLine->setWordWrap(true);
    statusLine->setObjectName(QStringLiteral("cloudStatus"));
    layout->addWidget(statusLine);
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    layout->addWidget(close, 0, Qt::AlignRight);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(refreshButton, &QPushButton::clicked, this, [this] { refreshCurrent(true); });
    connect(tabs, &QTabWidget::currentChanged, this, [this] { refreshCurrent(false); });
    connect(enabledBox, &QCheckBox::toggled, this, [this] { saveFields(); });
    // A new host or key is checked straight away (shortly after typing or pasting stops), so an
    // error from the previous key does not stay on screen.
    auto* recheck = new QTimer(this);
    recheck->setSingleShot(true);
    recheck->setInterval(700);
    auto credentialsChanged = [this, recheck] {
      std::fill(std::begin(loaded), std::end(loaded), false);
      imageCache.clear();
      account->setText(QStringLiteral("Checking…"));
      recheck->start();
    };
    connect(hostEdit, &QLineEdit::textChanged, this, credentialsChanged);
    connect(keyEdit, &QLineEdit::textChanged, this, credentialsChanged);
    connect(recheck, &QTimer::timeout, this, [this, recheck] {
      if (!tabs->isEnabled()) {  // something is still running; try again in a moment
        recheck->start();
        return;
      }
      if (hostEdit->text().trimmed().isEmpty() || keyEdit->text().trimmed().isEmpty()) {
        account->setText(QStringLiteral("Not connected."));
        return;
      }
      refreshCurrent(true);
    });
  }

  void start(int page) {
    fillLocal();
    tabs->setCurrentIndex(std::clamp(page, 0, 2));
    if (loggedIn()) refreshCurrent(true);
  }

  ~CloudSyncWindow() override { saveFields(); }

 private:
  // --- shared -----------------------------------------------------------------------------------
  CloudSyncHooks hooks;
  Net net;
  QLineEdit* hostEdit = nullptr;
  QLineEdit* keyEdit = nullptr;
  QCheckBox* enabledBox = nullptr;
  QLabel* account = nullptr;
  QLabel* statusLine = nullptr;
  QTabWidget* tabs = nullptr;
  bool loaded[3] = {false, false, false};
  std::map<QString, QByteArray> imageCache;

  struct Busy {
    explicit Busy(CloudSyncWindow* w) : window(w) {
      window->tabs->setEnabled(false);
      QApplication::setOverrideCursor(Qt::WaitCursor);
    }
    ~Busy() {
      QApplication::restoreOverrideCursor();
      window->tabs->setEnabled(true);
    }
    CloudSyncWindow* window;
  };

  void say(const QString& text) {
    statusLine->setText(text);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  }
  void saveFields() {
    QSettings store;
    store.setValue(QStringLiteral("cloudHost"), hostEdit->text().trimmed());
    store.setValue(QStringLiteral("cloudKey"), keyEdit->text().trimmed());
    store.setValue(QStringLiteral("cloudEnabled"), enabledBox->isChecked());
  }
  bool loggedIn() {
    saveFields();
    net.host = cloudHost(hostEdit->text());
    net.key = keyEdit->text().trimmed();
    if (net.host.isEmpty() || net.key.isEmpty()) {
      statusLine->setText(QStringLiteral("Enter the host and the user key."));
      return false;
    }
    return true;
  }
  QString projectsRoot() const { return hooks.appdataPath(QStringLiteral("projects")); }
  QString folderFor(const QString& name) const { return QDir(projectsRoot()).absoluteFilePath(name); }
  QStringList localNames() const {
    QStringList names;
    for (const QFileInfo& info : QDir(projectsRoot()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
      if (!projectFileIn(info.filePath()).isEmpty()) names << info.fileName();
    return names;
  }
  QString uniqueName(const QString& wanted) const {
    QString base = cleanProjectName(wanted);
    if (base.isEmpty()) base = QStringLiteral("Project");
    QString name = base;
    for (int n = 2; QFileInfo::exists(folderFor(name)); ++n) name = QStringLiteral("%1 %2").arg(base).arg(n);
    return name;
  }
  std::vector<PublicLink> links() const {
    std::vector<PublicLink> out;
    for (const QString& name : localNames()) {
      PublicLink link;
      if (readLink(folderFor(name), &link)) out.push_back(link);
    }
    return out;
  }
  // Saves the open project first when it is the one being sent.
  bool saveIfOpen(const QString& folder) {
    const QString current = hooks.currentProjectFolder ? hooks.currentProjectFolder() : QString();
    if (!current.isEmpty() && QDir::cleanPath(current) == QDir::cleanPath(folder)) return hooks.saveCurrent();
    return true;
  }
  bool packWithPreview(const QString& folder, QByteArray* zip, QByteArray* png, QString* error) {
    if (!saveIfOpen(folder)) {
      *error = QStringLiteral("the open project was not saved");
      return false;
    }
    *zip = hooks.packFolder(folder, error);
    if (zip->isEmpty()) return false;
    say(QStringLiteral("Drawing a preview…"));
    *png = pngBytes(hooks.renderPreview ? hooks.renderPreview(folder) : QImage());
    return true;
  }
  QByteArray image(const QString& path) {
    auto it = imageCache.find(path);
    if (it != imageCache.end()) return it->second;
    const Reply reply = net.call("GET", path);
    const QByteArray png = reply.ok() ? reply.body : QByteArray();
    imageCache[path] = png;
    return png;
  }
  // Downloads a zip (checked against its hash) and installs it as a new local project.
  bool installDownload(const Reply& reply, const QString& name, bool openAfter, QString* error) {
    if (!reply.ok() || reply.body.isEmpty()) {
      *error = reply.error.isEmpty() ? QStringLiteral("download failed") : reply.error;
      return false;
    }
    if (!reply.sha256.isEmpty() && sha256Hex(reply.body).compare(QString::fromLatin1(reply.sha256), Qt::CaseInsensitive) != 0) {
      *error = QStringLiteral("the download did not match the server's checksum");
      return false;
    }
    QTemporaryFile tmp;
    if (!tmp.open()) {
      *error = QStringLiteral("could not write a temporary file");
      return false;
    }
    tmp.write(reply.body);
    tmp.close();
    return hooks.installZip(tmp.fileName(), name, openAfter, error);
  }
  void refreshAccount() {
    const Reply me = net.call("GET", QStringLiteral("/v1/me"));
    if (me.ok()) {
      const QJsonObject obj = me.json();
      QString text = QStringLiteral("%1  ·  %2 of %3")
                         .arg(obj.value(QStringLiteral("name")).toString(),
                              prettyBytes(static_cast<qint64>(obj.value(QStringLiteral("bytesUsed")).toDouble())),
                              prettyBytes(static_cast<qint64>(obj.value(QStringLiteral("quotaBytes")).toDouble())));
      const int open = obj.value(QStringLiteral("openIncoming")).toInt();
      if (open > 0) text += QStringLiteral("  ·  %1 update request%2 waiting for you").arg(open).arg(open == 1 ? QString() : QStringLiteral("s"));
      account->setText(text);
      userName = obj.value(QStringLiteral("name")).toString();
    } else {
      account->setText(me.status == 401 ? QStringLiteral("The server did not accept this user key.") : me.error);
    }
  }
  void refreshCurrent(bool force) {
    const int page = tabs->currentIndex();
    if (page == 0) fillLocal();
    if (!force && loaded[page]) return;
    if (!loggedIn()) return;
    Busy busy(this);
    say(QStringLiteral("Reading the server…"));
    refreshAccount();
    if (page == 0) refreshPersonal();
    else if (page == 1) refreshPublic();
    else refreshRequests();
    loaded[page] = true;
  }
  QString userName;

  // --- Personal ---------------------------------------------------------------------------------
  QListWidget* localList = nullptr;
  QListWidget* remoteList = nullptr;
  QLabel* remotePreview = nullptr;

  QWidget* buildPersonal() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* columns = new QHBoxLayout();
    auto* localColumn = new QVBoxLayout();
    auto* remoteColumn = new QVBoxLayout();
    localColumn->addWidget(new QLabel(QStringLiteral("On this machine"), page));
    remoteColumn->addWidget(new QLabel(QStringLiteral("On the server (only you can see these)"), page));
    localList = new QListWidget(page);
    remoteList = new QListWidget(page);
    localList->setObjectName(QStringLiteral("cloudLocal"));
    remoteList->setObjectName(QStringLiteral("cloudRemote"));
    localColumn->addWidget(localList, 1);
    remoteColumn->addWidget(remoteList, 1);
    columns->addLayout(localColumn, 1);
    columns->addLayout(remoteColumn, 1);
    auto* previewColumn = new QVBoxLayout();
    remotePreview = previewLabel(page, QSize(300, 225));
    previewColumn->addWidget(remotePreview);
    previewColumn->addStretch(1);
    columns->addLayout(previewColumn);
    layout->addLayout(columns, 1);

    auto* put = new QPushButton(QStringLiteral("Put"), page);
    auto* putAll = new QPushButton(QStringLiteral("Put all"), page);
    auto* fetch = new QPushButton(QStringLiteral("Fetch"), page);
    auto* fetchAll = new QPushButton(QStringLiteral("Fetch all"), page);
    auto* remove = new QPushButton(QStringLiteral("Delete from server…"), page);
    put->setToolTip(QStringLiteral("Send the selected local project."));
    putAll->setToolTip(QStringLiteral("Send every local project."));
    fetch->setToolTip(QStringLiteral("Download the selected server project."));
    fetchAll->setToolTip(QStringLiteral("Download every server project that is missing or changed."));
    remove->setToolTip(QStringLiteral("Delete the selected project from the server. The copy on this machine stays."));
    auto* row = new QHBoxLayout();
    row->addWidget(put);
    row->addWidget(putAll);
    row->addStretch(1);
    row->addWidget(fetch);
    row->addWidget(fetchAll);
    row->addSpacing(16);
    row->addWidget(remove);
    layout->addLayout(row);

    connect(put, &QPushButton::clicked, this, [this] {
      if (localList->currentItem()) putNames({localList->currentItem()->text()});
      else say(QStringLiteral("Select a local project."));
    });
    connect(putAll, &QPushButton::clicked, this, [this] { putNames(localNames()); });
    connect(fetch, &QPushButton::clicked, this, [this] {
      if (QListWidgetItem* item = remoteList->currentItem())
        fetchNames({{item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toString()}});
      else say(QStringLiteral("Select a server project."));
    });
    connect(fetchAll, &QPushButton::clicked, this, [this] {
      std::vector<std::pair<QString, QString>> all;
      for (int i = 0; i < remoteList->count(); ++i)
        all.push_back({remoteList->item(i)->data(Qt::UserRole).toString(), remoteList->item(i)->data(Qt::UserRole + 1).toString()});
      fetchNames(all);
    });
    connect(remove, &QPushButton::clicked, this, [this] { deleteRemote(); });
    connect(remoteList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
      if (!item || !item->data(Qt::UserRole + 2).toBool()) {
        showImage(remotePreview, {});
        return;
      }
      showImage(remotePreview, image(QStringLiteral("/v1/projects/%1/preview").arg(Net::enc(item->data(Qt::UserRole).toString()))));
    });
    return page;
  }

  void fillLocal() {
    if (!localList) return;
    const QString selected = localList->currentItem() ? localList->currentItem()->text() : QString();
    localList->clear();
    for (const QString& name : localNames()) {
      auto* item = new QListWidgetItem(name, localList);
      PublicLink link;
      if (readLink(folderFor(name), &link))
        item->setToolTip(link.role == QLatin1String("owner") ? QStringLiteral("Published as “%1”").arg(link.title)
                                                             : QStringLiteral("Copy for an update to “%1” (from version %2)").arg(link.title).arg(link.baseVersion));
      if (name == selected) localList->setCurrentItem(item);
    }
  }

  void refreshPersonal() {
    const Reply reply = net.call("GET", QStringLiteral("/v1/projects"));
    if (!reply.ok()) {
      say(reply.error);
      return;
    }
    const QString selected = remoteList->currentItem() ? remoteList->currentItem()->data(Qt::UserRole).toString() : QString();
    imageCache.clear();
    remoteList->clear();
    const QJsonArray rows = reply.json().value(QStringLiteral("projects")).toArray();
    for (const QJsonValue& value : rows) {
      const QJsonObject obj = value.toObject();
      const QString name = obj.value(QStringLiteral("name")).toString();
      auto* item = new QListWidgetItem(QStringLiteral("%1  ·  %2  ·  %3")
                                           .arg(name, prettyBytes(static_cast<qint64>(obj.value(QStringLiteral("bytes")).toDouble())),
                                                niceDate(obj.value(QStringLiteral("updated")).toString())),
                                       remoteList);
      item->setData(Qt::UserRole, name);
      item->setData(Qt::UserRole + 1, obj.value(QStringLiteral("sha256")).toString());
      item->setData(Qt::UserRole + 2, obj.value(QStringLiteral("hasPreview")).toBool());
      if (name == selected) remoteList->setCurrentItem(item);
    }
    say(QStringLiteral("%1 local, %2 on the server.").arg(localList->count()).arg(rows.size()));
  }

  void putNames(const QStringList& names) {
    if (names.isEmpty() || !loggedIn()) return;
    Busy busy(this);
    say(QStringLiteral("Reading the server…"));
    const Reply list = net.call("GET", QStringLiteral("/v1/projects"));
    if (!list.ok()) {
      say(list.error);
      return;
    }
    std::map<QString, QString> remote;
    for (const QJsonValue& value : list.json().value(QStringLiteral("projects")).toArray())
      remote[value.toObject().value(QStringLiteral("name")).toString()] = value.toObject().value(QStringLiteral("sha256")).toString();
    int sent = 0, skipped = 0;
    QStringList left, failed;
    for (const QString& name : names) {
      say(QStringLiteral("Packing %1…").arg(name));
      QByteArray zip, png;
      QString error;
      if (!packWithPreview(folderFor(name), &zip, &png, &error)) {
        (error.contains(QStringLiteral("not saved")) ? left : failed) << (error.isEmpty() ? name : name + QStringLiteral(" (") + error + QLatin1Char(')'));
        continue;
      }
      const QString localSha = sha256Hex(zip);
      const QString remoteSha = remote.count(name) ? remote[name] : QString();
      const QString last = rememberedHash(net.host, name);
      if (!remoteSha.isEmpty() && localSha.compare(remoteSha, Qt::CaseInsensitive) == 0) {
        rememberHash(net.host, name, localSha);
        ++skipped;
        continue;
      }
      if (!remoteSha.isEmpty() && remoteSha.compare(last, Qt::CaseInsensitive) != 0) {
        if (QMessageBox::question(this, QStringLiteral("CloudSync"),
                                  QStringLiteral("“%1” on the server is different from what this machine last sent or fetched. Replace the server copy?").arg(name)) !=
            QMessageBox::Yes) {
          left << name;
          continue;
        }
      }
      say(QStringLiteral("Sending %1…").arg(name));
      const Reply reply = net.call("PUT", QStringLiteral("/v1/projects/") + Net::enc(name), zip, "application/zip");
      if (!reply.ok()) {
        failed << name + QStringLiteral(": ") + reply.error;
        continue;
      }
      if (!png.isEmpty()) net.call("PUT", QStringLiteral("/v1/projects/%1/preview").arg(Net::enc(name)), png, "image/png");
      const QString stored = reply.json().value(QStringLiteral("sha256")).toString();
      rememberHash(net.host, name, stored.isEmpty() ? localSha : stored);
      ++sent;
    }
    refreshPersonal();
    QStringList parts{QStringLiteral("Sent %1").arg(sent)};
    if (skipped) parts << QStringLiteral("skipped %1 unchanged").arg(skipped);
    if (!left.isEmpty()) parts << QStringLiteral("left %1").arg(left.join(QStringLiteral(", ")));
    if (!failed.isEmpty()) parts << failed.join(QStringLiteral("; "));
    say(parts.join(QStringLiteral(". ")) + QLatin1Char('.'));
    hooks.status(statusLine->text());
  }

  void fetchNames(const std::vector<std::pair<QString, QString>>& chosen) {
    if (chosen.empty() || !loggedIn()) return;
    Busy busy(this);
    int fetched = 0, skipped = 0;
    QStringList left, failed;
    for (const auto& [name, sha] : chosen) {
      const QString folder = folderFor(name);
      if (!projectFileIn(folder).isEmpty()) {
        say(QStringLiteral("Checking %1…").arg(name));
        if (!saveIfOpen(folder)) {
          left << name;
          continue;
        }
        QString error;
        const QByteArray zip = hooks.packFolder(folder, &error);
        if (!zip.isEmpty() && sha256Hex(zip).compare(sha, Qt::CaseInsensitive) == 0) {
          rememberHash(net.host, name, sha);
          ++skipped;
          continue;
        }
        if (QMessageBox::question(this, QStringLiteral("CloudSync"),
                                  QStringLiteral("“%1” on this machine is different. Replace the local copy?").arg(name)) != QMessageBox::Yes) {
          left << name;
          continue;
        }
      }
      say(QStringLiteral("Fetching %1…").arg(name));
      const Reply reply = net.call("GET", QStringLiteral("/v1/projects/") + Net::enc(name));
      QString error;
      if (!installDownload(reply, name, chosen.size() == 1, &error)) {
        failed << name + QStringLiteral(": ") + error;
        continue;
      }
      rememberHash(net.host, name, reply.sha256.isEmpty() ? sha256Hex(reply.body) : QString::fromLatin1(reply.sha256));
      ++fetched;
    }
    fillLocal();
    QStringList parts{QStringLiteral("Fetched %1").arg(fetched)};
    if (skipped) parts << QStringLiteral("skipped %1 unchanged").arg(skipped);
    if (!left.isEmpty()) parts << QStringLiteral("left %1").arg(left.join(QStringLiteral(", ")));
    if (!failed.isEmpty()) parts << failed.join(QStringLiteral("; "));
    say(parts.join(QStringLiteral(". ")) + QLatin1Char('.'));
    hooks.status(statusLine->text());
  }

  void deleteRemote() {
    QListWidgetItem* item = remoteList->currentItem();
    if (!item) {
      say(QStringLiteral("Select a server project to delete."));
      return;
    }
    if (!loggedIn()) return;
    const QString name = item->data(Qt::UserRole).toString();
    if (QMessageBox::warning(this, QStringLiteral("Delete from server"),
                             QStringLiteral("Delete “%1” from the server?\n\nThe copy on this machine is not touched. This cannot be undone.").arg(name),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
      return;
    Busy busy(this);
    const Reply reply = net.call("DELETE", QStringLiteral("/v1/projects/") + Net::enc(name));
    if (!reply.ok()) {
      say(reply.error);
      return;
    }
    rememberHash(net.host, name, QString());
    refreshAccount();
    refreshPersonal();
    say(QStringLiteral("Deleted “%1” from the server.").arg(name));
  }

  // --- Public -----------------------------------------------------------------------------------
  QLineEdit* searchEdit = nullptr;
  QListWidget* publicList = nullptr;
  QLabel* publicPreview = nullptr;
  QLabel* publicInfo = nullptr;
  QPushButton* copyLocal = nullptr;
  QPushButton* copyUpdate = nullptr;
  QPushButton* bringUp = nullptr;
  QPushButton* sendRequest = nullptr;
  QPushButton* newVersion = nullptr;
  QPushButton* editDetails = nullptr;
  QPushButton* deletePublic = nullptr;
  QJsonObject selectedPublic;

  QWidget* buildPublic() {
    auto* page = new QWidget(this);
    auto* layout = new QHBoxLayout(page);
    auto* left = new QVBoxLayout();
    searchEdit = new QLineEdit(page);
    searchEdit->setPlaceholderText(QStringLiteral("Search title, author, description or tags"));
    searchEdit->setClearButtonEnabled(true);
    publicList = new QListWidget(page);
    publicList->setObjectName(QStringLiteral("cloudPublic"));
    publicList->setIconSize(QSize(96, 72));
    publicList->setSpacing(2);
    auto* publish = new QPushButton(QStringLiteral("Publish a local project…"), page);
    publish->setToolTip(QStringLiteral("Share one of your local projects in the public list. You stay its author."));
    left->addWidget(searchEdit);
    left->addWidget(publicList, 1);
    left->addWidget(publish);
    layout->addLayout(left, 2);

    auto* right = new QVBoxLayout();
    publicPreview = previewLabel(page, QSize(360, 270));
    publicInfo = new QLabel(QStringLiteral("Select a project."), page);
    publicInfo->setWordWrap(true);
    publicInfo->setTextFormat(Qt::RichText);
    publicInfo->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    publicInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* infoScroll = new QScrollArea(page);
    infoScroll->setWidgetResizable(true);
    infoScroll->setFrameShape(QFrame::NoFrame);
    infoScroll->setWidget(publicInfo);
    copyLocal = new QPushButton(QStringLiteral("Copy project locally"), page);
    copyLocal->setToolTip(QStringLiteral("Download a plain copy. Nothing you change goes back."));
    copyUpdate = new QPushButton(QStringLiteral("Copy project for update"), page);
    copyUpdate->setToolTip(QStringLiteral("Download a copy that is linked to this project, so you can send your changes to the author as an update request."));
    bringUp = new QPushButton(QStringLiteral("Bring my linked copy up to date"), page);
    bringUp->setToolTip(QStringLiteral("Replace your linked local copy with the current version."));
    sendRequest = new QPushButton(QStringLiteral("Send update request…"), page);
    sendRequest->setToolTip(QStringLiteral("Send your linked copy to the author, who accepts or declines it."));
    newVersion = new QPushButton(QStringLiteral("Publish new version"), page);
    newVersion->setToolTip(QStringLiteral("You are the author: send your linked local project as the next version."));
    editDetails = new QPushButton(QStringLiteral("Edit details…"), page);
    deletePublic = new QPushButton(QStringLiteral("Delete from public…"), page);
    // Box rows, so hidden buttons (owner vs. not) leave no gaps.
    auto* rowCopy = new QHBoxLayout();
    rowCopy->addWidget(copyLocal);
    rowCopy->addWidget(copyUpdate);
    rowCopy->addWidget(bringUp);
    auto* rowSend = new QHBoxLayout();
    rowSend->addWidget(sendRequest);
    rowSend->addWidget(newVersion);
    rowSend->addWidget(editDetails);
    rowSend->addWidget(deletePublic);
    right->addWidget(publicPreview, 0, Qt::AlignHCenter);
    right->addWidget(infoScroll, 1);
    right->addLayout(rowCopy);
    right->addLayout(rowSend);
    layout->addLayout(right, 3);

    auto* searchTimer = new QTimer(page);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(400);
    connect(searchEdit, &QLineEdit::textChanged, searchTimer, qOverload<>(&QTimer::start));
    connect(searchTimer, &QTimer::timeout, this, [this] {
      if (!loggedIn()) return;
      Busy busy(this);
      refreshPublic();
    });
    connect(publicList, &QListWidget::currentItemChanged, this, [this] { showPublicSelection(); });
    connect(publish, &QPushButton::clicked, this, [this] { publishProject(); });
    connect(copyLocal, &QPushButton::clicked, this, [this] { copyPublic(false); });
    connect(copyUpdate, &QPushButton::clicked, this, [this] { copyPublic(true); });
    connect(bringUp, &QPushButton::clicked, this, [this] { bringLinkedUpToDate(); });
    connect(sendRequest, &QPushButton::clicked, this, [this] { sendUpdateRequest(); });
    connect(newVersion, &QPushButton::clicked, this, [this] { publishNewVersion(); });
    connect(editDetails, &QPushButton::clicked, this, [this] { editPublicDetails(); });
    connect(deletePublic, &QPushButton::clicked, this, [this] { removePublic(); });
    showPublicSelection();
    return page;
  }

  // Linked local copies of a public project on this server.
  std::vector<PublicLink> linksFor(const QString& publicId, const QString& role = QString()) const {
    std::vector<PublicLink> out;
    for (const PublicLink& link : links())
      if (link.publicId == publicId && cloudHost(link.host) == net.host && (role.isEmpty() || link.role == role)) out.push_back(link);
    return out;
  }

  void refreshPublic() {
    const QString selected = publicList->currentItem() ? publicList->currentItem()->data(Qt::UserRole).toString() : QString();
    QUrlQuery query;
    if (!searchEdit->text().trimmed().isEmpty()) query.addQueryItem(QStringLiteral("q"), searchEdit->text().trimmed());
    const Reply reply = net.call("GET", QStringLiteral("/v1/public") + (query.isEmpty() ? QString() : QStringLiteral("?") + query.toString(QUrl::FullyEncoded)));
    if (!reply.ok()) {
      say(reply.error);
      return;
    }
    publicList->clear();
    const QJsonArray rows = reply.json().value(QStringLiteral("projects")).toArray();
    for (const QJsonValue& value : rows) {
      const QJsonObject obj = value.toObject();
      const bool mine = obj.value(QStringLiteral("mine")).toBool();
      QString line2 = QStringLiteral("by %1 · v%2 · %3")
                          .arg(obj.value(QStringLiteral("author")).toString())
                          .arg(obj.value(QStringLiteral("version")).toInt())
                          .arg(niceDate(obj.value(QStringLiteral("updated")).toString()));
      const int open = obj.value(QStringLiteral("openRequests")).toInt();
      if (mine && open > 0) line2 += QStringLiteral(" · %1 request%2 waiting").arg(open).arg(open == 1 ? QString() : QStringLiteral("s"));
      auto* item = new QListWidgetItem(obj.value(QStringLiteral("title")).toString() + QLatin1Char('\n') + line2, publicList);
      item->setData(Qt::UserRole, obj.value(QStringLiteral("id")).toString());
      item->setData(Qt::UserRole + 1, obj);
      item->setSizeHint(QSize(0, 80));
      if (mine) item->setForeground(QColor(232, 116, 0));
      if (obj.value(QStringLiteral("id")).toString() == selected) publicList->setCurrentItem(item);
    }
    say(rows.isEmpty() ? QStringLiteral("No public projects yet. Publish one of yours to start the list.")
                       : QStringLiteral("%1 public project%2.").arg(rows.size()).arg(rows.size() == 1 ? QString() : QStringLiteral("s")));
    // Thumbnails, one by one so the list is usable straight away.
    for (int i = 0; i < publicList->count(); ++i) {
      QListWidgetItem* item = publicList->item(i);
      const QJsonObject obj = item->data(Qt::UserRole + 1).toJsonObject();
      const QByteArray png = image(QStringLiteral("/v1/public/%1/preview?version=%2").arg(obj.value(QStringLiteral("id")).toString()).arg(obj.value(QStringLiteral("version")).toInt()));
      QPixmap pixmap;
      if (!png.isEmpty() && pixmap.loadFromData(png, "PNG")) item->setIcon(QIcon(pixmap.scaled(96, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
      if (i % 4 == 3) QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
  }

  void showPublicSelection() {
    QListWidgetItem* item = publicList ? publicList->currentItem() : nullptr;
    selectedPublic = item ? item->data(Qt::UserRole + 1).toJsonObject() : QJsonObject();
    const bool any = !selectedPublic.isEmpty();
    const bool mine = selectedPublic.value(QStringLiteral("mine")).toBool();
    const QString id = selectedPublic.value(QStringLiteral("id")).toString();
    const int version = selectedPublic.value(QStringLiteral("version")).toInt();
    const std::vector<PublicLink> updateLinks = any ? linksFor(id, QStringLiteral("update")) : std::vector<PublicLink>{};
    const std::vector<PublicLink> ownerLinks = any ? linksFor(id, QStringLiteral("owner")) : std::vector<PublicLink>{};
    const std::vector<PublicLink> anyLinks = any ? linksFor(id) : std::vector<PublicLink>{};
    copyLocal->setEnabled(any);
    copyUpdate->setEnabled(any && !mine);
    sendRequest->setEnabled(any && !mine && !updateLinks.empty());
    bool behind = false;
    for (const PublicLink& link : anyLinks) behind = behind || link.baseVersion < version;
    bringUp->setEnabled(any && behind);
    newVersion->setEnabled(any && mine && !ownerLinks.empty());
    editDetails->setEnabled(any && mine);
    deletePublic->setEnabled(any && mine);
    copyUpdate->setVisible(!mine);
    sendRequest->setVisible(!mine);
    newVersion->setVisible(mine);
    editDetails->setVisible(mine);
    deletePublic->setVisible(mine);
    if (!any) {
      showImage(publicPreview, {});
      publicInfo->setText(QStringLiteral("Select a project to see it here."));
      return;
    }
    showImage(publicPreview, image(QStringLiteral("/v1/public/%1/preview?version=%2").arg(id).arg(version)));
    QStringList tags;
    for (const QJsonValue& tag : selectedPublic.value(QStringLiteral("tags")).toArray()) tags << tag.toString();
    QString html = QStringLiteral("<h3 style='margin:0'>%1</h3><p style='color:#99a5b3'>by <b>%2</b> · version %3 · updated %4 · %5 · %6 download%7</p>")
                       .arg(selectedPublic.value(QStringLiteral("title")).toString().toHtmlEscaped(),
                            selectedPublic.value(QStringLiteral("author")).toString().toHtmlEscaped())
                       .arg(version)
                       .arg(niceDate(selectedPublic.value(QStringLiteral("updated")).toString()),
                            prettyBytes(static_cast<qint64>(selectedPublic.value(QStringLiteral("bytes")).toDouble())))
                       .arg(selectedPublic.value(QStringLiteral("downloads")).toInt())
                       .arg(selectedPublic.value(QStringLiteral("downloads")).toInt() == 1 ? QString() : QStringLiteral("s"));
    if (!tags.isEmpty()) html += QStringLiteral("<p>Tags: %1</p>").arg(tags.join(QStringLiteral(", ")).toHtmlEscaped());
    const QString description = selectedPublic.value(QStringLiteral("description")).toString();
    if (!description.isEmpty()) html += QStringLiteral("<p>%1</p>").arg(description.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    if (!anyLinks.empty()) {
      QStringList lines;
      for (const PublicLink& link : anyLinks)
        lines << QStringLiteral("“%1” (%2, from version %3%4)")
                     .arg(link.name.toHtmlEscaped(), link.role == QLatin1String("owner") ? QStringLiteral("your published copy") : QStringLiteral("copy for update"))
                     .arg(link.baseVersion)
                     .arg(link.baseVersion < version ? QStringLiteral(", <b>behind</b>") : QString());
      html += QStringLiteral("<p><b>On this machine:</b> %1</p>").arg(lines.join(QStringLiteral("; ")));
    } else if (!mine) {
      html += QStringLiteral("<p style='color:#99a5b3'>To suggest a change: Copy project for update, edit it, then Send update request.</p>");
    }
    publicInfo->setText(html);
    // Version history, read on demand.
    const Reply details = net.call("GET", QStringLiteral("/v1/public/") + id);
    if (details.ok() && publicList->currentItem() == item) {
      QStringList rows;
      for (const QJsonValue& value : details.json().value(QStringLiteral("versions")).toArray()) {
        const QJsonObject v = value.toObject();
        rows.prepend(QStringLiteral("v%1 · %2 · %3%4")
                         .arg(v.value(QStringLiteral("version")).toInt())
                         .arg(niceDate(v.value(QStringLiteral("created")).toString()), v.value(QStringLiteral("author")).toString().toHtmlEscaped(),
                              v.value(QStringLiteral("fromRequest")).isNull() ? QString() : QStringLiteral(" (update request)")));
      }
      if (!rows.isEmpty()) publicInfo->setText(html + QStringLiteral("<p><b>Versions</b><br>%1</p>").arg(rows.join(QStringLiteral("<br>"))));
    }
  }

  struct PublishForm {
    QString local;
    QString title;
    QString description;
    QString tags;
  };
  // Title, description and tags, plus (when publishing) which local project.
  bool askDetails(const QString& heading, PublishForm* form, bool chooseLocal) {
    QDialog dialog(this);
    dialog.setWindowTitle(heading);
    dialog.resize(520, 420);
    auto* layout = new QFormLayout(&dialog);
    auto* local = new QComboBox(&dialog);
    if (chooseLocal) {
      for (const QString& name : localNames()) local->addItem(name);
      const QString current = hooks.currentProjectFolder ? QFileInfo(hooks.currentProjectFolder()).fileName() : QString();
      if (local->findText(current) >= 0) local->setCurrentText(current);
      layout->addRow(QStringLiteral("Local project"), local);
    }
    auto* title = new QLineEdit(form->title, &dialog);
    title->setMaxLength(120);
    auto* description = new QPlainTextEdit(form->description, &dialog);
    auto* tags = new QLineEdit(form->tags, &dialog);
    tags->setPlaceholderText(QStringLiteral("comma separated, e.g. raspberry pi, case"));
    layout->addRow(QStringLiteral("Title"), title);
    layout->addRow(QStringLiteral("Description"), description);
    layout->addRow(QStringLiteral("Tags"), tags);
    if (chooseLocal) {
      auto* note = new QLabel(QStringLiteral("Everyone with an account on this server can see and copy it. Changes by others come to you as update requests."), &dialog);
      note->setWordWrap(true);
      layout->addRow(note);
      if (title->text().isEmpty()) title->setText(local->currentText());
      connect(local, &QComboBox::currentTextChanged, &dialog, [title, local](const QString&) {
        if (title->text().isEmpty() || title->property("auto").toBool()) {
          title->setText(local->currentText());
          title->setProperty("auto", true);
        }
      });
      title->setProperty("auto", true);
      connect(title, &QLineEdit::textEdited, &dialog, [title] { title->setProperty("auto", false); });
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
    form->local = local->currentText();
    form->title = title->text().trimmed();
    form->description = description->toPlainText().trimmed();
    form->tags = tags->text();
    if (form->title.isEmpty() || (chooseLocal && form->local.isEmpty())) {
      say(chooseLocal && form->local.isEmpty() ? QStringLiteral("There is no local project to publish.") : QStringLiteral("A title is required."));
      return false;
    }
    return true;
  }

  static QJsonArray tagArray(const QString& text) {
    QJsonArray tags;
    for (const QString& tag : text.split(QLatin1Char(','), Qt::SkipEmptyParts))
      if (!tag.trimmed().isEmpty()) tags.append(tag.trimmed());
    return tags;
  }

  void publishProject() {
    if (!loggedIn()) return;
    PublishForm form;
    if (!askDetails(QStringLiteral("Publish a local project"), &form, true)) return;
    const QString folder = folderFor(form.local);
    PublicLink existing;
    if (readLink(folder, &existing) && existing.role == QLatin1String("owner") && cloudHost(existing.host) == net.host) {
      if (QMessageBox::question(this, QStringLiteral("Publish"),
                                QStringLiteral("“%1” is already published as “%2”. Publish it again as a separate project?").arg(form.local, existing.title)) !=
          QMessageBox::Yes)
        return;
    }
    Busy busy(this);
    say(QStringLiteral("Packing %1…").arg(form.local));
    QByteArray zip, png;
    QString error;
    if (!packWithPreview(folder, &zip, &png, &error)) {
      say(QStringLiteral("Could not pack %1: %2").arg(form.local, error));
      return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("title"), form.title);
    body.insert(QStringLiteral("description"), form.description);
    body.insert(QStringLiteral("tags"), tagArray(form.tags));
    say(QStringLiteral("Publishing…"));
    const Reply created = net.json("POST", QStringLiteral("/v1/public"), body);
    if (!created.ok()) {
      say(created.error);
      return;
    }
    const QString id = created.json().value(QStringLiteral("id")).toString();
    const Reply file = net.call("PUT", QStringLiteral("/v1/public/%1/file").arg(id), zip, "application/zip");
    if (!file.ok()) {
      net.call("DELETE", QStringLiteral("/v1/public/") + id);
      say(QStringLiteral("Could not send the project: %1").arg(file.error));
      return;
    }
    if (!png.isEmpty()) net.call("PUT", QStringLiteral("/v1/public/%1/preview").arg(id), png, "image/png");
    PublicLink link;
    link.folder = folder;
    link.host = net.host;
    link.publicId = id;
    link.title = form.title;
    link.baseVersion = file.json().value(QStringLiteral("version")).toInt(1);
    link.role = QStringLiteral("owner");
    writeLink(link);
    refreshAccount();
    refreshPublic();
    selectPublic(id);
    say(QStringLiteral("Published “%1”. Others can copy it now; their changes come to you as update requests.").arg(form.title));
    hooks.status(statusLine->text());
  }

  void selectPublic(const QString& id) {
    for (int i = 0; i < publicList->count(); ++i)
      if (publicList->item(i)->data(Qt::UserRole).toString() == id) publicList->setCurrentRow(i);
  }

  void copyPublic(bool forUpdate) {
    if (selectedPublic.isEmpty() || !loggedIn()) return;
    const QString id = selectedPublic.value(QStringLiteral("id")).toString();
    const QString title = selectedPublic.value(QStringLiteral("title")).toString();
    const QString name = uniqueName(forUpdate ? title + QStringLiteral(" (update)") : title);
    Busy busy(this);
    say(QStringLiteral("Downloading “%1”…").arg(title));
    const Reply reply = net.call("GET", QStringLiteral("/v1/public/%1/file").arg(id));
    QString error;
    if (!installDownload(reply, name, true, &error)) {
      say(QStringLiteral("Could not copy “%1”: %2").arg(title, error));
      return;
    }
    if (forUpdate) {
      PublicLink link;
      link.folder = folderFor(name);
      link.host = net.host;
      link.publicId = id;
      link.title = title;
      link.baseVersion = reply.version > 0 ? reply.version : selectedPublic.value(QStringLiteral("version")).toInt();
      link.role = QStringLiteral("update");
      writeLink(link);
    }
    fillLocal();
    showPublicSelection();
    say(forUpdate ? QStringLiteral("Copied to “%1” and opened it. Make your changes, save, then come back and press Send update request.").arg(name)
                  : QStringLiteral("Copied to “%1” and opened it. It is your own copy now.").arg(name));
    hooks.status(statusLine->text());
  }

  // Picks one linked copy (asks when there are several).
  bool chooseLink(const std::vector<PublicLink>& candidates, const QString& question, PublicLink* chosen) {
    if (candidates.empty()) return false;
    if (candidates.size() == 1) {
      *chosen = candidates.front();
      return true;
    }
    QStringList names;
    for (const PublicLink& link : candidates) names << link.name;
    bool ok = false;
    const QString picked = QInputDialog::getItem(this, QStringLiteral("CloudSync"), question, names, 0, false, &ok);
    if (!ok) return false;
    for (const PublicLink& link : candidates)
      if (link.name == picked) *chosen = link;
    return true;
  }

  void sendUpdateRequest() {
    if (selectedPublic.isEmpty() || !loggedIn()) return;
    const QString id = selectedPublic.value(QStringLiteral("id")).toString();
    PublicLink link;
    if (!chooseLink(linksFor(id, QStringLiteral("update")), QStringLiteral("Which copy do you want to send?"), &link)) {
      say(QStringLiteral("First use Copy project for update, change that copy, then send it."));
      return;
    }
    bool ok = false;
    const QString message = QInputDialog::getMultiLineText(
        this, QStringLiteral("Send update request"),
        QStringLiteral("Sending “%1” to %2 as an update to “%3” (you started from version %4).\n\nWhat did you change and why?")
            .arg(link.name, selectedPublic.value(QStringLiteral("author")).toString(), selectedPublic.value(QStringLiteral("title")).toString())
            .arg(link.baseVersion),
        QString(), &ok);
    if (!ok) return;
    if (message.trimmed().isEmpty()) {
      say(QStringLiteral("An update request needs a message."));
      return;
    }
    Busy busy(this);
    say(QStringLiteral("Packing %1…").arg(link.name));
    QByteArray zip, png;
    QString error;
    if (!packWithPreview(link.folder, &zip, &png, &error)) {
      say(QStringLiteral("Could not pack %1: %2").arg(link.name, error));
      return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("message"), message.trimmed());
    body.insert(QStringLiteral("baseVersion"), link.baseVersion);
    const Reply created = net.json("POST", QStringLiteral("/v1/public/%1/requests").arg(id), body);
    if (!created.ok()) {
      say(created.error);
      return;
    }
    const QString rid = created.json().value(QStringLiteral("id")).toString();
    say(QStringLiteral("Sending…"));
    if (!png.isEmpty()) net.call("PUT", QStringLiteral("/v1/public/%1/requests/%2/preview").arg(id, rid), png, "image/png");
    const Reply file = net.call("PUT", QStringLiteral("/v1/public/%1/requests/%2/file").arg(id, rid), zip, "application/zip");
    if (!file.ok()) {
      say(QStringLiteral("Could not send the request: %1").arg(file.error));
      return;
    }
    loaded[2] = false;
    refreshAccount();
    say(QStringLiteral("Sent. %1 sees it the next time they start DaedalCAD, and you will hear back when it is accepted or declined.")
            .arg(selectedPublic.value(QStringLiteral("author")).toString()));
    hooks.status(statusLine->text());
  }

  void publishNewVersion() {
    if (selectedPublic.isEmpty() || !loggedIn()) return;
    const QString id = selectedPublic.value(QStringLiteral("id")).toString();
    PublicLink link;
    if (!chooseLink(linksFor(id, QStringLiteral("owner")), QStringLiteral("Which local project is the new version?"), &link)) return;
    const int current = selectedPublic.value(QStringLiteral("version")).toInt();
    QString warn;
    if (link.baseVersion < current)
      warn = QStringLiteral("\n\nCareful: “%1” is based on version %2, but version %3 exists (probably an accepted update request). Publishing replaces those changes for everyone; use Bring my linked copy up to date first to keep them.")
                 .arg(link.name)
                 .arg(link.baseVersion)
                 .arg(current);
    if (QMessageBox::question(this, QStringLiteral("Publish new version"),
                              QStringLiteral("Send “%1” as version %2 of “%3”?%4").arg(link.name).arg(current + 1).arg(selectedPublic.value(QStringLiteral("title")).toString(), warn)) !=
        QMessageBox::Yes)
      return;
    Busy busy(this);
    QByteArray zip, png;
    QString error;
    if (!packWithPreview(link.folder, &zip, &png, &error)) {
      say(QStringLiteral("Could not pack %1: %2").arg(link.name, error));
      return;
    }
    const Reply file = net.call("PUT", QStringLiteral("/v1/public/%1/file").arg(id), zip, "application/zip");
    if (!file.ok()) {
      say(file.error);
      return;
    }
    if (!png.isEmpty()) net.call("PUT", QStringLiteral("/v1/public/%1/preview").arg(id), png, "image/png");
    link.baseVersion = file.json().value(QStringLiteral("version")).toInt(current + 1);
    writeLink(link);
    refreshAccount();
    refreshPublic();
    selectPublic(id);
    say(QStringLiteral("Published version %1.").arg(link.baseVersion));
  }

  void bringLinkedUpToDate() {
    if (selectedPublic.isEmpty() || !loggedIn()) return;
    const QString id = selectedPublic.value(QStringLiteral("id")).toString();
    const int current = selectedPublic.value(QStringLiteral("version")).toInt();
    std::vector<PublicLink> behind;
    for (const PublicLink& link : linksFor(id))
      if (link.baseVersion < current) behind.push_back(link);
    PublicLink link;
    if (!chooseLink(behind, QStringLiteral("Which copy should get the current version?"), &link)) return;
    if (QMessageBox::warning(this, QStringLiteral("Bring up to date"),
                             QStringLiteral("Replace “%1” (version %2) with version %3?\n\nChanges you made to “%1” since then are lost.").arg(link.name).arg(link.baseVersion).arg(current),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
      return;
    Busy busy(this);
    const Reply reply = net.call("GET", QStringLiteral("/v1/public/%1/file").arg(id));
    QString error;
    if (!installDownload(reply, link.name, true, &error)) {
      say(QStringLiteral("Could not update “%1”: %2").arg(link.name, error));
      return;
    }
    link.baseVersion = reply.version > 0 ? reply.version : current;
    writeLink(link);
    showPublicSelection();
    say(QStringLiteral("“%1” is now version %2 and is open.").arg(link.name).arg(link.baseVersion));
  }

  void editPublicDetails() {
    if (selectedPublic.isEmpty() || !loggedIn()) return;
    PublishForm form;
    form.title = selectedPublic.value(QStringLiteral("title")).toString();
    form.description = selectedPublic.value(QStringLiteral("description")).toString();
    QStringList tags;
    for (const QJsonValue& tag : selectedPublic.value(QStringLiteral("tags")).toArray()) tags << tag.toString();
    form.tags = tags.join(QStringLiteral(", "));
    if (!askDetails(QStringLiteral("Edit details"), &form, false)) return;
    QJsonObject body;
    body.insert(QStringLiteral("title"), form.title);
    body.insert(QStringLiteral("description"), form.description);
    body.insert(QStringLiteral("tags"), tagArray(form.tags));
    Busy busy(this);
    const QString id = selectedPublic.value(QStringLiteral("id")).toString();
    const Reply reply = net.json("PATCH", QStringLiteral("/v1/public/") + id, body);
    if (!reply.ok()) {
      say(reply.error);
      return;
    }
    refreshPublic();
    selectPublic(id);
    say(QStringLiteral("Details saved."));
  }

  void removePublic() {
    if (selectedPublic.isEmpty() || !loggedIn()) return;
    const QString title = selectedPublic.value(QStringLiteral("title")).toString();
    if (QMessageBox::warning(this, QStringLiteral("Delete from public"),
                             QStringLiteral("Delete “%1” from the public list, with every version and every update request?\n\nCopies other people already made stay theirs. Your local projects are not touched. This cannot be undone.").arg(title),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
      return;
    Busy busy(this);
    const QString id = selectedPublic.value(QStringLiteral("id")).toString();
    const Reply reply = net.call("DELETE", QStringLiteral("/v1/public/") + id);
    if (!reply.ok()) {
      say(reply.error);
      return;
    }
    // The local copy keeps working; it is just no longer linked.
    for (const PublicLink& link : linksFor(id)) QFile::remove(QDir(link.folder).absoluteFilePath(QStringLiteral("cloudsync.json")));
    refreshAccount();
    refreshPublic();
    say(QStringLiteral("Deleted “%1” from the public list.").arg(title));
  }

  // --- Update requests --------------------------------------------------------------------------
  QListWidget* incomingList = nullptr;
  QListWidget* outgoingList = nullptr;
  QLabel* currentPreview = nullptr;
  QLabel* requestPreview = nullptr;
  QLabel* requestInfo = nullptr;
  QPushButton* acceptButton = nullptr;
  QPushButton* declineButton = nullptr;
  QPushButton* lookButton = nullptr;
  QPushButton* withdrawButton = nullptr;
  QJsonObject selectedRequest;
  bool selectedIncoming = false;

  QWidget* buildRequests() {
    auto* page = new QWidget(this);
    auto* layout = new QHBoxLayout(page);
    auto* left = new QVBoxLayout();
    left->addWidget(new QLabel(QStringLiteral("<b>Waiting for you</b> (changes others sent to your projects)"), page));
    incomingList = new QListWidget(page);
    incomingList->setObjectName(QStringLiteral("cloudIncoming"));
    incomingList->setIconSize(QSize(80, 60));
    left->addWidget(incomingList, 3);
    left->addWidget(new QLabel(QStringLiteral("<b>Sent by you</b>"), page));
    outgoingList = new QListWidget(page);
    outgoingList->setObjectName(QStringLiteral("cloudOutgoing"));
    outgoingList->setIconSize(QSize(80, 60));
    left->addWidget(outgoingList, 2);
    layout->addLayout(left, 2);

    auto* right = new QVBoxLayout();
    auto* pictures = new QHBoxLayout();
    auto* nowColumn = new QVBoxLayout();
    auto* reqColumn = new QVBoxLayout();
    currentPreview = previewLabel(page, QSize(300, 225));
    requestPreview = previewLabel(page, QSize(300, 225));
    nowColumn->addWidget(new QLabel(QStringLiteral("Current version"), page));
    nowColumn->addWidget(currentPreview);
    reqColumn->addWidget(new QLabel(QStringLiteral("With this request"), page));
    reqColumn->addWidget(requestPreview);
    pictures->addLayout(nowColumn);
    pictures->addLayout(reqColumn);
    pictures->addStretch(1);
    right->addLayout(pictures);
    requestInfo = new QLabel(QStringLiteral("Select a request."), page);
    requestInfo->setWordWrap(true);
    requestInfo->setTextFormat(Qt::RichText);
    requestInfo->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    requestInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    right->addWidget(requestInfo, 1);
    lookButton = new QPushButton(QStringLiteral("Download to look at it"), page);
    lookButton->setToolTip(QStringLiteral("Open the request as a separate local project, without accepting it."));
    acceptButton = new QPushButton(QStringLiteral("Accept…"), page);
    acceptButton->setToolTip(QStringLiteral("Make this request the next version of your project."));
    declineButton = new QPushButton(QStringLiteral("Decline…"), page);
    withdrawButton = new QPushButton(QStringLiteral("Withdraw"), page);
    auto* row = new QHBoxLayout();
    row->addWidget(lookButton);
    row->addStretch(1);
    row->addWidget(withdrawButton);
    row->addWidget(declineButton);
    row->addWidget(acceptButton);
    right->addLayout(row);
    layout->addLayout(right, 3);

    connect(incomingList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
      if (!item) return;
      const QSignalBlocker blocker(outgoingList);
      outgoingList->clearSelection();
      outgoingList->setCurrentItem(nullptr);
      showRequest(item->data(Qt::UserRole).toJsonObject(), true);
    });
    connect(outgoingList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
      if (!item) return;
      const QSignalBlocker blocker(incomingList);
      incomingList->clearSelection();
      incomingList->setCurrentItem(nullptr);
      showRequest(item->data(Qt::UserRole).toJsonObject(), false);
    });
    connect(lookButton, &QPushButton::clicked, this, [this] { downloadRequest(); });
    connect(acceptButton, &QPushButton::clicked, this, [this] { decide(QStringLiteral("accept")); });
    connect(declineButton, &QPushButton::clicked, this, [this] { decide(QStringLiteral("decline")); });
    connect(withdrawButton, &QPushButton::clicked, this, [this] { decide(QStringLiteral("withdraw")); });
    showRequest({}, false);
    return page;
  }

  static QString statusWord(const QString& status) {
    if (status == QLatin1String("open")) return QStringLiteral("waiting");
    return status;
  }

  void refreshRequests() {
    const Reply in = net.call("GET", QStringLiteral("/v1/requests/incoming?status=open"));
    const Reply out = net.call("GET", QStringLiteral("/v1/requests/outgoing"));
    if (!in.ok() || !out.ok()) {
      say(in.ok() ? out.error : in.error);
      return;
    }
    incomingList->clear();
    outgoingList->clear();
    QStringList seenIncoming, seenDecisions;
    for (const QJsonValue& value : in.json().value(QStringLiteral("requests")).toArray()) {
      const QJsonObject r = value.toObject();
      auto* item = new QListWidgetItem(QStringLiteral("%1\nfrom %2 · %3%4")
                                           .arg(r.value(QStringLiteral("projectTitle")).toString(), r.value(QStringLiteral("from")).toString(),
                                                niceDate(r.value(QStringLiteral("created")).toString()),
                                                r.value(QStringLiteral("stale")).toBool() ? QStringLiteral(" · made on an older version") : QString()),
                                       incomingList);
      item->setData(Qt::UserRole, r);
      item->setSizeHint(QSize(0, 66));
      seenIncoming << r.value(QStringLiteral("id")).toString();
    }
    for (const QJsonValue& value : out.json().value(QStringLiteral("requests")).toArray()) {
      const QJsonObject r = value.toObject();
      auto* item = new QListWidgetItem(QStringLiteral("%1\n%2 · %3")
                                           .arg(r.value(QStringLiteral("projectTitle")).toString(), statusWord(r.value(QStringLiteral("status")).toString()),
                                                niceDate(r.value(QStringLiteral("created")).toString())),
                                       outgoingList);
      item->setData(Qt::UserRole, r);
      item->setSizeHint(QSize(0, 66));
      const QString status = r.value(QStringLiteral("status")).toString();
      if (status == QLatin1String("accepted")) item->setForeground(QColor(96, 196, 90));
      if (status == QLatin1String("declined")) item->setForeground(QColor(230, 110, 90));
      if (status == QLatin1String("accepted") || status == QLatin1String("declined")) seenDecisions << r.value(QStringLiteral("id")).toString();
    }
    // Looking at this page counts as having seen them: no start-up popup for these again.
    markSeen(net.host, QStringLiteral("incoming"), seenIncoming);
    markSeen(net.host, QStringLiteral("decisions"), seenDecisions);
    say(QStringLiteral("%1 request%2 waiting for you, %3 sent by you.")
            .arg(incomingList->count())
            .arg(incomingList->count() == 1 ? QString() : QStringLiteral("s"))
            .arg(outgoingList->count()));
    for (QListWidget* list : {incomingList, outgoingList}) {
      for (int i = 0; i < list->count(); ++i) {
        const QJsonObject r = list->item(i)->data(Qt::UserRole).toJsonObject();
        if (!r.value(QStringLiteral("hasPreview")).toBool() || r.value(QStringLiteral("status")).toString() != QLatin1String("open")) continue;
        QPixmap pixmap;
        const QByteArray png = image(QStringLiteral("/v1/public/%1/requests/%2/preview").arg(r.value(QStringLiteral("projectId")).toString(), r.value(QStringLiteral("id")).toString()));
        if (!png.isEmpty() && pixmap.loadFromData(png, "PNG")) list->item(i)->setIcon(QIcon(pixmap.scaled(80, 60, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
      }
    }
  }

  void showRequest(const QJsonObject& r, bool incoming) {
    selectedRequest = r;
    selectedIncoming = incoming;
    const bool any = !r.isEmpty();
    const bool open = r.value(QStringLiteral("status")).toString() == QLatin1String("open");
    acceptButton->setVisible(!any || incoming);
    declineButton->setVisible(!any || incoming);
    withdrawButton->setVisible(any && !incoming);
    acceptButton->setEnabled(any && incoming && open);
    declineButton->setEnabled(any && incoming && open);
    lookButton->setEnabled(any && open);
    withdrawButton->setEnabled(any && !incoming && open);
    if (!any) {
      showImage(currentPreview, {});
      showImage(requestPreview, {});
      requestInfo->setText(QStringLiteral("Select a request. Requests others send to your public projects show up at the top; yours below."));
      return;
    }
    const QString id = r.value(QStringLiteral("projectId")).toString();
    showImage(currentPreview, image(QStringLiteral("/v1/public/%1/preview").arg(id)));
    showImage(requestPreview, open && r.value(QStringLiteral("hasPreview")).toBool()
                                  ? image(QStringLiteral("/v1/public/%1/requests/%2/preview").arg(id, r.value(QStringLiteral("id")).toString()))
                                  : QByteArray());
    QString html = QStringLiteral("<h3 style='margin:0'>%1</h3>").arg(r.value(QStringLiteral("projectTitle")).toString().toHtmlEscaped());
    html += incoming ? QStringLiteral("<p>From <b>%1</b> · %2</p>").arg(r.value(QStringLiteral("from")).toString().toHtmlEscaped(), niceDate(r.value(QStringLiteral("created")).toString()))
                     : QStringLiteral("<p>To <b>%1</b> · sent %2 · <b>%3</b></p>")
                           .arg(r.value(QStringLiteral("owner")).toString().toHtmlEscaped(), niceDate(r.value(QStringLiteral("created")).toString()),
                                statusWord(r.value(QStringLiteral("status")).toString()));
    html += QStringLiteral("<p>Made on version %1; the project is at version %2.%3</p>")
                .arg(r.value(QStringLiteral("baseVersion")).toInt())
                .arg(r.value(QStringLiteral("projectVersion")).toInt())
                .arg(r.value(QStringLiteral("stale")).toBool()
                         ? QStringLiteral(" <b style='color:#e8a040'>Made on an older version: accepting it replaces whatever changed since.</b>")
                         : QString());
    html += QStringLiteral("<p><b>Message</b><br>%1</p>").arg(r.value(QStringLiteral("message")).toString().toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    if (!r.value(QStringLiteral("note")).toString().isEmpty())
      html += QStringLiteral("<p><b>Note from the author</b><br>%1</p>").arg(r.value(QStringLiteral("note")).toString().toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    if (r.value(QStringLiteral("status")).toString() == QLatin1String("accepted"))
      html += QStringLiteral("<p>Became version %1.</p>").arg(r.value(QStringLiteral("resultVersion")).toInt());
    requestInfo->setText(html);
  }

  void downloadRequest() {
    if (selectedRequest.isEmpty() || !loggedIn()) return;
    const QString name = uniqueName(QStringLiteral("%1 - request from %2")
                                        .arg(selectedRequest.value(QStringLiteral("projectTitle")).toString(),
                                             selectedIncoming ? selectedRequest.value(QStringLiteral("from")).toString() : QStringLiteral("me")));
    Busy busy(this);
    const Reply reply = net.call("GET", QStringLiteral("/v1/public/%1/requests/%2/file")
                                            .arg(selectedRequest.value(QStringLiteral("projectId")).toString(), selectedRequest.value(QStringLiteral("id")).toString()));
    QString error;
    if (!installDownload(reply, name, true, &error)) {
      say(QStringLiteral("Could not download the request: %1").arg(error));
      return;
    }
    fillLocal();
    say(QStringLiteral("Opened the request as “%1”. It is a separate copy; accepting or declining is still up to you here.").arg(name));
  }

  void decide(const QString& action) {
    if (selectedRequest.isEmpty() || !loggedIn()) return;
    const QString title = selectedRequest.value(QStringLiteral("projectTitle")).toString();
    QString note;
    if (action == QLatin1String("withdraw")) {
      if (QMessageBox::question(this, QStringLiteral("Withdraw"), QStringLiteral("Withdraw your request on “%1”?").arg(title)) != QMessageBox::Yes) return;
    } else {
      const bool accept = action == QLatin1String("accept");
      QString prompt = accept ? QStringLiteral("Accept the request from %1? It becomes version %2 of “%3”.")
                                    .arg(selectedRequest.value(QStringLiteral("from")).toString())
                                    .arg(selectedRequest.value(QStringLiteral("projectVersion")).toInt() + 1)
                                    .arg(title)
                              : QStringLiteral("Decline the request from %1 on “%2”?").arg(selectedRequest.value(QStringLiteral("from")).toString(), title);
      if (accept && selectedRequest.value(QStringLiteral("stale")).toBool())
        prompt += QStringLiteral("\n\nIt was made on version %1; whatever changed since is replaced.").arg(selectedRequest.value(QStringLiteral("baseVersion")).toInt());
      prompt += QStringLiteral("\n\nA note for them (optional):");
      bool ok = false;
      note = QInputDialog::getMultiLineText(this, accept ? QStringLiteral("Accept") : QStringLiteral("Decline"), prompt, QString(), &ok);
      if (!ok) return;
    }
    Busy busy(this);
    const QString projectId = selectedRequest.value(QStringLiteral("projectId")).toString();
    QJsonObject body;
    body.insert(QStringLiteral("note"), note.trimmed());
    const Reply reply = net.json("POST", QStringLiteral("/v1/public/%1/requests/%2/%3").arg(projectId, selectedRequest.value(QStringLiteral("id")).toString(), action), body);
    if (!reply.ok()) {
      say(reply.error);
      return;
    }
    const QJsonObject result = reply.json();
    loaded[1] = false;
    imageCache.clear();
    refreshAccount();
    refreshRequests();
    showRequest({}, false);
    if (action == QLatin1String("accept")) {
      const int version = result.value(QStringLiteral("resultVersion")).toInt();
      say(QStringLiteral("Accepted. “%1” is now at version %2.").arg(title).arg(version));
      // Offer to bring the author's own linked copy along, so the next version starts from this one.
      for (const PublicLink& link : linksFor(projectId, QStringLiteral("owner"))) {
        if (QMessageBox::question(this, QStringLiteral("Accepted"),
                                  QStringLiteral("Bring your local project “%1” up to version %2 as well?\n\nThat replaces it with the accepted version.").arg(link.name).arg(version)) !=
            QMessageBox::Yes)
          continue;
        const Reply file = net.call("GET", QStringLiteral("/v1/public/%1/file").arg(projectId));
        QString error;
        if (installDownload(file, link.name, true, &error)) {
          PublicLink updated = link;
          updated.baseVersion = file.version > 0 ? file.version : version;
          writeLink(updated);
          say(QStringLiteral("Accepted, and “%1” is now version %2.").arg(link.name).arg(updated.baseVersion));
        } else {
          say(QStringLiteral("Accepted, but “%1” could not be updated: %2").arg(link.name, error));
        }
        break;
      }
    } else {
      say(action == QLatin1String("decline") ? QStringLiteral("Declined.") : QStringLiteral("Withdrawn."));
    }
    hooks.status(statusLine->text());
  }
};

// ---------------------------------------------------------------------------------------------------
// Start-up check: two GETs in the background, then a popup if there is anything new.

struct StartupItem {
  QString title;
  QString line;
  QString text;
  QString previewPath;
};

void showStartupPopup(QWidget* parent, const CloudSyncHooks& hooks, const QString& host, const QString& key,
                      const std::vector<StartupItem>& items, const QStringList& incomingIds, const QStringList& decisionIds) {
  QDialog dialog(parent);
  dialog.setWindowTitle(QStringLiteral("CloudSync"));
  dialog.setObjectName(QStringLiteral("cloudStartup"));
  dialog.resize(640, 480);
  auto* layout = new QVBoxLayout(&dialog);
  auto* heading = new QLabel(QStringLiteral("<b>News from CloudSync</b>"), &dialog);
  layout->addWidget(heading);
  auto* list = new QListWidget(&dialog);
  list->setIconSize(QSize(96, 72));
  list->setWordWrap(true);
  layout->addWidget(list, 1);
  Net net;
  net.host = host;
  net.key = key;
  for (const StartupItem& item : items) {
    auto* row = new QListWidgetItem(QStringLiteral("%1\n%2\n%3").arg(item.title, item.line, item.text.left(160)), list);
    row->setSizeHint(QSize(0, 82));
    if (!item.previewPath.isEmpty()) {
      const Reply png = net.call("GET", item.previewPath);
      QPixmap pixmap;
      if (png.ok() && pixmap.loadFromData(png.body, "PNG")) row->setIcon(QIcon(pixmap.scaled(96, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
  }
  auto* buttons = new QHBoxLayout();
  auto* later = new QPushButton(QStringLiteral("Later"), &dialog);
  later->setToolTip(QStringLiteral("Show these again the next time DaedalCAD starts."));
  auto* review = new QPushButton(QStringLiteral("Review"), &dialog);
  review->setDefault(true);
  buttons->addStretch(1);
  buttons->addWidget(later);
  buttons->addWidget(review);
  layout->addLayout(buttons);
  QObject::connect(later, &QPushButton::clicked, &dialog, &QDialog::reject);
  QObject::connect(review, &QPushButton::clicked, &dialog, &QDialog::accept);
  if (dialog.exec() != QDialog::Accepted) return;
  markSeen(host, QStringLiteral("incoming"), incomingIds);
  markSeen(host, QStringLiteral("decisions"), decisionIds);
  openCloudSyncWindow(parent, hooks, 2);
}

}  // namespace

void openCloudSyncWindow(QWidget* parent, const CloudSyncHooks& hooks, int page) {
  CloudSyncWindow window(parent, hooks);
  window.start(page);
  window.exec();
}

void checkCloudSyncAtStartup(QWidget* parent, const CloudSyncHooks& hooks) {
  if (!cloudEnabled()) return;
  QSettings settings;
  const QString host = cloudHost(settings.value(QStringLiteral("cloudHost")).toString());
  const QString key = settings.value(QStringLiteral("cloudKey")).toString().trimmed();
  auto* manager = new QNetworkAccessManager(parent);
  struct State {
    QJsonArray incoming;
    QJsonArray outgoing;
    int pending = 2;
    bool failed = false;
  };
  auto state = std::make_shared<State>();
  QPointer<QWidget> owner(parent);
  auto finish = [=] {
    manager->deleteLater();
    if (state->failed || !owner) return;  // offline, server down or bad key: stay quiet
    const QSet<QString> seenIncoming = seenIds(host, QStringLiteral("incoming"));
    const QSet<QString> seenDecisions = seenIds(host, QStringLiteral("decisions"));
    std::vector<StartupItem> items;
    QStringList incomingIds, decisionIds;
    for (const QJsonValue& value : state->incoming) {
      const QJsonObject r = value.toObject();
      const QString id = r.value(QStringLiteral("id")).toString();
      if (seenIncoming.contains(id)) continue;
      incomingIds << id;
      items.push_back({r.value(QStringLiteral("projectTitle")).toString(),
                       QStringLiteral("Update request from %1%2").arg(r.value(QStringLiteral("from")).toString(),
                                                                       r.value(QStringLiteral("stale")).toBool() ? QStringLiteral(" (made on an older version)") : QString()),
                       r.value(QStringLiteral("message")).toString(),
                       r.value(QStringLiteral("hasPreview")).toBool()
                           ? QStringLiteral("/v1/public/%1/requests/%2/preview").arg(r.value(QStringLiteral("projectId")).toString(), id)
                           : QString()});
    }
    for (const QJsonValue& value : state->outgoing) {
      const QJsonObject r = value.toObject();
      const QString status = r.value(QStringLiteral("status")).toString();
      const QString id = r.value(QStringLiteral("id")).toString();
      if ((status != QLatin1String("accepted") && status != QLatin1String("declined")) || seenDecisions.contains(id)) continue;
      decisionIds << id;
      const QString note = r.value(QStringLiteral("note")).toString();
      items.push_back({r.value(QStringLiteral("projectTitle")).toString(),
                       status == QLatin1String("accepted")
                           ? QStringLiteral("%1 accepted your update request (now version %2)").arg(r.value(QStringLiteral("owner")).toString()).arg(r.value(QStringLiteral("resultVersion")).toInt())
                           : QStringLiteral("%1 declined your update request").arg(r.value(QStringLiteral("owner")).toString()),
                       note.isEmpty() ? r.value(QStringLiteral("message")).toString() : QStringLiteral("Note: ") + note,
                       QStringLiteral("/v1/public/%1/preview").arg(r.value(QStringLiteral("projectId")).toString())});
    }
    if (items.empty()) return;
    // Wait until any start-up dialog (recent projects) is closed.
    auto timer = new QTimer(owner);
    QObject::connect(timer, &QTimer::timeout, owner, [=] {
      if (!owner || QApplication::activeModalWidget()) return;
      timer->stop();
      timer->deleteLater();
      showStartupPopup(owner, hooks, host, key, items, incomingIds, decisionIds);
    });
    timer->start(300);
  };
  for (const QString& path : {QStringLiteral("/v1/requests/incoming?status=open"), QStringLiteral("/v1/requests/outgoing")}) {
    QNetworkRequest request(QUrl(host + path));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + key.toUtf8());
    request.setTransferTimeout(10000);
    QNetworkReply* reply = manager->get(request);
    const bool incoming = path.contains(QStringLiteral("incoming"));
    QObject::connect(reply, &QNetworkReply::finished, manager, [=] {
      const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (reply->error() != QNetworkReply::NoError || status != 200) state->failed = true;
      else (incoming ? state->incoming : state->outgoing) = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("requests")).toArray();
      reply->deleteLater();
      if (--state->pending == 0) finish();
    });
  }
}

// ---------------------------------------------------------------------------------------------------
// Preview picture: flat-shaded triangles, back to front, three-quarter view.

QImage renderPreviewImage(const std::vector<DrawItem>& items, const QSize& size) {
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  {
    QPainter fill(&image);
    QLinearGradient sky(0, 0, 0, size.height());
    sky.setColorAt(0, QColor(30, 35, 43));
    sky.setColorAt(1, QColor(16, 19, 24));
    fill.fillRect(image.rect(), sky);
  }
  struct Tri {
    QPointF p[3];
    float depth;
    QColor color;
  };
  // Camera: from the front-left, a little above, like the default 3D view.
  const float azimuth = -0.70f;
  const float elevation = 0.55f;
  const QVector3D forward(std::cos(elevation) * std::sin(azimuth), std::cos(elevation) * std::cos(azimuth), -std::sin(elevation));
  const QVector3D right = QVector3D::crossProduct(forward, QVector3D(0, 0, 1)).normalized();
  const QVector3D up = QVector3D::crossProduct(right, forward).normalized();
  const QVector3D light = (-forward * 0.6f + up * 0.6f + right * -0.35f).normalized();
  std::vector<Tri> tris;
  std::vector<QVector3D> views;
  float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
  for (const DrawItem& item : items) {
    if (item.hole) continue;
    const CpuMesh& mesh = meshOf(item.mesh);
    const size_t count = mesh.triCount();
    for (size_t t = 0; t < count; ++t) {
      QVector3D w[3];
      for (int k = 0; k < 3; ++k) {
        const float* p = mesh.corner(t, k);
        w[k] = item.model.map(QVector3D(p[0], p[1], p[2]));
      }
      const QVector3D normal = QVector3D::crossProduct(w[1] - w[0], w[2] - w[0]);
      if (normal.lengthSquared() < 1e-12f) continue;
      const QVector3D n = normal.normalized();
      if (QVector3D::dotProduct(n, forward) > 0) continue;  // facing away
      Tri tri;
      float depth = 0;
      for (int k = 0; k < 3; ++k) {
        const float x = QVector3D::dotProduct(w[k], right);
        const float y = QVector3D::dotProduct(w[k], up);
        tri.p[k] = QPointF(x, y);
        depth += QVector3D::dotProduct(w[k], forward);
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
      }
      tri.depth = depth / 3.f;
      const float diffuse = std::max(0.f, QVector3D::dotProduct(n, light));
      const float shade = 0.38f + 0.62f * diffuse;
      tri.color = QColor::fromRgbF(std::min(1.f, static_cast<float>(item.color.redF()) * shade),
                                   std::min(1.f, static_cast<float>(item.color.greenF()) * shade),
                                   std::min(1.f, static_cast<float>(item.color.blueF()) * shade));
      tris.push_back(tri);
    }
  }
  if (tris.empty()) return image;
  const float spanX = std::max(maxX - minX, 1e-3f);
  const float spanY = std::max(maxY - minY, 1e-3f);
  const float scale = std::min((size.width() * 0.84f) / spanX, (size.height() * 0.84f) / spanY);
  const QPointF centre((minX + maxX) * 0.5f, (minY + maxY) * 0.5f);
  auto toPixel = [&](const QPointF& p) {
    return QPointF(size.width() * 0.5 + (p.x() - centre.x()) * scale, size.height() * 0.5 - (p.y() - centre.y()) * scale);
  };
  std::sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, tris.size() < 300000);
  for (const Tri& tri : tris) {
    const QPointF pts[3] = {toPixel(tri.p[0]), toPixel(tri.p[1]), toPixel(tri.p[2])};
    painter.setPen(QPen(tri.color, 0.8));  // hides hairline seams between neighbours
    painter.setBrush(tri.color);
    painter.drawPolygon(pts, 3);
  }
  return image;
}
