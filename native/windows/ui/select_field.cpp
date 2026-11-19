#include "select_field.h"
#include "floating_surface.h"
#include "visual_style.h"
#include "code_options.h"

#include <QListView>
#include <QScreen>
#include <QStyleOptionComboBox>
#include <QStyledItemDelegate>

namespace native_ui {
namespace {

class OptionDelegate final : public QStyledItemDelegate {
public:
    explicit OptionDelegate(SelectField* field) : QStyledItemDelegate(field), field_(field) {}

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto size = QStyledItemDelegate::sizeHint(option, index);
        return {size.width() + 36, qMax(34, size.height() + 8)};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const bool chosen = index == field_->model()->index(field_->currentIndex(), field_->modelColumn(),
                                                             field_->rootModelIndex());
        const bool highlighted = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect row = option.rect.adjusted(3, 1, -3, -1);
        if (chosen || highlighted || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(highlighted || hovered ? QColor("#e5eeff") : QColor("#f1f5fd"));
            painter->drawRoundedRect(row, 6, 6);
        }
        const QColor ink = !enabled ? QColor("#78869c") : chosen || highlighted ? QColor("#214c98") : QColor("#263955");
        auto font = option.font;
        if (chosen)
            font.setWeight(QFont::DemiBold);
        painter->setFont(font);
        painter->setPen(ink);
        const QRect textArea = row.adjusted(10, 0, -30, 0);
        const auto text = QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, textArea.width());
        painter->drawText(textArea, Qt::AlignLeft | Qt::AlignVCenter, text);
        if (chosen) {
            const QPointF center(row.right() - 14, row.center().y() + 0.5);
            painter->setPen(QPen(ink, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawPolyline(QPolygonF{center + QPointF(-5, 0), center + QPointF(-2, 3),
                                            center + QPointF(5, -4)});
        }
        painter->restore();
    }

private:
    SelectField* field_;
};

}

SelectField::SelectField(QWidget* parent) : QComboBox(parent) {
    setFont(uiFont(14));
    setMinimumSize(128, 38);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral(R"(
        QComboBox {
            color: #182844; background: #ffffff; border: 1px solid #cbd7e8;
            border-radius: 8px; padding: 0 32px 0 12px; combobox-popup: 0;
        }
        QComboBox:hover { background: #f8faff; border-color: #9fb7d8; }
        QComboBox:focus, QComboBox:on { border: 2px solid #3475e4; padding: 0 31px 0 11px; }
        QComboBox:disabled { background: #f4f7fc; color: #71809a; border: 1px solid #dfe6f1; }
        QComboBox::drop-down { width: 30px; border: none; background: transparent; }
        QComboBox::down-arrow { image: none; width: 0; height: 0; }
    )"));
    auto* list = new QListView;
    list->setObjectName("selectOptions");
    list->setFont(font());
    list->setUniformItemSizes(true);
    list->setMouseTracking(true);
    list->viewport()->setAttribute(Qt::WA_Hover);
    list->viewport()->setCursor(Qt::PointingHandCursor);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setStyleSheet(QStringLiteral(
        "QListView#selectOptions { background: #ffffff; border: 1px solid #bdcde4; "
        "border-radius: 9px; padding: 4px; outline: none; }"));
    setView(list);
    setItemDelegate(new OptionDelegate(this));
    auto* popup = view()->window();
    popup->setObjectName("selectPopup");
    popup->setStyleSheet(QStringLiteral("QFrame#selectPopup { background: transparent; border: none; }"));
    styleFloatingSurface(*popup, *list);
}

void SelectField::showPopup() {
    auto* popup = view()->window();
    clearWindowDecoration(*popup);
    view()->setCurrentIndex(model()->index(currentIndex(), modelColumn(), rootModelIndex()));
    QComboBox::showPopup();
    if (!popup->isVisible())
        return;
    const auto margins = popup->contentsMargins();
    const auto anchor = mapToGlobal(QPoint{});
    auto bounds = popup->geometry();
    bounds.setWidth(width() + margins.left() + margins.right());
    bounds.moveLeft(anchor.x() - margins.left());
    bounds.translate(0, bounds.top() < anchor.y() ? margins.bottom() - 4 : 4 - margins.top());
    const auto available = popup->screen()->availableGeometry();
    bounds.setWidth(qMin(bounds.width(), available.width()));
    bounds.setHeight(qMin(bounds.height(), available.height()));
    bounds.moveLeft(qBound(available.left(), bounds.left(), available.right() - bounds.width() + 1));
    bounds.moveTop(qBound(available.top(), bounds.top(), available.bottom() - bounds.height() + 1));
    popup->setGeometry(bounds);
}

void SelectField::paintEvent(QPaintEvent* event) {
    QComboBox::paintEvent(event);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const auto box = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
    const QPointF center = QRectF(box).center();
    const qreal offset = view()->isVisible() ? -2 : 2;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(isEnabled() ? QColor("#456594") : QColor("#99a6ba"), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolyline(QPolygonF{center + QPointF(-4, -offset), center + QPointF(0, offset),
                                  center + QPointF(4, -offset)});
}

SelectField* codeLifetimeSelector(QLabel* label, const char* name) {
    auto* field = new SelectField;
    field->setObjectName(QString::fromLatin1(name));
    field->setAccessibleName(label->text());
    label->setBuddy(field);
    for (int minutes : evp::codeLifetimeMinutes)
        field->addItem(evp::codeLifetimeLabel(minutes), minutes);
    field->setCurrentIndex(field->findData(evp::CodeOptions{}.minutes));
    return field;
}

}
