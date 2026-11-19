#pragma once

class QWidget;

namespace native_ui {

void clearWindowDecoration(QWidget& window);
void styleFloatingSurface(QWidget& window, QWidget& surface);

}
