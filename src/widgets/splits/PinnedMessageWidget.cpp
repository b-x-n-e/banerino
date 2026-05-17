#include "widgets/splits/PinnedMessageWidget.hpp"

#include "common/LinkParser.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Theme.hpp"
#include "util/Helpers.hpp"
#include "util/WidgetHelpers.hpp"
#include <QLabel>
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/MessageView.hpp"
#include "widgets/splits/Split.hpp"
#include "singletons/Settings.hpp"
#include "Application.hpp"
#include "singletons/WindowManager.hpp"

#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/TooltipWidget.hpp"
#include <QHBoxLayout>

#include <QVBoxLayout>
#include <QPainter>
#include <QDesktopServices>
#include <QUrl>


namespace chatterino {

PinnedMessageWidget::PinnedMessageWidget(Split *parent)
    : BaseWidget(parent)
    , split_(parent)
{
    this->setFixedHeight(0);
    this->hide();

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Inner container for padding
    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(10, 2, 8, 2);
    layout->setSpacing(2);

    // Header layout
    auto *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);

    auto *pinIcon = new SvgButton({":/buttons/pinEnabled.svg", ":/buttons/pinEnabled.svg"}, this, QSize(2, 2));
    pinIcon->setFixedSize(18, 18);
    pinIcon->setToolTip("Pinned Message");
    headerLayout->addWidget(pinIcon);
    headerLayout->addSpacing(3);

    this->headerPreLabel_ = new QLabel("Pinned by ", this);
    this->headerPreLabel_->setStyleSheet("color: #aaa; margin: 0; padding: 0;");
    this->headerPreLabel_->setCursor(Qt::PointingHandCursor);
    this->headerPreLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    headerLayout->addWidget(this->headerPreLabel_);

    this->headerNameLabel_ = new SignalLabel(this);
    this->headerNameLabel_->setStyleSheet("font-weight: bold; margin: 0; padding: 0;");
    this->headerNameLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    headerLayout->addWidget(this->headerNameLabel_);

    this->headerPostLabel_ = new QLabel(this);
    this->headerPostLabel_->setStyleSheet("color: #aaa; margin: 0; padding: 0;");
    this->headerPostLabel_->setCursor(Qt::PointingHandCursor);
    headerLayout->addWidget(this->headerPostLabel_, 1);



    this->hideButton_ = new SvgButton({":/buttons/hidePin.svg", ":/buttons/hidePin.svg"}, this, QSize(2, 2));
    this->hideButton_->setFixedSize(22, 22);
    this->hideButton_->setToolTip("Hide Pin Locally");
    headerLayout->addWidget(this->hideButton_);

    this->closeButton_ = new SvgButton({":/buttons/unpinMessage.svg", ":/buttons/unpinMessage.svg"}, this, QSize(2, 2));
    this->closeButton_->setFixedSize(22, 22);
    this->closeButton_->setToolTip("Unpin for everyone (Mod only)");
    headerLayout->addWidget(this->closeButton_);

    layout->addLayout(headerLayout);

    // Message View
    this->messageView_ = new MessageView();
    // Enable full rendering: badges, username, timestamp, emotes, text
    this->messageView_->setFlags(MessageElementFlags{
        MessageElementFlag::Timestamp,
        MessageElementFlag::Badges,
        MessageElementFlag::Username,
        MessageElementFlag::Text,
        MessageElementFlag::EmoteImage,
        MessageElementFlag::EmojiAll,
        MessageElementFlag::AlwaysShow,
    });
    layout->addWidget(this->messageView_);

    this->tooltipWidget_ = new TooltipWidget(this);
    this->messageView_->setTooltipWidget(this->tooltipWidget_);

    mainLayout->addWidget(container);


    QObject::connect(this->hideButton_, &SvgButton::clicked, [this]() {
        if (this->currentChannel_ && this->currentChannel_->getPinnedMessage().has_value())
        {
            this->hiddenPinId_ = this->currentChannel_->getPinnedMessage()->pinId;
            this->hide();
        }
    });

    QObject::connect(this->headerNameLabel_, &SignalLabel::leftMouseUp, [this]() {

        if (!this->pinnerLogin_.isEmpty())
        {
            auto *userPopup = new UserInfoPopup(
                getSettings()->autoCloseUserPopup, this->split_);
            userPopup->setData(this->pinnerLogin_, this->split_->getChannel());
            userPopup->show();
            userPopup->moveTo(QCursor::pos(),
                              widgets::BoundsChecking::DesiredPosition);
        }
    });

    QObject::connect(this->closeButton_, &SvgButton::clicked, [this]() {
        if (this->currentChannel_)
        {
            this->currentChannel_->unpinMessage();
        }
    });

    // Click header labels (pre/post) to toggle minimize
    this->headerPreLabel_->installEventFilter(this);
    this->headerPostLabel_->installEventFilter(this);


    QObject::connect(this->messageView_, &MessageView::linkClicked, [this](const Link &link) {
        this->onLinkClicked(link);
    });

    this->setLayout(mainLayout);

    // Subscribe to global gif repaint signal to prevent animated emotes from stuttering
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->gifRepaintRequested, [this] {
            if (this->isVisible() && this->messageView_ && this->messageView_->isVisible())
            {
                this->messageView_->update();
            }
        });

    this->themeChangedEvent();
}

void PinnedMessageWidget::setChannel(std::shared_ptr<Channel> channel)
{
    this->signalHolder_.clear();
    
    // Re-subscribe to global gif repaint signal since clear() removed it
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->gifRepaintRequested, [this] {
            if (this->isVisible() && this->messageView_ && this->messageView_->isVisible())
            {
                this->messageView_->update();
            }
        });

    this->currentChannel_ = nullptr;

    auto *tc = dynamic_cast<TwitchChannel *>(channel.get());
    if (!tc)
    {
        this->hide();
        return;
    }

    this->currentChannel_ = tc;

    this->signalHolder_.managedConnect(tc->pinnedMessageUpdated, [this]() {
        this->updatePin();
    });

    // Check for existing pin
    this->updatePin();
}

void PinnedMessageWidget::updatePin()
{
    if (!this->currentChannel_)
    {
        this->hide();
        return;
    }

    const auto &pin = this->currentChannel_->getPinnedMessage();
    if (!pin.has_value())
    {
        this->hide();
        return;
    }

    if (this->hiddenPinId_ == pin->pinId)
    {
        this->hide();
        return;
    }

    QString pinnerStr = pin->pinnerName.isEmpty() ? pin->pinnerLogin : pin->pinnerName;
    if (pinnerStr.isEmpty()) pinnerStr = "a moderator";
    QDateTime displayTime = pin->pinnedAt.isValid() ? pin->pinnedAt : pin->sentAt;
    QString timeStr = displayTime.toLocalTime().time().toString("h:mm AP");

    this->pinnerLogin_ = pin->pinnerLogin;
    this->headerNameLabel_->setText(pinnerStr);
    this->headerPostLabel_->setText(QString(" at %1").arg(timeStr));
    this->headerNameLabel_->setCursor(Qt::PointingHandCursor);



    // Try to find the message in chat history to retain badges and paints
    MessagePtr msg = this->currentChannel_->findMessageByID(pin->messageId);
    if (!msg)
    {
        // Build a fallback message with colored username
        auto displayName = pin->senderName.isEmpty() ? pin->senderLogin : pin->senderName;
        QColor userColor = pin->chatColor.isEmpty()
                               ? getRandomColor(pin->senderId)
                               : QColor(pin->chatColor);

        MessageBuilder builder;
        builder.message().flags.set(MessageFlag::PubSub);
        
        builder.emplace<TimestampElement>(pin->sentAt.toLocalTime().time());
        
        auto usernameText = new TextElement(
            displayName + ":", MessageElementFlag::Username,
            MessageColor(userColor), FontStyle::ChatMediumBold);
        usernameText->setLink(Link(Link::UserInfo, pin->senderLogin));
        builder.append(std::unique_ptr<MessageElement>(usernameText));

        // Parse text word-by-word to detect URLs and make them clickable
        auto words = pin->text.split(' ', Qt::SkipEmptyParts);
        for (const auto &word : words)
        {
            auto parsed = linkparser::parse(word);
            if (parsed)
            {
                auto linkStr = parsed->link.toString();
                if (!linkStr.startsWith("http"))
                {
                    linkStr = "https://" + linkStr;
                }
                auto *linkElem = new TextElement(
                    word, MessageElementFlag::Text,
                    MessageColor(QColor("#00b5f0")),
                    FontStyle::ChatMedium);
                linkElem->setLink(Link(Link::Url, linkStr));
                builder.append(std::unique_ptr<MessageElement>(linkElem));
            }
            else
            {
                builder.append(std::make_unique<TextElement>(
                    word, MessageElementFlag::Text, MessageColor::Text));
            }
        }

        msg = builder.release();
    }

    this->messageView_->setMessageDirect(msg);
    this->messageView_->setWidth(this->width() - 24);

    // Only show close button if user has mod rights
    this->closeButton_->setVisible(this->currentChannel_->hasModRights());

    if (this->minimized_)
    {
        this->messageView_->setVisible(false);
        this->setMinimumHeight(0);
        this->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    else
    {
        this->messageView_->setVisible(true);
        this->setMinimumHeight(0);
        this->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    this->show();
}

void PinnedMessageWidget::paintEvent(QPaintEvent *event)
{
    BaseWidget::paintEvent(event);

    QPainter painter(this);
    auto border = this->theme->splits.header.border;
    auto accent = this->theme->accent;

    // Draw full outline border
    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 1);

    // Draw accent left line
    painter.fillRect(0, 0, 4, this->height(), accent);
}

void PinnedMessageWidget::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    if (this->messageView_)
    {
        // Qt Layouts handle height automatically based on MessageView's fixed height
        this->messageView_->setWidth(this->width() - 24);
    }
}

void PinnedMessageWidget::themeChangedEvent()
{
    auto bg = this->theme->splits.background;
    this->setStyleSheet(QString("PinnedMessageWidget { background-color: %1; }").arg(bg.darker(110).name()));
    
    QColor systemColor = this->theme->messages.textColors.system;

    if (this->headerNameLabel_) {
        this->headerNameLabel_->setStyleSheet(QString("font-size: 11px; font-weight: bold; color: %1;").arg(systemColor.name()));
    }

    if (this->hideButton_) {
        this->hideButton_->setColor(systemColor);
    }
    
    if (this->closeButton_) {
        this->closeButton_->setColor(systemColor);
    }

    if (this->currentChannel_ && this->currentChannel_->getPinnedMessage().has_value())
    {
        this->updatePin();
    }
}

void PinnedMessageWidget::onLinkClicked(const Link &link)
{
    switch (link.type)
    {
        case Link::UserInfo: {
            auto *userPopup = new UserInfoPopup(
                getSettings()->autoCloseUserPopup, this->split_);
            userPopup->setData(link.value, this->split_->getChannel());
            userPopup->show();
            userPopup->moveTo(QCursor::pos(),
                              widgets::BoundsChecking::DesiredPosition);
        }

        break;

        case Link::Url: {
            QDesktopServices::openUrl(QUrl(link.value));
        }
        break;

        default:
            break;
    }
}

bool PinnedMessageWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease &&
        (obj == this->headerPreLabel_ || obj == this->headerPostLabel_))
    {
        this->minimized_ = !this->minimized_;
        this->updatePin();
        return true;
    }
    return BaseWidget::eventFilter(obj, event);
}

}  // namespace chatterino

