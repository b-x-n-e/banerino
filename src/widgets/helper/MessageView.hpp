// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/Message.hpp"
#include "providers/links/LinkInfo.hpp"
#include "widgets/BaseWidget.hpp"


#include <QWidget>

namespace chatterino {

class MessageLayout;
class Link;
class TooltipWidget;



/// MessageView is a fixed-width widget that displays a single message.
/// For the message to be rendered, you must call setWidth.
class MessageView : public BaseWidget
{
    Q_OBJECT

public:
    MessageView();
    ~MessageView() override;
    MessageView(const MessageView &) = delete;
    MessageView(MessageView &&) = delete;
    MessageView &operator=(const MessageView &) = delete;
    MessageView &operator=(MessageView &&) = delete;

    void setMessage(const MessagePtr &message);
    /// Sets the message directly, preserving all its elements (badges, colors, emotes).
    void setMessageDirect(const MessagePtr &message);
    void clearMessage();

    void setWidth(int width);
    int getHeight() const;


    /// Override the element flags used for layout.
    void setFlags(MessageElementFlags flags);

    void setTooltipWidget(TooltipWidget *tooltipWidget);

Q_SIGNALS:

    void linkClicked(const Link &link);



protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;
    void themeChangedEvent() override;

    void scaleChangedEvent(float newScale) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;


private:
    void createMessageLayout();
    void layoutMessage();

    MessagePtr message_;
    std::unique_ptr<MessageLayout> messageLayout_;

    MessageColors messageColors_;
    MessagePreferences messagePreferences_;

    int width_{};
    std::optional<MessageElementFlags> customFlags_;
    TooltipWidget *tooltipWidget_{};

    std::map<QString, std::shared_ptr<LinkInfo>> linkInfos_;
    void resolveLink(const QString &url, const QPoint &pos);
};



}  // namespace chatterino
