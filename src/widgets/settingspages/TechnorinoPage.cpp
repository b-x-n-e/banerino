#include "widgets/settingspages/TechnorinoPage.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/CrashHandler.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/NativeMessaging.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <magic_enum/magic_enum.hpp>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontDialog>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QUrlQuery>

namespace {

using namespace chatterino;
using namespace literals;

#ifdef Q_OS_WIN
const QString META_KEY = u"Windows"_s;
#else
const QString META_KEY = u"Meta"_s;
#endif

void addKeyboardModifierSetting(GeneralPageView &layout, const QString &title,
                                EnumSetting<Qt::KeyboardModifier> &setting)
{
    layout.addDropdown<std::underlying_type<Qt::KeyboardModifier>::type>(
        title, {"None", "Shift", "Control", "Alt", META_KEY}, setting,
        [](int index) {
            switch (index)
            {
                case Qt::ShiftModifier:
                    return 1;
                case Qt::ControlModifier:
                    return 2;
                case Qt::AltModifier:
                    return 3;
                case Qt::MetaModifier:
                    return 4;
                default:
                    return 0;
            }
        },
        [](DropdownArgs args) {
            switch (args.index)
            {
                case 1:
                    return Qt::ShiftModifier;
                case 2:
                    return Qt::ControlModifier;
                case 3:
                    return Qt::AltModifier;
                case 4:
                    return Qt::MetaModifier;
                default:
                    return Qt::NoModifier;
            }
        },
        false);
}
}  // namespace

namespace chatterino {

TechnorinoPage::TechnorinoPage()
{
    auto *y = new QVBoxLayout;
    auto *x = new QHBoxLayout;
    auto *view = GeneralPageView::withNavigation(this);
    this->view_ = view;
    x->addWidget(view);
    auto *z = new QFrame;
    z->setLayout(x);
    y->addWidget(z);
    this->setLayout(y);

    this->initLayout(*view);

    this->initExtra();
}

bool TechnorinoPage::filterElements(const QString &query)
{
    if (this->view_)
    {
        return this->view_->filterElements(query) || query.isEmpty();
    }
    else
    {
        return false;
    }
}

void TechnorinoPage::initLayout(GeneralPageView &layout)
{
    auto &s = *getSettings();

    // ---- Twitch GQL Authentication ----
    layout.addTitle("Twitch GQL Authentication");

    // Token status label
    auto *tokenStatusLabel = new QLabel();
    tokenStatusLabel->setWordWrap(true);
    this->updateTokenStatus(tokenStatusLabel);
    layout.addWidget(tokenStatusLabel);

    // Authenticate button
    auto *authBtn = new QPushButton("Authenticate with Twitch");
    authBtn->setStyleSheet(
        "QPushButton { background-color: #9147ff; color: white; "
        "font-weight: bold; padding: 8px 16px; border-radius: 4px; } "
        "QPushButton:hover { background-color: #772ce8; } "
        "QPushButton:disabled { background-color: #555; }");

    // Status label for auth flow
    auto *authStatusLabel = new QLabel();
    authStatusLabel->setWordWrap(true);
    authStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    authStatusLabel->setStyleSheet("color: #aaa; padding: 4px;");
    authStatusLabel->setHidden(true);

    QObject::connect(authBtn, &QPushButton::clicked,
                     [this, authBtn, authStatusLabel, tokenStatusLabel]() {
                         this->startDeviceCodeFlow(authBtn, authStatusLabel,
                                                   tokenStatusLabel);
                     });
    layout.addWidget(authBtn);
    layout.addWidget(authStatusLabel);

    // Clear token button
    auto *clearBtn = new QPushButton("Clear Token");
    clearBtn->setStyleSheet(
        "QPushButton { background-color: #333; color: #ff6b6b; "
        "padding: 6px 12px; border-radius: 4px; border: 1px solid #555; } "
        "QPushButton:hover { background-color: #442222; }");
    QObject::connect(clearBtn, &QPushButton::clicked,
                     [tokenStatusLabel]() {
                         getSettings()->gqlAuthToken = "";
                         tokenStatusLabel->setText(
                             "<span style='color: #ff6b6b;'>"
                             "⚠ No token configured. Features like pinning "
                             "messages and channel points require "
                             "authentication.</span>");
                     });
    layout.addWidget(clearBtn);

    // ---- Banerino Section ----
    layout.addTitle("Banerino");

    layout.addTitle("Chat");
    SettingWidget::checkbox("Show message action menu on hover",
                            s.enableMessageActionMenu)
        ->setTooltip("Show an inline action menu (copy, reply, pin, delete) "
                     "when hovering over messages")
        ->addTo(layout);

    // ---- Technorino Section ----
    layout.addTitle("Technorino");

    layout.addTitle("Technorino Chat");
    SettingWidget::checkbox(
        "Show placeholder in text input box (requires restart)",
        s.showTextInputPlaceholder)
        ->setTooltip("Show placeholder in text input box (requires restart)")
        ->addTo(layout);
    SettingWidget::checkbox("Convert #text to channel links", s.channelLinks)
        ->setTooltip("Convert #text to channel links")
        ->addTo(layout);

    layout.addTitle("Miscellaneous");
    SettingWidget::checkbox("Fake messages as webchat", s.fakeWebChat)
        ->setTooltip("Fake messages as webchat")
        ->addTo(layout);
    SettingWidget::checkbox("Use bot limits for messages",
                            s.useBotLimitsMessage)
        ->setTooltip("Use bot limits for messages")
        ->addTo(layout);
    SettingWidget::checkbox("Use bot limits for JOINs", s.useBotLimitsJoin)
        ->setTooltip("Use bot limits for JOINs")
        ->addTo(layout);
    SettingWidget::checkbox(
        "Enable. Required for abnormal nonce and webchat detection to work!",
        s.nonceFuckeryEnabled)
        ->setTooltip("Enable. Required for abnormal nonce and webchat "
                     "detection to work!")
        ->addTo(layout);
    SettingWidget::checkbox("Abnormal nonce detection",
                            s.abnormalNonceDetection)
        ->setTooltip("Abnormal nonce detection")
        ->addTo(layout);
    SettingWidget::checkbox("\"7TV User\" usercard button", s.stvUsercardButton)
        ->setTooltip("Add \"7TV User\" button to usercard directly")
        ->addTo(layout);

    layout.addTitle("Client detection");
    SettingWidget::checkbox("Client detection highlights. ",
                            s.normalNonceDetection)
        ->setTooltip("Highlights messages sent from specified clients "
                     "using the specified color below.")
        ->addTo(layout);
    SettingWidget::colorButton("Webchat color", getSettings()->webchatColor)
        ->addTo(layout);
    SettingWidget::colorButton("Android color", getSettings()->androidColor)
        ->addTo(layout);
    SettingWidget::colorButton("iOS color", getSettings()->iosColor)
        ->addTo(layout);

    SettingWidget::checkbox("Watching tab live sound", s.watchingTabLiveSound)
        ->setTooltip("Watching tab live sound")
        ->addTo(layout);
    SettingWidget::checkbox("Auto detach watching tab (~10s timeout)",
                            s.autoDetachLiveTab)
        ->setTooltip("Auto detach watching tab (~10s timeout)")
        ->addTo(layout);
    SettingWidget::checkbox("Markdown parsing (Experimental)",
                            s.markdownParsing)
        ->setTooltip("Markdown parsing (Experimental)")
        ->addTo(layout);
    layout.addStretch();

    // invisible element for width
    auto *inv = new BaseWidget(this);
    layout.addWidget(inv);
}

void TechnorinoPage::updateTokenStatus(QLabel *label)
{
    QString token = getSettings()->gqlAuthToken;
    if (token.isEmpty())
    {
        label->setText(
            "<span style='color: #ff6b6b;'>"
            "⚠ No token configured. Features like pinning messages "
            "and channel points require authentication.</span>");
    }
    else
    {
        QString masked = token.left(4) + "..." + token.right(4);
        label->setText(
            "<span style='color: #00e6cb;'>✓ Token configured: " + masked +
            "</span>");
    }
}

void TechnorinoPage::startDeviceCodeFlow(QPushButton *authBtn,
                                          QLabel *statusLabel,
                                          QLabel *tokenStatusLabel)
{
    static const QString DEVICE_CLIENT_ID =
        u"ue6666qo983tsx6so1t0vnawi233wa"_s;

    authBtn->setEnabled(false);
    authBtn->setText("Authenticating...");
    statusLabel->setHidden(false);
    statusLabel->setText("Starting device login...");

    QUrlQuery postData;
    postData.addQueryItem("client_id", DEVICE_CLIENT_ID);
    postData.addQueryItem(
        "scopes",
        "chat:read chat:edit channel:moderate "
        "moderator:manage:chat_messages");

    NetworkRequest("https://id.twitch.tv/oauth2/device",
                   NetworkRequestType::Post)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .payload(postData.toString(QUrl::FullyEncoded).toUtf8())
        .onSuccess([this, authBtn, statusLabel,
                    tokenStatusLabel](const NetworkResult &result) {
            auto json = result.parseJson();
            auto userCode = json["user_code"].toString();
            auto deviceCode = json["device_code"].toString();
            auto interval = static_cast<int>(json["interval"].toDouble(5));

            // Copy code to clipboard
            QGuiApplication::clipboard()->setText(userCode);

            statusLabel->setText(
                "<span style='color: #ffcc00; font-size: 13px;'>"
                "1. Go to <a href='https://www.twitch.tv/activate' "
                "style='color: #9147ff;'>"
                "twitch.tv/activate</a><br>"
                "2. Enter code: <b style='font-size: 15px; color: "
                "#fff;'>" +
                userCode +
                "</b> (copied to clipboard)<br>"
                "3. Waiting for authorization...</span>");
            statusLabel->setOpenExternalLinks(true);

            this->pollDeviceCode(deviceCode, DEVICE_CLIENT_ID,
                                 interval, 0, authBtn, statusLabel,
                                 tokenStatusLabel);
        })
        .onError([authBtn, statusLabel](const NetworkResult &result) {
            statusLabel->setText(
                "<span style='color: #ff6b6b;'>Failed to start "
                "device login: " +
                result.formatError() + "</span>");
            authBtn->setEnabled(true);
            authBtn->setText("Authenticate with Twitch");
        })
        .execute();
}

void TechnorinoPage::pollDeviceCode(const QString &deviceCode,
                                     const QString &clientId, int interval,
                                     int attempt, QPushButton *authBtn,
                                     QLabel *statusLabel,
                                     QLabel *tokenStatusLabel)
{
    if (attempt > 60)
    {
        statusLabel->setText(
            "<span style='color: #ff6b6b;'>Device login timed out. "
            "Try again.</span>");
        authBtn->setEnabled(true);
        authBtn->setText("Authenticate with Twitch");
        return;
    }

    QTimer::singleShot(
        interval * 1000,
        [this, deviceCode, clientId, interval, attempt, authBtn, statusLabel,
         tokenStatusLabel]() {
            QUrlQuery postData;
            postData.addQueryItem("client_id", clientId);
            postData.addQueryItem("device_code", deviceCode);
            postData.addQueryItem(
                "grant_type",
                "urn:ietf:params:oauth:grant-type:device_code");

            NetworkRequest("https://id.twitch.tv/oauth2/token",
                           NetworkRequestType::Post)
                .header("Content-Type", "application/x-www-form-urlencoded")
                .payload(postData.toString(QUrl::FullyEncoded).toUtf8())
                .onSuccess([authBtn, statusLabel,
                            tokenStatusLabel](const NetworkResult &result) {
                    auto json = result.parseJson();
                    auto accessToken = json["access_token"].toString();
                    if (!accessToken.isEmpty())
                    {
                        getSettings()->gqlAuthToken = accessToken;

                        statusLabel->setText(
                            "<span style='color: #00e6cb;'>✓ "
                            "Authentication successful! Token saved.</span>");
                        authBtn->setEnabled(true);
                        authBtn->setText("Authenticate with Twitch");

                        QString masked = accessToken.left(4) + "..." +
                                         accessToken.right(4);
                        tokenStatusLabel->setText(
                            "<span style='color: #00e6cb;'>✓ "
                            "Token configured: " +
                            masked + "</span>");
                    }
                })
                .onError(
                    [this, deviceCode, clientId, interval, attempt, authBtn,
                     statusLabel,
                     tokenStatusLabel](const NetworkResult &result) {
                        auto json = result.parseJson();
                        auto errorMsg = json["message"].toString();
                        if (errorMsg.contains("authorization_pending") ||
                            errorMsg.contains("slow_down"))
                        {
                            this->pollDeviceCode(
                                deviceCode, clientId, interval, attempt + 1,
                                authBtn, statusLabel, tokenStatusLabel);
                        }
                        else
                        {
                            statusLabel->setText(
                                "<span style='color: #ff6b6b;'>"
                                "Login failed: " +
                                errorMsg + "</span>");
                            authBtn->setEnabled(true);
                            authBtn->setText("Authenticate with Twitch");
                        }
                    })
                .execute();
        });
}

void TechnorinoPage::initExtra()
{
    /// update cache path
    if (this->cachePath_)
    {
        getSettings()->cachePath.connect(
            [cachePath = this->cachePath_](const auto &, auto) mutable {
                QString newPath = getApp()->getPaths().cacheDirectory();

                QString pathShortened = "Current location: <a href=\"file:///" +
                                        newPath + "\">" +
                                        shortenString(newPath, 50) + "</a>";

                cachePath->setText(pathShortened);
                cachePath->setToolTip(newPath);
            });
    }
}

}  // namespace chatterino
