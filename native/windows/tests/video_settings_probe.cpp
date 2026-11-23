#include "ui/session_window.h"
#include "video_settings_wire.h"

#include <QAbstractButton>
#include <QTest>

class VideoSettingsProbe final : public QObject {
    Q_OBJECT

private slots:
    void wireValues();
    void controlWaitsForHostAndPreservesOtherSettings();
};

void VideoSettingsProbe::wireValues() {
    using evp::VideoAdaptation;
    const evp::VideoSettings defaults;
    QCOMPARE(defaults.adaptation, VideoAdaptation::PreserveResolution);
    for (const auto mode : {VideoAdaptation::PreserveResolution, VideoAdaptation::Balanced,
                            VideoAdaptation::PreserveFramerate}) {
        const evp::VideoSettings choice{0, 144, mode};
        QVERIFY(choice.valid());
        QCOMPARE(evp::parseVideoAdaptation(evp::videoAdaptationName(mode)).value(), mode);
    }
    QVERIFY(!evp::parseVideoAdaptation(QJsonValue(QJsonValue::Undefined)));
    QVERIFY(!evp::parseVideoAdaptation(QJsonValue(QJsonValue::Null)));
    QVERIFY(!evp::parseVideoAdaptation(QJsonValue(1)));
    QVERIFY(!evp::parseVideoAdaptation(QStringLiteral("unknown")));
}

void VideoSettingsProbe::controlWaitsForHostAndPreservesOtherSettings() {
    using evp::VideoAdaptation;
    SessionWindow window(new QWidget);
    auto* clarity = window.findChild<QAbstractButton*>("videoAdaptationResolution");
    auto* balanced = window.findChild<QAbstractButton*>("videoAdaptationBalanced");
    auto* motion = window.findChild<QAbstractButton*>("videoAdaptationFramerate");
    auto* fps = window.findChild<QAbstractButton*>("videoFps90");
    QVERIFY(clarity && balanced && motion && fps);

    std::optional<evp::VideoSettings> request;
    connect(&window, &SessionWindow::videoSettingsRequested, this, [&](const evp::VideoSettings& settings) {
        request = settings;
    });

    const evp::VideoSettings defaults{14, 60};
    window.setVideoSettings(defaults, true, false);
    QVERIFY(fps->isEnabled());
    QVERIFY(!clarity->isEnabled() && !balanced->isEnabled() && !motion->isEnabled());
    balanced->click();
    QVERIFY(!request);

    window.setVideoSettings(defaults, true, true);
    QVERIFY(clarity->isChecked() && balanced->isEnabled() && motion->isEnabled());
    balanced->click();
    const evp::VideoSettings balancedChoice{14, 60, VideoAdaptation::Balanced};
    QVERIFY(request && *request == balancedChoice);
    QVERIFY(clarity->isChecked() && !balanced->isChecked());

    window.setVideoSettings(defaults, false, true);
    QVERIFY(clarity->isChecked() && !balanced->isEnabled());
    window.setVideoSettings(balancedChoice, true, true);
    QVERIFY(balanced->isChecked() && !clarity->isChecked());

    fps->click();
    const evp::VideoSettings fasterChoice{14, 90, VideoAdaptation::Balanced};
    QVERIFY(request && *request == fasterChoice);
}

QTEST_MAIN(VideoSettingsProbe)
#include "video_settings_probe.moc"
