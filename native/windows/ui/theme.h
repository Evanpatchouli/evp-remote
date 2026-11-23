#pragma once

#include <QColor>
#include <QStyleHints>
#include <QString>
#include <functional>

class QWidget;

namespace native_ui {

enum class Theme { System, Light, Dark };
enum class ThemeColor { Accent, Background, Foreground };

Theme savedTheme();
void applyTheme(Theme theme);
void saveTheme(Theme theme);
QColor appearanceColor(ThemeColor color);
void saveAppearanceColor(ThemeColor color, const QColor& value);
QString appearanceFont();
void saveAppearanceFont(const QString& family);
void resetAppearance();

QColor themeColor(const char* light, const char* dark, const char* role = "");
QString themeStyle(QString style);
void onThemeChanged(QWidget& widget, const std::function<void()>& update);
void setThemedStyle(QWidget& widget, const QString& style);

}
