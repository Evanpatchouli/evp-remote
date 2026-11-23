#include "theme.h"

#include <QApplication>
#include <QApplicationStatic>
#include <QEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QRegularExpression>
#include <QSettings>
#include <QWidget>
#include <array>
#include <cmath>

namespace native_ui {
namespace {

constexpr auto themeSetting = "appearance/theme";
constexpr auto fontSetting = "appearance/font";
constexpr std::array colorSettings{"accent", "background", "foreground"};
constexpr std::array lightColors{"#2463da", "#f7f9fd", "#182844"};
constexpr std::array darkColors{"#2355ad", "#191a1c", "#e6e8ec"};

QString colorKey(bool dark, ThemeColor color) {
    return QStringLiteral("appearance/%1/%2").arg(dark ? "dark" : "light", colorSettings.at(static_cast<int>(color)));
}

QColor mix(const QColor& base, const QColor& tint, double amount) {
    const auto channel = [amount](int from, int to) { return qRound(from + (to - from) * amount); };
    return QColor(channel(base.red(), tint.red()), channel(base.green(), tint.green()), channel(base.blue(), tint.blue()));
}

double luminance(const QColor& color) {
    const auto linear = [](float channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}

QColor contrastingText(const QColor& background) {
    const QColor light("#eef0f5"), dark("#121317");
    return (luminance(light) + 0.05) / (luminance(background) + 0.05) >
           (luminance(background) + 0.05) / (luminance(dark) + 0.05) ? light : dark;
}

class ThemeState final : public QObject {
    Q_OBJECT

public:
    ThemeState() : dark_(resolveDark()) {
        QCoreApplication::instance()->installEventFilter(this);
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, &ThemeState::refresh, Qt::QueuedConnection);
    }

    bool isDark() const { return dark_; }
    QString font() const { return font_; }

    QColor color(ThemeColor role) const {
        const QColor custom = colors_.at(static_cast<int>(role));
        if (custom.isValid())
            return custom;
        if (role == ThemeColor::Foreground && colors_.at(static_cast<int>(ThemeColor::Background)).isValid())
            return contrastingText(color(ThemeColor::Background));
        return QColor((dark_ ? darkColors : lightColors).at(static_cast<int>(role)));
    }

    QString styleColor(QStringView light, QStringView dark, QStringView role) const {
        const auto fallback = dark_ ? dark : light;
        if (role.isEmpty())
            return fallback.toString();
        const QColor customAccent = colors_.at(static_cast<int>(ThemeColor::Accent));
        const QColor background = colors_.at(static_cast<int>(ThemeColor::Background));
        const bool customizedForeground = background.isValid() || colors_.at(static_cast<int>(ThemeColor::Foreground)).isValid();
        if (role == u"foreground" && customizedForeground)
            return color(ThemeColor::Foreground).name();
        if (role == u"muted" && customizedForeground)
            return mix(color(ThemeColor::Foreground), color(ThemeColor::Background), 0.28).name();
        if (background.isValid()) {
            if (role == u"background")
                return background.name();
            // The window, cards, and controls stay distinguishable around the chosen base.
            const QColor lift = background.lightness() < 128 ? QColor("#ffffff") : QColor("#000000");
            if (role == u"surface")
                return mix(background, lift, 0.045).name();
            if (role == u"raised")
                return mix(background, lift, 0.085).name();
            if (role == u"stroke")
                return mix(background, lift, 0.20).name();
        }
        if (customAccent.isValid()) {
            if (role == u"accent")
                return customAccent.name();
            if (role == u"accentHover")
                return mix(customAccent, contrastingText(customAccent), 0.12).name();
            if (role == u"accentPressed")
                return mix(customAccent, contrastingText(customAccent), 0.23).name();
            if (role == u"onAccent")
                return contrastingText(customAccent).name();
        }
        if ((customAccent.isValid() || background.isValid()) && (role == u"accentTint" || role == u"accentHoverTint"))
            return mix(color(ThemeColor::Background), color(ThemeColor::Accent), role == u"accentTint" ? 0.21 : 0.30).name();
        if ((role == u"accentText" || role == u"accentMark") && (customAccent.isValid() || customizedForeground)) {
            const auto base = color(ThemeColor::Background);
            const auto ink = customAccent.isValid() ? customAccent : color(ThemeColor::Foreground);
            const auto ratio = (qMax(luminance(base), luminance(ink)) + 0.05) /
                               (qMin(luminance(base), luminance(ink)) + 0.05);
            const auto minimum = role == u"accentText" ? 6.0 : 3.0;
            return (ratio >= minimum ? ink : mix(ink, contrastingText(base), 0.85)).name();
        }
        return fallback.toString();
    }

    void apply(Theme choice) {
        choice_ = choice;
        const auto scheme = choice == Theme::Light ? Qt::ColorScheme::Light
                          : choice == Theme::Dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Unknown;
        QGuiApplication::styleHints()->setColorScheme(scheme);
        refresh();
    }

    void refresh() {
        const bool dark = resolveDark();
        const QSettings settings;
        const std::array colors{
            QColor(settings.value(colorKey(dark, ThemeColor::Accent)).toString()),
            QColor(settings.value(colorKey(dark, ThemeColor::Background)).toString()),
            QColor(settings.value(colorKey(dark, ThemeColor::Foreground)).toString()),
        };
        const auto family = settings.value(fontSetting).toString();
        if (dark_ == dark && colors_ == colors && font_ == family)
            return;
        dark_ = dark;
        colors_ = colors;
        if (font_ != family) {
            font_ = family;
            auto escaped = family;
            escaped.replace(u'\\', QStringLiteral("\\\\")).replace(u'"', QStringLiteral("\\\""));
            qApp->setStyleSheet(family.isEmpty() ? QString{} : QStringLiteral("* { font-family: \"%1\"; }").arg(escaped));
        }
        emit changed();
    }

signals:
    void changed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == QCoreApplication::instance() && event->type() == QEvent::ApplicationPaletteChange)
            QMetaObject::invokeMethod(this, &ThemeState::refresh, Qt::QueuedConnection);
        return false;
    }

private:
    bool resolveDark() const {
        if (choice_ != Theme::System)
            return choice_ == Theme::Dark;
        const auto scheme = QGuiApplication::styleHints()->colorScheme();
        if (scheme != Qt::ColorScheme::Unknown)
            return scheme == Qt::ColorScheme::Dark;
        // Windows contrast themes can report an unknown scheme.
        return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    }

    Theme choice_ = Theme::System;
    bool dark_;
    std::array<QColor, 3> colors_;
    QString font_;
};

Q_APPLICATION_STATIC(ThemeState, themeState)

}

Theme savedTheme() {
    const auto value = QSettings().value(themeSetting).toString();
    if (value == QStringLiteral("light")) return Theme::Light;
    if (value == QStringLiteral("dark")) return Theme::Dark;
    return Theme::System;
}

void applyTheme(Theme theme) {
    themeState->apply(theme);
}

void saveTheme(Theme theme) {
    QSettings settings;
    if (theme == Theme::System)
        settings.remove(themeSetting);
    else
        settings.setValue(themeSetting, theme == Theme::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
    applyTheme(theme);
}

QColor appearanceColor(ThemeColor color) {
    return themeState->color(color);
}

void saveAppearanceColor(ThemeColor color, const QColor& value) {
    if (!value.isValid())
        return;
    QSettings().setValue(colorKey(themeState->isDark(), color), value.name());
    themeState->refresh();
}

QString appearanceFont() {
    return themeState->font();
}

void saveAppearanceFont(const QString& family) {
    QSettings settings;
    if (family.isEmpty())
        settings.remove(fontSetting);
    else
        settings.setValue(fontSetting, family);
    themeState->refresh();
}

void resetAppearance() {
    QSettings settings;
    for (const auto role : {ThemeColor::Accent, ThemeColor::Background, ThemeColor::Foreground})
        settings.remove(colorKey(themeState->isDark(), role));
    settings.remove(fontSetting);
    themeState->refresh();
}

QColor themeColor(const char* light, const char* dark, const char* role) {
    return QColor(themeState->styleColor(QString::fromLatin1(light), QString::fromLatin1(dark), QString::fromLatin1(role)));
}

QString themeStyle(QString style) {
    // Keep the light and dark values next to each other in rules that use custom colors.
    static const QRegularExpression pair(
        QStringLiteral(R"((#[0-9a-f]{6}|white|palette\([a-z-]+\))/(#[0-9a-f]{6})(?:@([a-zA-Z]+))?)"));
    QString resolved;
    qsizetype cursor = 0;
    auto matches = pair.globalMatch(style);
    while (matches.hasNext()) {
        const auto match = matches.next();
        resolved += QStringView(style).sliced(cursor, match.capturedStart() - cursor);
        resolved += themeState->styleColor(match.capturedView(1), match.capturedView(2), match.capturedView(3));
        cursor = match.capturedEnd();
    }
    resolved += QStringView(style).sliced(cursor);
    return resolved;
}

void onThemeChanged(QWidget& widget, const std::function<void()>& update) {
    update();
    QObject::connect(themeState(), &ThemeState::changed, &widget, update);
}

void setThemedStyle(QWidget& widget, const QString& style) {
    onThemeChanged(widget, [&widget, style] { widget.setStyleSheet(themeStyle(style)); });
}

}

#include "theme.moc"
