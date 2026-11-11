#pragma once

// The start-up picture, drawn inside the main window: it shows for a moment, then fades out (a
// click skips ahead). Calls `finished` once it is gone, so start-up questions come after it.

#include <QPixmap>
#include <QWidget>
#include <functional>

class QVariantAnimation;

class SplashOverlay : public QWidget {
 public:
  SplashOverlay(QWidget* window, const QPixmap& picture, int holdMs, int fadeMs, std::function<void()> finished);
  void fadeOut();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  QPixmap picture;
  double opacity = 1.0;
  int fadeMs = 800;
  bool fading = false;
  QVariantAnimation* fade = nullptr;
  std::function<void()> finished;
};
