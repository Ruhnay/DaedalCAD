#include "splashoverlay.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QVariantAnimation>

SplashOverlay::SplashOverlay(QWidget* window, const QPixmap& pictureIn, int holdMs, int fadeMsIn, std::function<void()> done)
    : QWidget(window), picture(pictureIn), fadeMs(fadeMsIn), finished(std::move(done)) {
  setObjectName(QStringLiteral("splashOverlay"));
  setAttribute(Qt::WA_NoSystemBackground);
  setAutoFillBackground(false);
  setCursor(Qt::PointingHandCursor);
  setToolTip(QStringLiteral("Click to skip"));
  setGeometry(window->rect());
  window->installEventFilter(this);
  raise();
  show();
  QTimer::singleShot(holdMs, this, [this] { fadeOut(); });
}

void SplashOverlay::fadeOut() {
  if (fading) return;
  fading = true;
  fade = new QVariantAnimation(this);
  fade->setStartValue(opacity);
  fade->setEndValue(0.0);
  fade->setDuration(fadeMs);
  fade->setEasingCurve(QEasingCurve::InOutQuad);
  connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    opacity = value.toDouble();
    update();
  });
  connect(fade, &QVariantAnimation::finished, this, [this] {
    hide();
    if (parentWidget()) parentWidget()->removeEventFilter(this);
    if (finished) finished();
    deleteLater();
  });
  fade->start();
}

void SplashOverlay::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setOpacity(opacity);
  painter.fillRect(rect(), QColor(15, 18, 24));
  if (picture.isNull()) return;
  // As large as fits in 60% of the window, never blown up past its own size.
  QSize size = picture.size();
  const QSize room(static_cast<int>(width() * 0.6), static_cast<int>(height() * 0.6));
  if (size.width() > room.width() || size.height() > room.height()) size.scale(room, Qt::KeepAspectRatio);
  const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawPixmap(target, picture);
}

void SplashOverlay::mousePressEvent(QMouseEvent* event) {
  event->accept();
  fadeOut();
}

bool SplashOverlay::eventFilter(QObject* watched, QEvent* event) {
  if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::ChildAdded)) {
    setGeometry(parentWidget()->rect());
    raise();
  }
  return QWidget::eventFilter(watched, event);
}
