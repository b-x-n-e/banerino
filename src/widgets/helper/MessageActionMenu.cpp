#include "widgets/helper/MessageActionMenu.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/buttons/SvgButton.hpp"

#include <QPainter>
#include <QTimer>

namespace chatterino {

MessageActionMenu::MessageActionMenu(ChannelView *parent)
    : BaseWidget(parent)
    , view_(parent)
{
    this->initLayout();
    this->setMouseTracking(true);
    
    // Hide initially
    this->hide();
}

void MessageActionMenu::initLayout()
{
    this->layout_ = new QHBoxLayout(this);
    this->layout_->setContentsMargins(2, 2, 2, 2);
    this->layout_->setSpacing(2);

    auto makeSvgBtn = [this](const QString &tooltip) {
        auto *btn = new SvgButton({"", ""}, this, QSize(3, 3)); // 3px margin perfectly sizes the icon
        btn->setToolTip(tooltip);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedSize(20, 20);
        this->layout_->addWidget(btn);
        return btn;
    };

    this->copyBtn_ = makeSvgBtn("Copy message");
    QObject::connect(this->copyBtn_, &Button::leftClicked, this, [this]() {
        if (this->targetMessage_) {
            crossPlatformCopy(this->targetMessage_->messageText);
        }
    });

    this->replyBtn_ = makeSvgBtn("Reply to message");
    QObject::connect(this->replyBtn_, &Button::leftClicked, this, [this]() {
        if (this->targetMessage_ && this->view_) {
            this->view_->setInputReply(this->targetMessage_);
        }
    });

    this->pinBtn_ = makeSvgBtn("Pin message");
    QObject::connect(this->pinBtn_, &Button::leftClicked, this, [this]() {
        if (this->targetMessage_ && this->view_) {
            auto *twitchChannel = dynamic_cast<TwitchChannel *>(this->view_->underlyingChannel().get());
            if (twitchChannel) {
                twitchChannel->pinMessage(this->targetMessage_->id);
            }
        }
    });

    this->deleteBtn_ = makeSvgBtn("Delete message");
    QObject::connect(this->deleteBtn_, &Button::leftClicked, this, [this]() {
        if (this->targetMessage_ && this->view_) {
            auto *twitchChannel = dynamic_cast<TwitchChannel *>(this->view_->underlyingChannel().get());
            if (twitchChannel) {
                twitchChannel->deleteMessagesAs(
                    this->targetMessage_->id, 
                    getApp()->getAccounts()->twitch.getCurrent().get());
            }
        }
    });
}

void MessageActionMenu::setTarget(MessagePtr message)
{
    this->targetMessage_ = message;
    this->updateButtons();
}

void MessageActionMenu::updateButtons()
{
    if (!this->targetMessage_ || !this->view_) return;

    bool isMod = this->view_->underlyingChannel()->hasModRights();
    
    // Only show Pin and Delete if we are mod
    this->pinBtn_->setVisible(isMod);
    this->deleteBtn_->setVisible(isMod);

    // Disable reply if message cannot be replied to
    bool canReply = true;
    if (this->targetMessage_->flags.has(MessageFlag::System) ||
        this->targetMessage_->flags.has(MessageFlag::Timeout) ||
        this->targetMessage_->id.isEmpty()) {
        canReply = false;
    }
    this->replyBtn_->setVisible(canReply);
    
    // Adjust overall width dynamically
    this->adjustSize();
}

void MessageActionMenu::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    bool dark = this->theme->isLightTheme() == false;

    // Use highly polished modern colors for the pill background
    QColor bgColor = dark ? QColor("#18181B") : QColor("#F4F4F5");
    
    QPainterPath path;
    qreal radius = (this->rect().height() - 2) / 2.0; // Perfect pill shape
    path.addRoundedRect(this->rect().adjusted(1, 1, -1, -1), radius, radius);
    painter.fillPath(path, bgColor);

    // Subtle crisp border
    QColor borderColor = dark ? QColor("#27272A") : QColor("#E4E4E7");
    QPen pen(borderColor, 1);
    painter.setPen(pen);
    painter.drawPath(path);
}

void MessageActionMenu::enterEvent(QEnterEvent *event)
{
    this->isHovering_ = true;
    BaseWidget::enterEvent(event);
}

void MessageActionMenu::leaveEvent(QEvent *event)
{
    this->isHovering_ = false;
    BaseWidget::leaveEvent(event);
    
    // Hide when leaving the menu unless we entered another child or the message bounds
    // The ChannelView will handle hiding it if we truly leave the message.
    this->hide();
}

void MessageActionMenu::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();

    this->copyBtn_->setSource({":/buttons/copyAction.svg", ":/buttons/copyAction.svg"});
    this->replyBtn_->setSource({":/buttons/replyAction.svg", ":/buttons/replyAction.svg"});
    this->deleteBtn_->setSource({":/buttons/trashAction.svg", ":/buttons/trashAction.svg"});
    this->pinBtn_->setSource({":/buttons/pinAction.svg", ":/buttons/pinAction.svg"});
    
    // Make sure all SVG icons take on the color of standard text, ensuring they always look native
    QColor iconColor = this->theme->messages.textColors.system;
    this->copyBtn_->setColor(iconColor);
    this->replyBtn_->setColor(iconColor);
    this->deleteBtn_->setColor(iconColor);
    this->pinBtn_->setColor(iconColor);
}

} // namespace chatterino
