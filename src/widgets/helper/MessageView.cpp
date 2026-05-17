// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/MessageView.hpp"

#include "Application.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"

#include <optional>
#include "providers/colors/ColorProvider.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/WidgetHelpers.hpp"
#include "widgets/TooltipWidget.hpp"
#include "messages/Emote.hpp"



#include <QApplication>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include "providers/links/LinkResolver.hpp"
#include "singletons/Settings.hpp"



namespace {

using namespace chatterino;

const Selection EMPTY_SELECTION;

const MessageElementFlags MESSAGE_FLAGS{
    MessageElementFlag::Text,
    MessageElementFlag::EmojiAll,
    MessageElementFlag::EmoteText,
};

}  // namespace

namespace chatterino {

MessageView::MessageView() = default;
MessageView::~MessageView() = default;

void MessageView::createMessageLayout()
{
    if (this->message_ == nullptr)
    {
        this->messageLayout_.reset();
        return;
    }

    this->messageLayout_ = std::make_unique<MessageLayout>(this->message_);
}

void MessageView::setMessage(const MessagePtr &message)
{
    if (!message)
    {
        return;
    }

    auto singleLineMessage = std::make_shared<Message>();
    singleLineMessage->elements.emplace_back(
        std::make_unique<SingleLineTextElement>(
            message->messageText, MESSAGE_FLAGS, MessageColor::Type::System,
            FontStyle::ChatMediumSmall));
    this->message_ = std::move(singleLineMessage);
    this->createMessageLayout();
    this->layoutMessage();
}

void MessageView::setMessageDirect(const MessagePtr &message)
{
    if (!message)
    {
        return;
    }

    this->message_ = message;
    this->createMessageLayout();
    this->layoutMessage();
}

void MessageView::setFlags(MessageElementFlags flags)
{
    this->customFlags_ = flags;
    this->layoutMessage();
}

void MessageView::setTooltipWidget(TooltipWidget *tooltipWidget)
{
    this->tooltipWidget_ = tooltipWidget;
}


void MessageView::clearMessage()
{
    this->setMessage(nullptr);
}

void MessageView::setWidth(int width)
{
    if (this->width_ != width)
    {
        this->width_ = width;
        this->layoutMessage();
    }
}

void MessageView::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    auto ctx = MessagePaintContext{
        .painter = painter,
        .selection = EMPTY_SELECTION,
        .colorProvider = ColorProvider::instance(),
        .messageColors = this->messageColors_,
        .preferences = this->messagePreferences_,

        .canvasWidth = this->width_,
        .isWindowFocused = this->window() == QApplication::activeWindow(),
        .isMentions = false,

        .y = 0,
        .messageIndex = 0,
        .isLastReadMessage = false,
    };

    this->messageLayout_->paint(ctx);
}

void MessageView::themeChangedEvent()
{
    this->messageColors_.applyTheme(getTheme(), false, 255);
    this->messageColors_.regularBg = getTheme()->splits.input.background;
    if (this->messageLayout_)
    {
        this->messageLayout_->invalidateBuffer();
    }
}

void MessageView::scaleChangedEvent(float newScale)
{
    (void)newScale;

    this->layoutMessage();
}

void MessageView::layoutMessage()
{
    if (this->messageLayout_ == nullptr)
    {
        return;
    }

    auto flags = this->customFlags_.value_or(MESSAGE_FLAGS);
    bool updateRequired = this->messageLayout_->layout(
        {
            .messageColors = this->messageColors_,
            .flags = flags,
            .width = this->width_,
            .scale = this->scale(),
            .imageScale =
                this->scale() * static_cast<float>(this->devicePixelRatio()),
            .selectedChannel = nullptr,
            .message = *this->message_,
        },
        false);

    if (updateRequired)
    {
        this->setFixedSize(this->width_, this->messageLayout_->getHeight());
        this->update();
    }
}

bool MessageView::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip)
    {
        auto *helpEvent = static_cast<QHelpEvent *>(event);
        if (this->messageLayout_)
        {
            auto *element = this->messageLayout_->getElementAt(helpEvent->pos());
            if (element)
            {
                const auto &link = element->getLink();
                if (link.type == Link::Url)
                {
                    if (this->tooltipWidget_)
                    {
                        this->resolveLink(link.value, helpEvent->globalPos());
                        this->tooltipWidget_->setOne({nullptr, link.value});
                        this->tooltipWidget_->moveTo(helpEvent->globalPos(),
                                                     widgets::BoundsChecking::DesiredPosition);
                        this->tooltipWidget_->show();
                    }
                    else
                    {
                        QToolTip::showText(helpEvent->globalPos(), link.value, this);
                    }
                    return true;
                }
            }
        }
    }
    return BaseWidget::event(event);
}

int MessageView::getHeight() const
{
    return this->messageLayout_ ? this->messageLayout_->getHeight() : 0;
}


void MessageView::mousePressEvent(QMouseEvent *event)
{
    if (this->messageLayout_)
    {
        auto *element = this->messageLayout_->getElementAt(event->pos());
        if (element && element->getLink().type != Link::None)
        {
            event->accept();
            return;
        }
    }
    this->BaseWidget::mousePressEvent(event);
}

void MessageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        if (this->messageLayout_)
        {
            auto *element = this->messageLayout_->getElementAt(event->pos());
            if (element)
            {
                const auto &link = element->getLink();
                if (link.type != Link::None)
                {
                    this->linkClicked(link);
                }
            }
        }
    }
    this->BaseWidget::mouseReleaseEvent(event);
}

void MessageView::mouseMoveEvent(QMouseEvent *event)
{
    if (this->messageLayout_)
    {
        auto *element = this->messageLayout_->getElementAt(event->pos());
        if (element && element->getLink().type != Link::None)
        {
            this->setCursor(Qt::PointingHandCursor);
        }
        else
        {
            this->setCursor(Qt::ArrowCursor);
            if (this->tooltipWidget_) {
                this->tooltipWidget_->hide();
            }
        }
    }
    this->BaseWidget::mouseMoveEvent(event);
}

void MessageView::leaveEvent(QEvent *event)
{
    this->setCursor(Qt::ArrowCursor);
    if (this->tooltipWidget_) {
        this->tooltipWidget_->hide();
    }
    this->BaseWidget::leaveEvent(event);
}

void MessageView::resolveLink(const QString &url, const QPoint &pos)
{
    if (!getSettings()->linkInfoTooltip)
    {
        return;
    }

    auto &info = this->linkInfos_[url];
    if (!info)
    {
        info = std::make_shared<LinkInfo>(url);
    }

    if (info->isPending())
    {
        getApp()->getLinkResolver()->resolve(info.get());
    }

    auto updateTooltip = [this, info, pos] {
        if (this->tooltipWidget_ && info->isResolved() && this->isVisible())
        {
            this->tooltipWidget_->setOne({info->thumbnail(), info->tooltip()});
        }
    };

    if (info->isResolved())
    {
        updateTooltip();
    }
    else
    {
        QObject::connect(info.get(), &LinkInfo::stateChanged, this,
                         [updateTooltip](auto) {
                             updateTooltip();
                         });
    }
}


}  // namespace chatterino

