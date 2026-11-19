#include "controller.h"
#include "device_probe.h"
#include "devices/device_identity.h"
#include "ui/select_field.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <Windows.h>

namespace {

void verifyElevation(const QPixmap& layer, const QRect& panel) {
    const auto pixels = layer.toImage().convertToFormat(QImage::Format_ARGB32);
    const auto ratio = layer.devicePixelRatio();
    const auto alpha = [&](int column, int row) {
        return qAlpha(pixels.pixel(qRound(column * ratio), qRound(row * ratio)));
    };
    for (const auto corner : {pixels.rect().topLeft(), pixels.rect().topRight(),
                              pixels.rect().bottomLeft(), pixels.rect().bottomRight()})
        QCOMPARE(qAlpha(pixels.pixel(corner)), 0);
    for (const auto corner : {panel.topLeft(), panel.topRight(), panel.bottomLeft(), panel.bottomRight()})
        QVERIFY(alpha(corner.x(), corner.y()) < 80);
    QVERIFY(layer.deviceIndependentSize().height() - panel.bottom() > 6);
    const auto nearShadow = alpha(panel.center().x(), panel.bottom() + 2);
    QVERIFY(nearShadow > 0 && nearShadow < 80);
    QVERIFY(alpha(panel.center().x(), panel.bottom() + 6) < nearShadow);
}

class DeviceSecurityProbe : public QObject {
    Q_OBJECT

private slots:
    void bindsProofToPeerAttemptNonceAndPayload();
    void pausesOnUnwritableSettingsAcrossRestarts();
    void assistanceTabsAndScrollableDeviceList();
    void cleanTabFocus();
    void themedLifetimeSelectors_data();
    void themedLifetimeSelectors();
    void floatingStopControl();
    void onscreenPopupOpening();
};

void DeviceSecurityProbe::bindsProofToPeerAttemptNonceAndPayload() {
    evp::DeviceIdentity host, viewer;
    QVERIFY(host.load({}));
    QVERIFY(viewer.load({}));
    const auto hostId = evp::deviceId(host.publicKey());
    const auto viewerId = evp::deviceId(viewer.publicKey());
    QVERIFY(!hostId.isEmpty() && !viewerId.isEmpty() && hostId != viewerId);
    const auto token = [] { return evp::encodeDeviceBytes(evp::deviceRandom()); };
    const auto attempt = token();
    const auto nonce = token();
    const auto payload = [](const QByteArray& fingerprint) {
        const auto sdp = QString::fromLatin1("v=0\r\na=fingerprint:sha-256 " + fingerprint.toHex(':') + "\r\n");
        const QJsonObject description{{"type", "offer"}, {"sdp", sdp}};
        return evp::encodeDeviceBytes(QJsonDocument(QJsonObject{{"description", description}}).toJson(QJsonDocument::Compact));
    };
    const auto original = payload(evp::deviceRandom());
    const auto frame = [&](const QString& frameAttempt, const QString& frameNonce, const QString& sender,
                           const QString& sequence, const QString& data) {
        return evp::deviceProof({"frame", "control", viewerId, hostId, frameAttempt, frameNonce,
                                 sender, "signal", sequence, data});
    };
    const auto proof = frame(attempt, nonce, "host", "2", original);
    const auto signature = host.sign(proof);
    QVERIFY(evp::DeviceIdentity::verify(host.publicKey(), proof, signature));
    QVERIFY(!evp::DeviceIdentity::verify(viewer.publicKey(), proof, signature));
    const auto verifyFrame = [&](const QByteArray& bytes) {
        return evp::DeviceIdentity::verify(host.publicKey(), bytes, signature);
    };
    QVERIFY(!verifyFrame(frame(token(), nonce, "host", "2", original)));
    QVERIFY(!verifyFrame(frame(attempt, token(), "host", "2", original)));
    QVERIFY(!verifyFrame(frame(attempt, nonce, "viewer", "2", original)));
    QVERIFY(!verifyFrame(frame(attempt, nonce, "host", "3", original)));
    QVERIFY(!verifyFrame(frame(attempt, nonce, "host", "2", payload(evp::deviceRandom()))));
}

void DeviceSecurityProbe::pausesOnUnwritableSettingsAcrossRestarts() {
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const auto path = profile.filePath("devices.dat");
    QString id;
    {
        evp::DeviceStore initial(path);
        QVERIFY(initial.open());
        id = evp::deviceId(initial.identity.publicKey());
        initial.preferences.allowIncoming = true;
        QVERIFY(initial.save());
    }
    const auto nativePath = QDir::toNativeSeparators(path).toStdWString();
    const auto open = CreateFileW(nativePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(open != INVALID_HANDLE_VALUE);
    const auto close = [](void* handle) { CloseHandle(handle); };
    std::unique_ptr<void, decltype(close)> heldFile(open, close);
    {
        HomeWindow window;
        evp::DesktopController controller(window, {}, path);
        const device_probe::Ui ui(window);
        QCOMPARE(ui.id(), id);
        QVERIFY(!ui.online());
        QVERIFY(!window.findChild<QCheckBox*>("allowTrustedControl")->isChecked());
        QVERIFY(ui.message().contains(QStringLiteral("尚未保存")));
        QVERIFY(QFile::exists(path + ".hold"));
    }
    heldFile.reset();
    {
        HomeWindow window;
        evp::DesktopController controller(window, {}, path);
        const device_probe::Ui ui(window);
        QVERIFY(!window.findChild<QCheckBox*>("allowTrustedControl")->isChecked());
        QVERIFY(!ui.online());
        window.findChild<QCheckBox*>("deviceShareAudio")->setChecked(false);
        QVERIFY(!QFile::exists(path + ".hold"));
    }
    evp::DeviceStore recovered(path);
    QVERIFY(recovered.open());
    QCOMPARE(evp::deviceId(recovered.identity.publicKey()), id);
    QVERIFY(!recovered.preferences.allowIncoming);
    QVERIFY(!recovered.preferences.audio);
}

void DeviceSecurityProbe::assistanceTabsAndScrollableDeviceList() {
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    auto* tabs = window.findChild<QTabBar*>("assistanceTabs");
    auto* navigation = window.findChild<QPushButton*>("navigation");
    QVERIFY(tabs && navigation && window.findChild<QPushButton*>("deviceNavigation"));
    QVERIFY(!window.findChild<QPushButton*>("hostNavigation"));
    navigation->click();
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->tabText(0), QStringLiteral("远程协助"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("共享本机"));
    QCOMPARE(tabs->currentIndex(), 0);
    window.setSignalingUrl(QUrl("wss://first.example.invalid/"));
    QSignalSpy joinRequest(&window, &HomeWindow::joinRequested);
    window.findChild<QLineEdit*>("invitation")->setText("EVP1.preview.private");
    window.findChild<QPushButton*>("join")->click();
    QCOMPARE(joinRequest.count(), 0);
    QVERIFY(window.findChild<QLabel*>("bannerMessage")->text().contains(QStringLiteral("我的设备")));
    window.findChild<QLineEdit*>("invitation")->clear();
    tabs->setFocus();
    QTest::keyClick(tabs, Qt::Key_Right);
    QCOMPARE(tabs->currentIndex(), 1);
    QVERIFY(window.findChild<QPushButton*>("hostCreate")->isVisible());
    auto* key = window.findChild<QLineEdit*>("hostKey");
    key->setText("ui-test-credential");
    tabs->setCurrentIndex(0);
    window.findChild<QLineEdit*>("signalingUrl")->setText("wss://second.example.invalid/");
    tabs->setCurrentIndex(1);
    QVERIFY(key->text().isEmpty());
    window.setSignalingUrl({});
    const QList<int> minutes{5, 15, 30, 60, 120, 300, 720};
    for (const auto* name : {"invitationLifetime", "pairingLifetime"}) {
        auto* combo = window.findChild<QComboBox*>(name);
        QVERIFY(combo);
        QCOMPARE(combo->currentData().toInt(), 5);
        QCOMPARE(combo->count(), minutes.size());
        for (int index = 0; index < minutes.size(); ++index)
            QCOMPARE(combo->itemData(index).toInt(), minutes.at(index));
    }
    QVERIFY(!window.findChild<QCheckBox*>("autoAcceptInvitation")->isChecked());
    QCoreApplication::processEvents();
    if (!preview.isEmpty())
        QVERIFY(window.grab().save(QDir(preview).filePath("assistance.png")));

    window.showDevicePage();
    QVERIFY(!tabs->isVisible());
    QVERIFY(!window.findChild<QCheckBox*>("autoAcceptPairing")->isChecked());
    auto& page = window.devices();
    page.setIdentity("UI-PREVIEW");
    page.setRegistration(true, QStringLiteral("本机在线"));
    QMap<QString, evp::DevicePeer> peers;
    QSet<QString> online;
    const QStringList names{QStringLiteral("办公室电脑"), QStringLiteral("家用台式机"),
                            QStringLiteral("工作笔记本"), QStringLiteral("客厅电脑")};
    for (int index = 0; index < 24; ++index) {
        const auto id = QString("preview-%1").arg(index, 2, 10, QChar('0'));
        peers.insert(id, {{}, index < names.size() ? names.at(index) : QStringLiteral("测试设备 %1").arg(index)});
        if (index % 3 != 1)
            online.insert(id);
    }
    page.setDevices(peers, online);
    auto* list = window.findChild<QListWidget*>("trustedDeviceList");
    QVERIFY(list);
    QCOMPARE(list->count(), peers.size());
    QTRY_VERIFY(list->verticalScrollBar()->maximum() > 0);
    list->setCurrentRow(1);
    QTest::keyClick(list, Qt::Key_Down);
    QCOMPARE(list->currentRow(), 2);
    const auto selected = list->currentItem()->data(Qt::UserRole).toString();
    auto* connectButton = window.findChild<QPushButton*>("connectDevice");
    QVERIFY(connectButton->isEnabled());
    QTest::qWait(120);
    if (!preview.isEmpty()) {
        QVERIFY(window.grab().save(QDir(preview).filePath("devices.png")));
        auto* pairingLifetime = window.findChild<QComboBox*>("pairingLifetime");
        pairingLifetime->setCurrentIndex(pairingLifetime->findData(120));
        auto* pageScroll = window.findChild<QScrollArea*>("homeScroll");
        auto* pairingCard = pairingLifetime->parentWidget();
        pageScroll->verticalScrollBar()->setValue(pairingCard->mapTo(pageScroll->widget(), QPoint{}).y() - 16);
        QTest::qWait(100);
        QVERIFY(window.grab().save(QDir(preview).filePath("pairing-options.png")));
    }
    auto* scroll = list->verticalScrollBar();
    scroll->setValue(scroll->maximum());
    const int position = scroll->value();
    online.remove(selected);
    page.setDevices(peers, online);
    QCoreApplication::processEvents();
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), selected);
    QCOMPARE(scroll->value(), position);
    QVERIFY(!connectButton->isEnabled());
    navigation->click();
    QCOMPARE(tabs->currentIndex(), 1);
}

void DeviceSecurityProbe::cleanTabFocus() {
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    QApplication::setActiveWindow(&window);
    auto* tabs = window.findChild<QTabBar*>("assistanceTabs");
    auto* navigation = window.findChild<QPushButton*>("navigation");
    QVERIFY(tabs && navigation);
    tabs->setFocus(Qt::MouseFocusReason);
    QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(0).center());
    QCOMPARE(tabs->currentIndex(), 0);
    QVERIFY(tabs->hasFocus());
    const QImage clicked = tabs->grab().toImage();
    navigation->setFocus(Qt::OtherFocusReason);
    QVERIFY(!tabs->hasFocus());
    const QImage unfocused = tabs->grab().toImage();
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    if (!preview.isEmpty())
        QVERIFY(clicked.save(QDir(preview).filePath("clicked-tabs.png")));
    QVERIFY2(clicked == unfocused, "A mouse click drew an extra focus rectangle on the selected tab.");
    QTest::keyClick(navigation, Qt::Key_Tab);
    QVERIFY(tabs->hasFocus());
    const QImage keyboard = tabs->grab().toImage();
    QVERIFY2(keyboard != unfocused, "Keyboard navigation needs a visible focus indicator.");
    if (!preview.isEmpty())
        QVERIFY(keyboard.save(QDir(preview).filePath("keyboard-tabs.png")));
    QTest::keyClick(tabs, Qt::Key_Right);
    QCOMPARE(tabs->currentIndex(), 1);
    QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(0).center());
    QCOMPARE(tabs->currentIndex(), 0);
    QVERIFY(tabs->grab().toImage() == clicked);
}

void DeviceSecurityProbe::themedLifetimeSelectors_data() {
    QTest::addColumn<bool>("pairing");
    QTest::newRow("invitation") << false;
    QTest::newRow("pairing") << true;
}

void DeviceSecurityProbe::themedLifetimeSelectors() {
    QFETCH(bool, pairing);
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    auto& devices = window.devices();
    if (pairing)
        window.showDevicePage();
    else
        window.findChild<QTabBar*>("assistanceTabs")->setCurrentIndex(1);
    auto* field = window.findChild<native_ui::SelectField*>(pairing ? "pairingLifetime" : "invitationLifetime");
    QVERIFY(field);
    QCOMPARE(field->accessibleName(), pairing ? QStringLiteral("配对码有效期") : QStringLiteral("邀请码有效期"));
    if (pairing) {
        QVERIFY(!field->isEnabled());
        QVERIFY(!window.findChild<QPushButton*>("generatePairingCode")->isEnabled());
        QVERIFY(window.findChild<QLineEdit*>("localPairingCode")->isReadOnly());
        devices.setIdentity("UI-PREVIEW");
        devices.setPreferences({}, true);
        devices.setRegistration(true, QStringLiteral("本机在线"));
        auto* scroll = window.findChild<QScrollArea*>("homeScroll");
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    }
    QTest::qWait(100);
    QVERIFY(field->isEnabled());
    auto* view = field->view();
    auto* popup = view->window();
    QVERIFY(popup != &window);
    popup->setAttribute(Qt::WA_DontShowOnScreen);
    QSignalSpy changed(field, &QComboBox::currentIndexChanged);
    field->setFocus();
    QTest::keyClick(field, Qt::Key_Down, Qt::AltModifier);
    QTRY_VERIFY(popup->isVisible());
    QTRY_VERIFY(view->visualRect(field->model()->index(0, 0)).height() >= 32);
    QTest::keyClick(view, Qt::Key_Down);
    QTest::keyClick(view, Qt::Key_Escape);
    QTRY_VERIFY(!popup->isVisible());
    QCOMPARE(field->currentData().toInt(), 5);
    QCOMPARE(changed.count(), 0);

    field->showPopup();
    QTRY_VERIFY(popup->isVisible());
    QTest::keyClick(view, Qt::Key_Down);
    QTest::keyClick(view, Qt::Key_Return);
    QTRY_VERIFY(!popup->isVisible());
    QCOMPARE(field->currentData().toInt(), 15);
    QCOMPARE(changed.count(), 1);
    QTest::mouseClick(field, Qt::LeftButton);
    QTRY_VERIFY(popup->isVisible());
    const auto last = field->model()->index(field->count() - 1, 0);
    view->scrollTo(last);
    const auto lastRow = view->visualRect(last);
    QVERIFY(view->viewport()->rect().contains(lastRow.center()));
    QMouseEvent hover(QEvent::MouseMove, lastRow.center(), view->viewport()->mapToGlobal(lastRow.center()),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &hover);
    QCOMPARE(view->currentIndex(), last);
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    const auto layer = popup->grab();
    if (!preview.isEmpty()) {
        auto image = window.grab();
        QPainter painter(&image);
        painter.drawPixmap(window.mapFromGlobal(popup->mapToGlobal(QPoint{})), layer);
        painter.end();
        const auto filename = pairing ? "pairing-selector.png" : "invitation-selector.png";
        QVERIFY(image.save(QDir(preview).filePath(filename)));
        QVERIFY(layer.save(QDir(preview).filePath(pairing ? "pairing-layer.png" : "invitation-layer.png")));
    }
    const QRect panel(view->mapTo(popup, QPoint{}), view->size());
    verifyElevation(layer, panel);
    QVERIFY(view->viewport()->rect().contains(lastRow));
    QCOMPARE(panel.width(), field->width());
    QTest::qWait(QApplication::doubleClickInterval() + 20);
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, lastRow.center());
    QTRY_VERIFY(!popup->isVisible());
    QCOMPARE(field->currentData().toInt(), 720);
    QCOMPARE(changed.count(), 2);
    field->showPopup();
    QTRY_VERIFY(popup->isVisible());
    QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier, QPoint(2, panel.center().y()));
    QTRY_VERIFY(!popup->isVisible());
    QCOMPARE(changed.count(), 2);
    field->showPopup();
    QTRY_VERIFY(popup->isVisible());
    const auto anchor = field->mapToGlobal(field->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, popup->mapFromGlobal(anchor), anchor,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(popup, &press);
    QTRY_VERIFY(!popup->isVisible());
    QVERIFY(popup->testAttribute(Qt::WA_NoMouseReplay));
    QCOMPARE(changed.count(), 2);
    if (pairing) {
        devices.setActivity(true, false);
        QVERIFY(!field->isEnabled());
        devices.setActivity(false, false);
        QVERIFY(field->isEnabled());
        devices.setRegistration(false, QStringLiteral("离线"));
        QVERIFY(!field->isEnabled());
        QCOMPARE(field->currentData().toInt(), 720);
    }
}

void DeviceSecurityProbe::floatingStopControl() {
    HomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    QVERIFY(window.setHostBusy(true, true));
    window.showMinimized();
    window.setHostActive(true);
    QWidget* panel = nullptr;
    QPushButton* stop = nullptr;
    for (auto* top : QApplication::topLevelWidgets()) {
        if (auto* button = top->findChild<QPushButton*>("trustedShareStop")) {
            panel = top;
            stop = button;
            break;
        }
    }
    QVERIFY(panel && stop);
    QTRY_VERIFY(panel->isVisible() && stop->isVisible());
    QVERIFY(panel->testAttribute(Qt::WA_DontShowOnScreen));
    QVERIFY(panel->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
    const auto image = panel->grab();
    verifyElevation(image, QRect(stop->mapTo(panel, QPoint{}), stop->size()));
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    if (!preview.isEmpty())
        QVERIFY(image.save(QDir(preview).filePath("stop-sharing-layer.png")));
    QSignalSpy stopped(&window, &HomeWindow::hostEndRequested);
    QTest::mouseClick(stop, Qt::LeftButton);
    QCOMPARE(stopped.count(), 1);
    QVERIFY(window.setHostBusy(false));
    QVERIFY(!panel->isVisible());
}

void DeviceSecurityProbe::onscreenPopupOpening() {
    if (qEnvironmentVariableIsEmpty("EVP_PROBE_ONSCREEN_POPUP"))
        QSKIP("Set EVP_PROBE_ONSCREEN_POPUP=1 to exercise the Windows desktop compositor.");
    auto* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);
    QWidget fixture(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    fixture.setAttribute(Qt::WA_ShowWithoutActivating);
    fixture.setFixedSize(400, 380);
    fixture.setAutoFillBackground(true);
    const QColor background("#e3ecf7");
    auto colors = fixture.palette();
    colors.setColor(QPalette::Window, background);
    fixture.setPalette(colors);
    auto* layout = new QVBoxLayout(&fixture);
    layout->setContentsMargins(80, 24, 80, 24);
    auto* label = new QLabel(QStringLiteral("邀请码有效期"));
    layout->addWidget(label);
    auto* field = native_ui::codeLifetimeSelector(label, "probeLifetime");
    layout->addWidget(field);
    layout->addStretch();
    auto* popup = field->view()->window();
    const auto area = screen->availableGeometry();
    fixture.move(area.left() + (area.width() - fixture.width()) / 2,
                 area.top() + (area.height() - fixture.height()) / 2);
    fixture.show();
    QTest::qWait(150);
    const bool general = QApplication::isEffectEnabled(Qt::UI_General);
    const bool animated = QApplication::isEffectEnabled(Qt::UI_AnimateCombo);
    const auto restore = qScopeGuard([general, animated] {
        QApplication::setEffectEnabled(Qt::UI_AnimateCombo, animated);
        QApplication::setEffectEnabled(Qt::UI_General, general);
    });
    QApplication::setEffectEnabled(Qt::UI_General, true);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, true);
    const auto origin = fixture.mapToGlobal(QPoint{}) - screen->geometry().topLeft();
    const auto grab = [&] { return screen->grabWindow(0, origin.x(), origin.y(), fixture.width(), fixture.height()); };
    const auto baseline = grab();
    QVERIFY(!baseline.isNull());
    const auto ratio = baseline.devicePixelRatio();
    const auto sample = [&](const QImage& image, QPoint point) {
        return image.pixelColor(qRound(point.x() * ratio), qRound(point.y() * ratio));
    };
    QVERIFY(qAbs(sample(baseline.toImage(), QPoint(5, 5)).red() - background.red()) < 8);
    QTest::mouseClick(field, Qt::LeftButton);
    QElapsedTimer opening;
    opening.start();
    bool extraWindow = false, capturedDarkFrame = false;
    const auto preview = qEnvironmentVariable("EVP_PROBE_UI_DIR");
    do {
        QCoreApplication::processEvents();
        for (auto* top : QApplication::topLevelWidgets()) {
            if (top != &fixture && top != popup && top->isVisible())
                extraWindow = true;
        }
        const auto frame = grab();
        const auto image = frame.toImage();
        const QPoint surface = fixture.mapFromGlobal(popup->mapToGlobal(QPoint{}));
        const auto size = popup->size();
        const auto left = sample(image, surface + QPoint(3, size.height() / 2));
        const auto right = sample(image, surface + QPoint(size.width() - 4, size.height() / 2));
        const auto bottom = sample(image, surface + QPoint(size.width() / 2, size.height() - 4));
        const auto shadow = [](const QColor& color) { return qMax(color.red(), qMax(color.green(), color.blue())) < 135; };
        if (shadow(left) || shadow(right) || shadow(bottom)) {
            if (!capturedDarkFrame && !preview.isEmpty())
                frame.save(QDir(preview).filePath("opening-dark-frame.png"));
            capturedDarkFrame = true;
        }
        QTest::qWait(8);
    } while (opening.elapsed() < 230);
    const auto settled = grab();
    const auto bodyPoint = fixture.mapFromGlobal(popup->mapToGlobal(QPoint(popup->width() - 25, popup->height() / 2)));
    const auto body = sample(settled.toImage(), bodyPoint);
    if (!preview.isEmpty())
        QVERIFY(settled.save(QDir(preview).filePath("opening-settled.png")));
    QVERIFY(popup->isVisible());
    QVERIFY(QApplication::isEffectEnabled(Qt::UI_AnimateCombo));
    field->hidePopup();
    fixture.hide();
    QVERIFY(body.red() > 245 && body.green() > 245);
    QVERIFY2(!capturedDarkFrame, "The desktop compositor rendered a dark popup gutter.");
    QVERIFY2(!extraWindow, "An extra top-level animation window appeared while the dropdown opened.");
}

}

int runDeviceSecurityProbe(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir preferences;
    if (!preferences.isValid())
        return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    app.setOrganizationName("EVP-Smoke");
    app.setApplicationName("DeviceChecks");
    DeviceSecurityProbe tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "device_security_probe.moc"
