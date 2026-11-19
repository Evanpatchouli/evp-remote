#pragma once

#include "ui/device_page.h"
#include "ui/home_window.h"
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <cstdio>

namespace device_probe {

inline QString storePath(const QString& fallback) {
    return qEnvironmentVariable("EVP_PROBE_DEVICE_STORE", QDir(fallback).filePath("devices.dat"));
}

inline void print(const QJsonObject& report) {
    std::fprintf(stdout, "%s\n", QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    std::fflush(stdout);
}

class Ui {
public:
    explicit Ui(HomeWindow& home) : home_(home) {}

    QPushButton* button(const char* name) const { return home_.findChild<QPushButton*>(name); }
    QLineEdit* field(const char* name) const { return home_.findChild<QLineEdit*>(name); }
    bool online() const { return home_.devices().property("deviceRegistered").toBool(); }
    QString id() const { return field("localDeviceId")->text(); }
    QString peer() const {
        const auto* item = home_.findChild<QListWidget*>("trustedDeviceList")->currentItem();
        return item ? item->data(Qt::UserRole).toString() : QString{};
    }
    QString message() const { return home_.findChild<QLabel*>("deviceMessage")->text(); }

    bool saveSettings(const QUrl& url, const QString& key = {}) const {
        auto* navigation = button("settingsNavigation");
        auto* address = field("settingsServerUrl");
        auto* secret = field("settingsDeploymentKey");
        auto* save = button("saveSettings");
        if (!navigation || !address || !secret || !save)
            return false;
        navigation->click();
        if (!address->isVisible() || !secret->isVisible() || !secret->text().isEmpty() ||
            secret->echoMode() != QLineEdit::Password)
            return false;
        address->setText(url.toString());
        secret->setText(key);
        if (!save->isVisible() || !save->isEnabled())
            return false;
        save->click();
        return secret->text().isEmpty() && (key.isEmpty() || !secret->placeholderText().contains(key));
    }

private:
    HomeWindow& home_;
};

}
