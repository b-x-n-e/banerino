#pragma once

#include "widgets/BaseWidget.hpp"
#include "messages/Message.hpp"

#include <QWidget>
#include <QHBoxLayout>

namespace chatterino {

class ChannelView;
class SvgButton;
class PixmapButton;

class MessageActionMenu : public BaseWidget
{
    Q_OBJECT

public:
    explicit MessageActionMenu(ChannelView *parent);

    void setTarget(MessagePtr message);
    MessagePtr getTarget() const { return targetMessage_; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void themeChangedEvent() override;

private:
    void initLayout();
    void updateButtons();

    ChannelView *view_;
    MessagePtr targetMessage_;

    QHBoxLayout *layout_;
    SvgButton *copyBtn_;
    SvgButton *replyBtn_;
    SvgButton *pinBtn_;
    SvgButton *deleteBtn_;

    bool isHovering_{false};
};

} // namespace chatterino
