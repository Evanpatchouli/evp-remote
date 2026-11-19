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

    void configure(const QString& key) const {
        home_.showDevicePage();
        if (!key.isEmpty()) {
            field("deviceDeploymentKey")->setText(key);
            button("configureDevices")->click();
        }
    }

private:
    HomeWindow& home_;
};

}
