#pragma once

#include "../video_settings.h"
#include <QMainWindow>

class QCloseEvent;
class QButtonGroup;
class QDialog;
class QEvent;
class QLabel;
class QPushButton;
class QSize;
class QSpinBox;

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
    void setVideoSettings(const evp::VideoSettings& settings, bool available, bool adaptationSupported,
                          const QString& message = {});
    void showFailure(const QString& message);
    void finish();

signals:
    void endRequested();
    void videoSettingsRequested(const evp::VideoSettings& settings);

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void createVideoSettingsPanel();
    void positionVideoSettingsPanel();
    void requestVideoSettings(const evp::VideoSettings& choice);
    void updateStatus();

    QLabel* statusIndicator_;
    QWidget* toolbar_ = nullptr;
    QWidget* fullscreenBanner_ = nullptr;
    QLabel* statusLabel_;
    QLabel* routeLabel_;
    QLabel* inputLabel_;
    QPushButton* videoSettingsButton_;
    QDialog* videoSettingsPanel_;
    QButtonGroup* bitrateOptions_;
    QButtonGroup* frameRateOptions_;
    QButtonGroup* adaptationOptions_;
    QSpinBox* customBitrate_;
    QPushButton* applyBitrate_;
    QLabel* customBitrateInfo_;
    QLabel* adaptationNote_;
    QLabel* videoSettingsMessage_;
    QWidget* videoSettingsFocus_ = nullptr;
    QString statusMessage_;
    evp::VideoSettings videoSettings_;
    bool videoSettingsAvailable_ = false;
    bool connected_ = false;
    bool inputAvailable_ = false;
    bool failed_ = false;
    bool closeHandled_ = false;
};
