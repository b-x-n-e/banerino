#pragma once

#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include "widgets/buttons/SignalLabel.hpp"
#include <QPushButton>


#include <memory>

class QLabel;

namespace chatterino {


class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
class Split;
class TwitchChannel;

class MessageView;
class SvgButton;
class Link;
class TooltipWidget;





class PinnedMessageWidget : public BaseWidget
{
    Q_OBJECT

public:
    explicit PinnedMessageWidget(Split *parent);

    void setChannel(std::shared_ptr<Channel> channel);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void themeChangedEvent() override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void updatePin();
    void onLinkClicked(const Link &link);

    Split *split_;
    QLabel *headerPreLabel_{};
    SignalLabel *headerNameLabel_{};
    QLabel *headerPostLabel_{};

    MessageView *messageView_{};
    SvgButton *hideButton_{};
    SvgButton *closeButton_{};
    TooltipWidget *tooltipWidget_{};



    TwitchChannel *currentChannel_ = nullptr;
    pajlada::Signals::SignalHolder signalHolder_;

    QString hiddenPinId_;
    QString pinnerLogin_;
    bool minimized_ = false;
};


}  // namespace chatterino
