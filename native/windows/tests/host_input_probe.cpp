#include "host_input.h"

#include <QTest>
#include <Windows.h>
#include <vector>

namespace {
using evp::HostInput;
using Result = HostInput::Result;

class HostInputProbe : public QObject {
    Q_OBJECT

  private slots:
    void translatesOnlyWhileArmed();
    void releasesHeldInput();
    void failsClosedAndRetriesRelease();
};

void HostInputProbe::translatesOnlyWhileArmed() {
    std::vector<INPUT> sent;
    HostInput sink([&](const INPUT& event) {
        sent.push_back(event);
        return true;
    }, [] { return QString{}; });
    const QJsonObject move{{"type", "move"}, {"x", 0.5}, {"y", 1.0}};
    QCOMPARE(sink.apply(move), Result::Ignored);
    QVERIFY(sent.empty());
    QCOMPARE(sink.arm(), QString{});
    QCOMPARE(sink.apply({{"type", "move"}, {"x", "0.5"}, {"y", 0}}), Result::Ignored);
    QCOMPARE(sink.apply({{"type", "move"}, {"x", -0.01}, {"y", 0}}), Result::Ignored);
    QCOMPARE(sink.apply({{"type", "wheel"}, {"delta", -32769}}), Result::Ignored);
    QCOMPARE(sink.apply({{"type", "wheel"}, {"delta", 1.5}}), Result::Ignored);
    QCOMPARE(sink.apply({{"type", "key"}, {"code", "Unknown"}, {"down", true}}), Result::Ignored);
    QCOMPARE(sink.apply({{"type", "button"}, {"button", 0}, {"down", 1}}), Result::Ignored);
    QCOMPARE(sink.apply({{"type", "button"}, {"button", 0.5}, {"down", true}}), Result::Ignored);
    QCOMPARE(sink.apply({{"type", "button"}, {"button", 3}, {"down", true}}), Result::Ignored);
    QCOMPARE(sink.apply({{"type", "ping"}}), Result::Applied);
    QVERIFY(sent.empty());
    QCOMPARE(sink.apply(move), Result::Applied);
    QCOMPARE(sent.at(0).type, DWORD(INPUT_MOUSE));
    QCOMPARE(sent.at(0).mi.dwFlags, DWORD(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE));
    QCOMPARE(sent.at(0).mi.dx, LONG(32768));
    QCOMPARE(sent.at(0).mi.dy, LONG(65535));
    QCOMPARE(sink.apply({{"type", "wheel"}, {"delta", -120}}), Result::Applied);
    QCOMPARE(sent.at(1).mi.dwFlags, DWORD(MOUSEEVENTF_WHEEL));
    QCOMPARE(sent.at(1).mi.mouseData, DWORD(-120));
}

void HostInputProbe::releasesHeldInput() {
    std::vector<INPUT> sent;
    HostInput sink([&](const INPUT& event) {
        sent.push_back(event);
        return true;
    }, [] { return QString{}; });
    QCOMPARE(sink.arm(), QString{});
    const QJsonObject ctrl{{"type", "key"}, {"code", "ControlRight"}, {"down", true}};
    const QJsonObject a{{"type", "key"}, {"code", "KeyA"}, {"down", true}};
    const QJsonObject middle{{"type", "button"}, {"button", 1}, {"down", true}};
    QCOMPARE(sink.apply({{"type", "key"}, {"code", "KeyA"}, {"down", false}}), Result::Ignored);
    QCOMPARE(sink.apply(ctrl), Result::Applied);
    QCOMPARE(sink.apply(a), Result::Applied);
    QCOMPARE(sink.apply(a), Result::Applied);
    QCOMPARE(sink.apply(middle), Result::Applied);
    QCOMPARE(sink.apply(middle), Result::Ignored);
    QCOMPARE(sink.apply({{"type", "release"}}), Result::Applied);
    QCOMPARE(sent.size(), size_t(7));
    QCOMPARE(sent.at(0).type, DWORD(INPUT_KEYBOARD));
    QCOMPARE(sent.at(0).ki.wScan, WORD(0x1d));
    QCOMPARE(sent.at(0).ki.dwFlags, DWORD(KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY));
    QCOMPARE(sent.at(1).ki.wScan, WORD(0x1e));
    QCOMPARE(sent.at(2).ki.wScan, WORD(0x1e));
    QCOMPARE(sent.at(3).mi.dwFlags, DWORD(MOUSEEVENTF_MIDDLEDOWN));
    QCOMPARE(sent.at(4).mi.dwFlags, DWORD(MOUSEEVENTF_MIDDLEUP));
    QCOMPARE(sent.at(5).ki.wScan, WORD(0x1e));
    QCOMPARE(sent.at(5).ki.dwFlags, DWORD(KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP));
    QCOMPARE(sent.at(6).ki.wScan, WORD(0x1d));
    QCOMPARE(sent.at(6).ki.dwFlags, DWORD(KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP));
    QVERIFY(sink.disarm());
    QCOMPARE(sent.size(), size_t(7));
    QCOMPARE(sink.apply(a), Result::Ignored);
}

void HostInputProbe::failsClosedAndRetriesRelease() {
    std::vector<INPUT> sent;
    QString environment = QStringLiteral("Test desktop unavailable");
    bool accept = true;
    HostInput sink([&](const INPUT& event) {
        if (accept)
            sent.push_back(event);
        return accept;
    }, [&] { return environment; });
    QCOMPARE(sink.arm(), environment);
    const QJsonObject a{{"type", "key"}, {"code", "KeyA"}, {"down", true}};
    QCOMPARE(sink.apply(a), Result::Ignored);
    environment.clear();
    QCOMPARE(sink.arm(), QString{});
    QCOMPARE(sink.apply(a), Result::Applied);
    accept = false;
    QCOMPARE(sink.apply({{"type", "wheel"}, {"delta", 120}}), Result::Failed);
    QCOMPARE(sink.apply(a), Result::Ignored);
    QVERIFY(!sink.disarm());
    QVERIFY(!sink.arm().isEmpty());
    QCOMPARE(sent.size(), size_t(1));
    accept = true;
    QVERIFY(sink.disarm());
    QCOMPARE(sent.size(), size_t(2));
    QCOMPARE(sent.at(1).ki.dwFlags, DWORD(KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP));
    QCOMPARE(sink.arm(), QString{});
    QCOMPARE(sink.apply(a), Result::Applied);
    environment = QStringLiteral("Test desktop unavailable");
    QCOMPARE(sink.apply({{"type", "ping"}}), Result::Failed);
    QCOMPARE(sink.apply(a), Result::Ignored);
    QCOMPARE(sent.size(), size_t(4));
    QCOMPARE(sent.at(3).ki.dwFlags, DWORD(KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP));
}
} // namespace

QTEST_GUILESS_MAIN(HostInputProbe)
#include "host_input_probe.moc"
