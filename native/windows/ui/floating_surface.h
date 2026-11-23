#pragma once

class QWidget;
class QString;

namespace native_ui {

void clearWindowDecoration(QWidget& window);
void styleFloatingSurface(QWidget& window, QWidget& surface);
void attachThemedToolTip(QWidget& anchor, const QString& text);

}
