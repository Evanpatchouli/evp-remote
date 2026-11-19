#pragma once

#include <QMainWindow>

class QCloseEvent;
class QEvent;
class QLabel;
class QSize;

class SessionWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit SessionWindow(QWidget* videoSurface, QWidget* parent = nullptr);

public slots:
    void setStatus(const QString& message);
    void setConnected(bool connected);
    void setInputAvailable(bool available);
    void setRoute(const QString& route);
    void setVideoFormat(const QSize& resolution, double fps);
    void showFailure(const QString& message);
    void finish();

signals:
    void endRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void updateStatus();

    QLabel* statusIndicator_;
    QWidget* toolbar_ = nullptr;
    QLabel* statusLabel_;
    QLabel* routeLabel_;
    QLabel* videoFormatLabel_;
    QLabel* inputLabel_;
    QString statusMessage_;
    bool connected_ = false;
    bool inputAvailable_ = false;
    bool failed_ = false;
    bool closeHandled_ = false;
};
