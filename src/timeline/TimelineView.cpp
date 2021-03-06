/*
 * nheko Copyright (C) 2017  Konstantinos Sideris <siderisk@auth.gr>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/variant.hpp>

#include <QApplication>
#include <QFileInfo>
#include <QTimer>
#include <QtConcurrent>

#include "Cache.h"
#include "ChatPage.h"
#include "Config.h"
#include "Logging.h"
#include "Olm.h"
#include "UserSettingsPage.h"
#include "Utils.h"
#include "ui/FloatingButton.h"
#include "ui/InfoMessage.h"

#include "timeline/TimelineView.h"
#include "timeline/widgets/AudioItem.h"
#include "timeline/widgets/FileItem.h"
#include "timeline/widgets/ImageItem.h"
#include "timeline/widgets/VideoItem.h"

using TimelineEvent = mtx::events::collections::TimelineEvents;

//! Maximum number of widgets to keep in the timeline layout.
constexpr int MAX_RETAINED_WIDGETS = 100;
constexpr int MIN_SCROLLBAR_HANDLE = 60;

//! Retrieve the timestamp of the event represented by the given widget.
QDateTime
getDate(QWidget *widget)
{
        auto item = qobject_cast<TimelineItem *>(widget);
        if (item)
                return item->descriptionMessage().datetime;

        auto infoMsg = qobject_cast<InfoMessage *>(widget);
        if (infoMsg)
                return infoMsg->datetime();

        return QDateTime();
}

TimelineView::TimelineView(const mtx::responses::Timeline &timeline,
                           const QString &room_id,
                           QWidget *parent)
  : QWidget(parent)
  , room_id_{room_id}
{
        init();
        addEvents(timeline);
}

TimelineView::TimelineView(const QString &room_id, QWidget *parent)
  : QWidget(parent)
  , room_id_{room_id}
{
        init();
        getMessages();
}

void
TimelineView::sliderRangeChanged(int min, int max)
{
        Q_UNUSED(min);

        if (!scroll_area_->verticalScrollBar()->isVisible()) {
                scroll_area_->verticalScrollBar()->setValue(max);
                return;
        }

        // If the scrollbar is close to the bottom and a new message
        // is added we move the scrollbar.
        if (max - scroll_area_->verticalScrollBar()->value() < SCROLL_BAR_GAP) {
                scroll_area_->verticalScrollBar()->setValue(max);
                return;
        }

        int currentHeight = scroll_widget_->size().height();
        int diff          = currentHeight - oldHeight_;
        int newPosition   = oldPosition_ + diff;

        // Keep the scroll bar to the bottom if it hasn't been activated yet.
        if (oldPosition_ == 0 && !scroll_area_->verticalScrollBar()->isVisible())
                newPosition = max;

        if (lastMessageDirection_ == TimelineDirection::Top)
                scroll_area_->verticalScrollBar()->setValue(newPosition);
}

void
TimelineView::fetchHistory()
{
        if (!isScrollbarActivated() && !isTimelineFinished) {
                if (!isVisible())
                        return;

                isPaginationInProgress_ = true;
                getMessages();
                paginationTimer_->start(2000);

                return;
        }

        paginationTimer_->stop();
}

void
TimelineView::scrollDown()
{
        int current = scroll_area_->verticalScrollBar()->value();
        int max     = scroll_area_->verticalScrollBar()->maximum();

        // The first time we enter the room move the scroll bar to the bottom.
        if (!isInitialized) {
                scroll_area_->verticalScrollBar()->setValue(max);
                isInitialized = true;
                return;
        }

        // If the gap is small enough move the scroll bar down. e.g when a new
        // message appears.
        if (max - current < SCROLL_BAR_GAP)
                scroll_area_->verticalScrollBar()->setValue(max);
}

void
TimelineView::sliderMoved(int position)
{
        if (!scroll_area_->verticalScrollBar()->isVisible())
                return;

        toggleScrollDownButton();

        // The scrollbar is high enough so we can start retrieving old events.
        if (position < SCROLL_BAR_GAP) {
                if (isTimelineFinished)
                        return;

                // Prevent user from moving up when there is pagination in
                // progress.
                if (isPaginationInProgress_)
                        return;

                isPaginationInProgress_ = true;

                getMessages();
        }
}

bool
TimelineView::isStartOfTimeline(const mtx::responses::Messages &msgs)
{
        return (msgs.chunk.size() == 0 && (msgs.end.empty() || msgs.end == msgs.start));
}

void
TimelineView::addBackwardsEvents(const mtx::responses::Messages &msgs)
{
        // We've reached the start of the timline and there're no more messages.
        if (isStartOfTimeline(msgs)) {
                nhlog::ui()->info("[{}] start of timeline reached, no more messages to fetch",
                                  room_id_.toStdString());
                isTimelineFinished = true;
                return;
        }

        isTimelineFinished = false;

        // Queue incoming messages to be rendered later.
        topMessages_.insert(topMessages_.end(),
                            std::make_move_iterator(msgs.chunk.begin()),
                            std::make_move_iterator(msgs.chunk.end()));

        // The RoomList message preview will be updated only if this
        // is the first batch of messages received through /messages
        // i.e there are no other messages currently present.
        if (!topMessages_.empty() && scroll_layout_->count() == 0)
                notifyForLastEvent(findFirstViewableEvent(topMessages_));

        if (isVisible()) {
                renderTopEvents(topMessages_);

                // Free up space for new messages.
                topMessages_.clear();

                // Send a read receipt for the last event.
                if (isActiveWindow())
                        readLastEvent();
        }

        prev_batch_token_       = QString::fromStdString(msgs.end);
        isPaginationInProgress_ = false;
}

QWidget *
TimelineView::parseMessageEvent(const mtx::events::collections::TimelineEvents &event,
                                TimelineDirection direction)
{
        using namespace mtx::events;

        using AudioEvent  = RoomEvent<msg::Audio>;
        using EmoteEvent  = RoomEvent<msg::Emote>;
        using FileEvent   = RoomEvent<msg::File>;
        using ImageEvent  = RoomEvent<msg::Image>;
        using NoticeEvent = RoomEvent<msg::Notice>;
        using TextEvent   = RoomEvent<msg::Text>;
        using VideoEvent  = RoomEvent<msg::Video>;

        if (boost::get<RedactionEvent<msg::Redaction>>(&event) != nullptr) {
                auto redaction_event = boost::get<RedactionEvent<msg::Redaction>>(event);
                const auto event_id  = QString::fromStdString(redaction_event.redacts);

                QTimer::singleShot(0, this, [event_id, this]() {
                        if (eventIds_.contains(event_id))
                                removeEvent(event_id);
                });

                return nullptr;
        } else if (boost::get<StateEvent<state::Encryption>>(&event) != nullptr) {
                auto msg      = boost::get<StateEvent<state::Encryption>>(event);
                auto event_id = QString::fromStdString(msg.event_id);

                if (eventIds_.contains(event_id))
                        return nullptr;

                auto item = new InfoMessage(tr("Encryption is enabled"), this);
                item->saveDatetime(QDateTime::fromMSecsSinceEpoch(msg.origin_server_ts));
                eventIds_[event_id] = item;

                // Force the next message to have avatar by not providing the current username.
                saveMessageInfo("", msg.origin_server_ts, direction);

                return item;
        } else if (boost::get<RoomEvent<msg::Audio>>(&event) != nullptr) {
                auto audio = boost::get<RoomEvent<msg::Audio>>(event);
                return processMessageEvent<AudioEvent, AudioItem>(audio, direction);
        } else if (boost::get<RoomEvent<msg::Emote>>(&event) != nullptr) {
                auto emote = boost::get<RoomEvent<msg::Emote>>(event);
                return processMessageEvent<EmoteEvent>(emote, direction);
        } else if (boost::get<RoomEvent<msg::File>>(&event) != nullptr) {
                auto file = boost::get<RoomEvent<msg::File>>(event);
                return processMessageEvent<FileEvent, FileItem>(file, direction);
        } else if (boost::get<RoomEvent<msg::Image>>(&event) != nullptr) {
                auto image = boost::get<RoomEvent<msg::Image>>(event);
                return processMessageEvent<ImageEvent, ImageItem>(image, direction);
        } else if (boost::get<RoomEvent<msg::Notice>>(&event) != nullptr) {
                auto notice = boost::get<RoomEvent<msg::Notice>>(event);
                return processMessageEvent<NoticeEvent>(notice, direction);
        } else if (boost::get<RoomEvent<msg::Text>>(&event) != nullptr) {
                auto text = boost::get<RoomEvent<msg::Text>>(event);
                return processMessageEvent<TextEvent>(text, direction);
        } else if (boost::get<RoomEvent<msg::Video>>(&event) != nullptr) {
                auto video = boost::get<RoomEvent<msg::Video>>(event);
                return processMessageEvent<VideoEvent, VideoItem>(video, direction);
        } else if (boost::get<Sticker>(&event) != nullptr) {
                return processMessageEvent<Sticker, StickerItem>(boost::get<Sticker>(event),
                                                                 direction);
        } else if (boost::get<EncryptedEvent<msg::Encrypted>>(&event) != nullptr) {
                auto res = parseEncryptedEvent(boost::get<EncryptedEvent<msg::Encrypted>>(event));
                auto widget = parseMessageEvent(res.event, direction);

                if (widget == nullptr)
                        return nullptr;

                auto item = qobject_cast<TimelineItem *>(widget);

                if (item && res.isDecrypted)
                        item->markReceived(true);
                else if (item && !res.isDecrypted)
                        item->addKeyRequestAction();

                return widget;
        }

        return nullptr;
}

DecryptionResult
TimelineView::parseEncryptedEvent(const mtx::events::EncryptedEvent<mtx::events::msg::Encrypted> &e)
{
        MegolmSessionIndex index;
        index.room_id    = room_id_.toStdString();
        index.session_id = e.content.session_id;
        index.sender_key = e.content.sender_key;

        mtx::events::RoomEvent<mtx::events::msg::Notice> dummy;
        dummy.origin_server_ts = e.origin_server_ts;
        dummy.event_id         = e.event_id;
        dummy.sender           = e.sender;
        dummy.content.body     = "-- Encrypted Event (No keys found for decryption) --";

        try {
                if (!cache::client()->inboundMegolmSessionExists(index)) {
                        nhlog::crypto()->info("Could not find inbound megolm session ({}, {}, {})",
                                              index.room_id,
                                              index.session_id,
                                              e.sender);
                        // TODO: request megolm session_id & session_key from the sender.
                        return {dummy, false};
                }
        } catch (const lmdb::error &e) {
                nhlog::db()->critical("failed to check megolm session's existence: {}", e.what());
                dummy.content.body = "-- Decryption Error (failed to communicate with DB) --";
                return {dummy, false};
        }

        std::string msg_str;
        try {
                auto session = cache::client()->getInboundMegolmSession(index);
                auto res     = olm::client()->decrypt_group_message(session, e.content.ciphertext);
                msg_str      = std::string((char *)res.data.data(), res.data.size());
        } catch (const lmdb::error &e) {
                nhlog::db()->critical("failed to retrieve megolm session with index ({}, {}, {})",
                                      index.room_id,
                                      index.session_id,
                                      index.sender_key,
                                      e.what());
                dummy.content.body =
                  "-- Decryption Error (failed to retrieve megolm keys from db) --";
                return {dummy, false};
        } catch (const mtx::crypto::olm_exception &e) {
                nhlog::crypto()->critical("failed to decrypt message with index ({}, {}, {}): {}",
                                          index.room_id,
                                          index.session_id,
                                          index.sender_key,
                                          e.what());
                dummy.content.body = "-- Decryption Error (" + std::string(e.what()) + ") --";
                return {dummy, false};
        }

        // Add missing fields for the event.
        json body                = json::parse(msg_str);
        body["event_id"]         = e.event_id;
        body["sender"]           = e.sender;
        body["origin_server_ts"] = e.origin_server_ts;
        body["unsigned"]         = e.unsigned_data;

        nhlog::crypto()->debug("decrypted event: {}", e.event_id);

        json event_array = json::array();
        event_array.push_back(body);

        std::vector<TimelineEvent> events;
        mtx::responses::utils::parse_timeline_events(event_array, events);

        if (events.size() == 1)
                return {events.at(0), true};

        dummy.content.body = "-- Encrypted Event (Unknown event type) --";
        return {dummy, false};
}

void
TimelineView::displayReadReceipts(std::vector<TimelineEvent> events)
{
        QtConcurrent::run(
          [events = std::move(events), room_id = room_id_, local_user = local_user_, this]() {
                  std::vector<QString> event_ids;

                  for (const auto &e : events) {
                          if (utils::event_sender(e) == local_user)
                                  event_ids.emplace_back(
                                    QString::fromStdString(utils::event_id(e)));
                  }

                  auto readEvents =
                    cache::client()->filterReadEvents(room_id, event_ids, local_user.toStdString());

                  if (!readEvents.empty())
                          emit markReadEvents(readEvents);
          });
}

void
TimelineView::renderBottomEvents(const std::vector<TimelineEvent> &events)
{
        int counter = 0;

        for (const auto &event : events) {
                QWidget *item = parseMessageEvent(event, TimelineDirection::Bottom);

                if (item != nullptr) {
                        addTimelineItem(item, TimelineDirection::Bottom);
                        counter++;

                        // Prevent blocking of the event-loop
                        // by calling processEvents every 10 items we render.
                        if (counter % 4 == 0)
                                QApplication::processEvents();
                }
        }

        lastMessageDirection_ = TimelineDirection::Bottom;

        displayReadReceipts(events);

        QApplication::processEvents();
}

void
TimelineView::renderTopEvents(const std::vector<TimelineEvent> &events)
{
        std::vector<QWidget *> items;

        // Reset the sender of the first message in the timeline
        // cause we're about to insert a new one.
        firstSender_.clear();
        firstMsgTimestamp_ = QDateTime();

        // Parse in reverse order to determine where we should not show sender's name.
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
                auto item = parseMessageEvent(*it, TimelineDirection::Top);

                if (item != nullptr)
                        items.push_back(item);
        }

        // Reverse again to render them.
        std::reverse(items.begin(), items.end());

        oldPosition_ = scroll_area_->verticalScrollBar()->value();
        oldHeight_   = scroll_widget_->size().height();

        for (const auto &item : items)
                addTimelineItem(item, TimelineDirection::Top);

        lastMessageDirection_ = TimelineDirection::Top;

        QApplication::processEvents();

        displayReadReceipts(events);

        // If this batch is the first being rendered (i.e the first and the last
        // events originate from this batch), set the last sender.
        if (lastSender_.isEmpty() && !items.empty()) {
                for (const auto &w : items) {
                        auto timelineItem = qobject_cast<TimelineItem *>(w);
                        if (timelineItem) {
                                saveLastMessageInfo(timelineItem->descriptionMessage().userid,
                                                    timelineItem->descriptionMessage().datetime);
                                break;
                        }
                }
        }
}

void
TimelineView::addEvents(const mtx::responses::Timeline &timeline)
{
        if (isInitialSync) {
                prev_batch_token_ = QString::fromStdString(timeline.prev_batch);
                isInitialSync     = false;
        }

        bottomMessages_.insert(bottomMessages_.end(),
                               std::make_move_iterator(timeline.events.begin()),
                               std::make_move_iterator(timeline.events.end()));

        if (!bottomMessages_.empty())
                notifyForLastEvent(findLastViewableEvent(bottomMessages_));

        // If the current timeline is open and there are messages to be rendered.
        if (isVisible() && !bottomMessages_.empty()) {
                renderBottomEvents(bottomMessages_);

                // Free up space for new messages.
                bottomMessages_.clear();

                // Send a read receipt for the last event.
                if (isActiveWindow())
                        readLastEvent();
        }
}

void
TimelineView::init()
{
        local_user_ = utils::localUser();

        QIcon icon;
        icon.addFile(":/icons/icons/ui/angle-arrow-down.png");
        scrollDownBtn_ = new FloatingButton(icon, this);
        scrollDownBtn_->hide();

        connect(scrollDownBtn_, &QPushButton::clicked, this, [this]() {
                const int max = scroll_area_->verticalScrollBar()->maximum();
                scroll_area_->verticalScrollBar()->setValue(max);
        });
        top_layout_ = new QVBoxLayout(this);
        top_layout_->setSpacing(0);
        top_layout_->setMargin(0);

        scroll_area_ = new QScrollArea(this);
        scroll_area_->setWidgetResizable(true);
        scroll_area_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        scroll_widget_ = new QWidget(this);
        scroll_widget_->setObjectName("scroll_widget");

        // Height of the typing display.
        QFont f;
        f.setPointSizeF(f.pointSizeF() * 0.9);
        const int bottomMargin = QFontMetrics(f).height() + 6;

        scroll_layout_ = new QVBoxLayout(scroll_widget_);
        scroll_layout_->setContentsMargins(4, 0, 15, bottomMargin);
        scroll_layout_->setSpacing(0);
        scroll_layout_->setObjectName("timelinescrollarea");

        scroll_area_->setWidget(scroll_widget_);
        scroll_area_->setAlignment(Qt::AlignBottom);

        top_layout_->addWidget(scroll_area_);

        setLayout(top_layout_);

        paginationTimer_ = new QTimer(this);
        connect(paginationTimer_, &QTimer::timeout, this, &TimelineView::fetchHistory);

        connect(this, &TimelineView::messagesRetrieved, this, &TimelineView::addBackwardsEvents);

        connect(this, &TimelineView::messageFailed, this, &TimelineView::handleFailedMessage);
        connect(this, &TimelineView::messageSent, this, &TimelineView::updatePendingMessage);

        connect(
          this, &TimelineView::markReadEvents, this, [this](const std::vector<QString> &event_ids) {
                  for (const auto &event : event_ids) {
                          if (eventIds_.contains(event)) {
                                  auto widget = eventIds_[event];
                                  if (!widget)
                                          return;

                                  auto item = qobject_cast<TimelineItem *>(widget);
                                  if (!item)
                                          return;

                                  item->markRead();
                          }
                  }
          });

        connect(scroll_area_->verticalScrollBar(),
                SIGNAL(valueChanged(int)),
                this,
                SLOT(sliderMoved(int)));
        connect(scroll_area_->verticalScrollBar(),
                SIGNAL(rangeChanged(int, int)),
                this,
                SLOT(sliderRangeChanged(int, int)));
}

void
TimelineView::getMessages()
{
        mtx::http::MessagesOpts opts;
        opts.room_id = room_id_.toStdString();
        opts.from    = prev_batch_token_.toStdString();

        http::client()->messages(
          opts, [this, opts](const mtx::responses::Messages &res, mtx::http::RequestErr err) {
                  if (err) {
                          nhlog::net()->error("failed to call /messages ({}): {} - {}",
                                              opts.room_id,
                                              mtx::errors::to_string(err->matrix_error.errcode),
                                              err->matrix_error.error);
                          return;
                  }

                  emit messagesRetrieved(std::move(res));
          });
}

void
TimelineView::updateLastSender(const QString &user_id, TimelineDirection direction)
{
        if (direction == TimelineDirection::Bottom)
                lastSender_ = user_id;
        else
                firstSender_ = user_id;
}

bool
TimelineView::isSenderRendered(const QString &user_id,
                               uint64_t origin_server_ts,
                               TimelineDirection direction)
{
        if (direction == TimelineDirection::Bottom) {
                return (lastSender_ != user_id) ||
                       isDateDifference(lastMsgTimestamp_,
                                        QDateTime::fromMSecsSinceEpoch(origin_server_ts));
        } else {
                return (firstSender_ != user_id) ||
                       isDateDifference(firstMsgTimestamp_,
                                        QDateTime::fromMSecsSinceEpoch(origin_server_ts));
        }
}

void
TimelineView::addTimelineItem(QWidget *item, TimelineDirection direction)
{
        const auto newDate = getDate(item);

        if (direction == TimelineDirection::Bottom) {
                QWidget *lastItem    = nullptr;
                int lastItemPosition = 0;

                if (scroll_layout_->count() > 0) {
                        lastItemPosition = scroll_layout_->count() - 1;
                        lastItem         = scroll_layout_->itemAt(lastItemPosition)->widget();
                }

                if (lastItem) {
                        const auto oldDate = getDate(lastItem);

                        if (oldDate.daysTo(newDate) != 0) {
                                auto separator = new DateSeparator(newDate, this);

                                if (separator)
                                        pushTimelineItem(separator, direction);
                        }
                }

                pushTimelineItem(item, direction);
        } else {
                if (scroll_layout_->count() > 0) {
                        const auto firstItem = scroll_layout_->itemAt(0)->widget();

                        if (firstItem) {
                                const auto oldDate = getDate(firstItem);

                                if (newDate.daysTo(oldDate) != 0) {
                                        auto separator = new DateSeparator(oldDate);

                                        if (separator)
                                                pushTimelineItem(separator, direction);
                                }
                        }
                }

                pushTimelineItem(item, direction);
        }
}

void
TimelineView::updatePendingMessage(const std::string &txn_id, const QString &event_id)
{
        nhlog::ui()->debug("[{}] message was received by the server", txn_id);
        if (!pending_msgs_.isEmpty() &&
            pending_msgs_.head().txn_id == txn_id) { // We haven't received it yet
                auto msg     = pending_msgs_.dequeue();
                msg.event_id = event_id;

                if (msg.widget) {
                        msg.widget->setEventId(event_id);
                        eventIds_[event_id] = msg.widget;

                        // If the response comes after we have received the event from sync
                        // we've already marked the widget as received.
                        if (!msg.widget->isReceived()) {
                                msg.widget->markReceived(msg.is_encrypted);
                                cache::client()->addPendingReceipt(room_id_, event_id);
                                pending_sent_msgs_.append(msg);
                        }
                } else {
                        nhlog::ui()->warn("[{}] received message response for invalid widget",
                                          txn_id);
                }
        }

        sendNextPendingMessage();
}

void
TimelineView::addUserMessage(mtx::events::MessageType ty, const QString &body)
{
        auto with_sender = (lastSender_ != local_user_) || isDateDifference(lastMsgTimestamp_);

        TimelineItem *view_item =
          new TimelineItem(ty, local_user_, body, with_sender, room_id_, scroll_widget_);

        PendingMessage message;
        message.ty     = ty;
        message.txn_id = http::client()->generate_txn_id();
        message.body   = body;
        message.widget = view_item;

        try {
                message.is_encrypted = cache::client()->isRoomEncrypted(room_id_.toStdString());
        } catch (const lmdb::error &e) {
                nhlog::db()->critical("failed to check encryption status of room {}", e.what());
                view_item->deleteLater();

                // TODO: Send a notification to the user.

                return;
        }

        addTimelineItem(view_item);

        lastMessageDirection_ = TimelineDirection::Bottom;

        saveLastMessageInfo(local_user_, QDateTime::currentDateTime());
        handleNewUserMessage(message);
}

void
TimelineView::handleNewUserMessage(PendingMessage msg)
{
        pending_msgs_.enqueue(msg);
        if (pending_msgs_.size() == 1 && pending_sent_msgs_.isEmpty())
                sendNextPendingMessage();
}

void
TimelineView::sendNextPendingMessage()
{
        if (pending_msgs_.size() == 0)
                return;

        using namespace mtx::events;

        PendingMessage &m = pending_msgs_.head();

        nhlog::ui()->debug("[{}] sending next queued message", m.txn_id);

        if (m.widget)
                m.widget->markSent();

        if (m.is_encrypted) {
                nhlog::ui()->debug("[{}] sending encrypted event", m.txn_id);
                prepareEncryptedMessage(std::move(m));
                return;
        }

        switch (m.ty) {
        case mtx::events::MessageType::Audio: {
                http::client()->send_room_message<msg::Audio, EventType::RoomMessage>(
                  room_id_.toStdString(),
                  m.txn_id,
                  toRoomMessage<msg::Audio>(m),
                  std::bind(&TimelineView::sendRoomMessageHandler,
                            this,
                            m.txn_id,
                            std::placeholders::_1,
                            std::placeholders::_2));

                break;
        }
        case mtx::events::MessageType::Image: {
                http::client()->send_room_message<msg::Image, EventType::RoomMessage>(
                  room_id_.toStdString(),
                  m.txn_id,
                  toRoomMessage<msg::Image>(m),
                  std::bind(&TimelineView::sendRoomMessageHandler,
                            this,
                            m.txn_id,
                            std::placeholders::_1,
                            std::placeholders::_2));

                break;
        }
        case mtx::events::MessageType::Video: {
                http::client()->send_room_message<msg::Video, EventType::RoomMessage>(
                  room_id_.toStdString(),
                  m.txn_id,
                  toRoomMessage<msg::Video>(m),
                  std::bind(&TimelineView::sendRoomMessageHandler,
                            this,
                            m.txn_id,
                            std::placeholders::_1,
                            std::placeholders::_2));

                break;
        }
        case mtx::events::MessageType::File: {
                http::client()->send_room_message<msg::File, EventType::RoomMessage>(
                  room_id_.toStdString(),
                  m.txn_id,
                  toRoomMessage<msg::File>(m),
                  std::bind(&TimelineView::sendRoomMessageHandler,
                            this,
                            m.txn_id,
                            std::placeholders::_1,
                            std::placeholders::_2));

                break;
        }
        case mtx::events::MessageType::Text: {
                http::client()->send_room_message<msg::Text, EventType::RoomMessage>(
                  room_id_.toStdString(),
                  m.txn_id,
                  toRoomMessage<msg::Text>(m),
                  std::bind(&TimelineView::sendRoomMessageHandler,
                            this,
                            m.txn_id,
                            std::placeholders::_1,
                            std::placeholders::_2));

                break;
        }
        case mtx::events::MessageType::Emote: {
                http::client()->send_room_message<msg::Emote, EventType::RoomMessage>(
                  room_id_.toStdString(),
                  m.txn_id,
                  toRoomMessage<msg::Emote>(m),
                  std::bind(&TimelineView::sendRoomMessageHandler,
                            this,
                            m.txn_id,
                            std::placeholders::_1,
                            std::placeholders::_2));
                break;
        }
        default:
                nhlog::ui()->warn("cannot send unknown message type: {}", m.body.toStdString());
                break;
        }
}

void
TimelineView::notifyForLastEvent()
{
        if (scroll_layout_->count() == 0) {
                nhlog::ui()->error("notifyForLastEvent called with empty timeline");
                return;
        }

        auto lastItem = scroll_layout_->itemAt(scroll_layout_->count() - 1);

        if (!lastItem)
                return;

        auto *lastTimelineItem = qobject_cast<TimelineItem *>(lastItem->widget());

        if (lastTimelineItem)
                emit updateLastTimelineMessage(room_id_, lastTimelineItem->descriptionMessage());
        else
                nhlog::ui()->warn("cast to TimelineItem failed: {}", room_id_.toStdString());
}

void
TimelineView::notifyForLastEvent(const TimelineEvent &event)
{
        auto descInfo = utils::getMessageDescription(event, local_user_, room_id_);

        if (!descInfo.timestamp.isEmpty())
                emit updateLastTimelineMessage(room_id_, descInfo);
}

bool
TimelineView::isPendingMessage(const std::string &txn_id,
                               const QString &sender,
                               const QString &local_userid)
{
        if (sender != local_userid)
                return false;

        auto match_txnid = [txn_id](const auto &msg) -> bool { return msg.txn_id == txn_id; };

        return std::any_of(pending_msgs_.cbegin(), pending_msgs_.cend(), match_txnid) ||
               std::any_of(pending_sent_msgs_.cbegin(), pending_sent_msgs_.cend(), match_txnid);
}

void
TimelineView::removePendingMessage(const std::string &txn_id)
{
        if (txn_id.empty())
                return;

        for (auto it = pending_sent_msgs_.begin(); it != pending_sent_msgs_.end(); ++it) {
                if (it->txn_id == txn_id) {
                        int index = std::distance(pending_sent_msgs_.begin(), it);
                        pending_sent_msgs_.removeAt(index);

                        if (pending_sent_msgs_.isEmpty())
                                sendNextPendingMessage();

                        nhlog::ui()->debug("[{}] removed message with sync", txn_id);
                }
        }
        for (auto it = pending_msgs_.begin(); it != pending_msgs_.end(); ++it) {
                if (it->txn_id == txn_id) {
                        if (it->widget) {
                                it->widget->markReceived(it->is_encrypted);

                                // TODO: update when a solution for encrypted messages is available.
                                if (!it->is_encrypted)
                                        cache::client()->addPendingReceipt(room_id_, it->event_id);
                        }

                        nhlog::ui()->debug("[{}] received sync before message response", txn_id);
                        return;
                }
        }
}

void
TimelineView::handleFailedMessage(const std::string &txn_id)
{
        Q_UNUSED(txn_id);
        // Note: We do this even if the message has already been echoed.
        QTimer::singleShot(2000, this, SLOT(sendNextPendingMessage()));
}

void
TimelineView::paintEvent(QPaintEvent *)
{
        QStyleOption opt;
        opt.init(this);
        QPainter p(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void
TimelineView::readLastEvent() const
{
        if (!ChatPage::instance()->userSettings()->isReadReceiptsEnabled())
                return;

        const auto eventId = getLastEventId();

        if (!eventId.isEmpty())
                http::client()->read_event(room_id_.toStdString(),
                                           eventId.toStdString(),
                                           [this, eventId](mtx::http::RequestErr err) {
                                                   if (err) {
                                                           nhlog::net()->warn(
                                                             "failed to read event ({}, {})",
                                                             room_id_.toStdString(),
                                                             eventId.toStdString());
                                                   }
                                           });
}

QString
TimelineView::getLastEventId() const
{
        auto index = scroll_layout_->count();

        // Search backwards for the first event that has a valid event id.
        while (index > 0) {
                --index;

                auto lastItem          = scroll_layout_->itemAt(index);
                auto *lastTimelineItem = qobject_cast<TimelineItem *>(lastItem->widget());

                if (lastTimelineItem && !lastTimelineItem->eventId().isEmpty())
                        return lastTimelineItem->eventId();
        }

        return QString("");
}

void
TimelineView::showEvent(QShowEvent *event)
{
        if (!topMessages_.empty()) {
                renderTopEvents(topMessages_);
                topMessages_.clear();
        }

        if (!bottomMessages_.empty()) {
                renderBottomEvents(bottomMessages_);
                bottomMessages_.clear();
                scrollDown();
        }

        toggleScrollDownButton();

        readLastEvent();

        QWidget::showEvent(event);
}

void
TimelineView::hideEvent(QHideEvent *event)
{
        const auto handleHeight = scroll_area_->verticalScrollBar()->sizeHint().height();
        const auto widgetsNum   = scroll_layout_->count();

        // Remove widgets from the timeline to reduce the memory footprint.
        if (handleHeight < MIN_SCROLLBAR_HANDLE && widgetsNum > MAX_RETAINED_WIDGETS)
                clearTimeline();

        QWidget::hideEvent(event);
}

bool
TimelineView::event(QEvent *event)
{
        if (event->type() == QEvent::WindowActivate)
                readLastEvent();

        return QWidget::event(event);
}

void
TimelineView::clearTimeline()
{
        // Delete all widgets.
        QLayoutItem *item;
        while ((item = scroll_layout_->takeAt(0)) != nullptr) {
                delete item->widget();
                delete item;
        }

        // The next call to /messages will be without a prev token.
        prev_batch_token_.clear();
        eventIds_.clear();

        // Clear queues with pending messages to be rendered.
        bottomMessages_.clear();
        topMessages_.clear();

        firstSender_.clear();
        lastSender_.clear();
}

void
TimelineView::toggleScrollDownButton()
{
        const int maxScroll     = scroll_area_->verticalScrollBar()->maximum();
        const int currentScroll = scroll_area_->verticalScrollBar()->value();

        if (maxScroll - currentScroll > SCROLL_BAR_GAP) {
                scrollDownBtn_->show();
                scrollDownBtn_->raise();
        } else {
                scrollDownBtn_->hide();
        }
}

void
TimelineView::removeEvent(const QString &event_id)
{
        if (!eventIds_.contains(event_id)) {
                nhlog::ui()->warn("cannot remove widget with unknown event_id: {}",
                                  event_id.toStdString());
                return;
        }

        auto removedItem = eventIds_[event_id];

        // Find the next and the previous widgets in the timeline
        auto prevWidget = relativeWidget(removedItem, -1);
        auto nextWidget = relativeWidget(removedItem, 1);

        // See if they are timeline items
        auto prevItem = qobject_cast<TimelineItem *>(prevWidget);
        auto nextItem = qobject_cast<TimelineItem *>(nextWidget);

        // ... or a date separator
        auto prevLabel = qobject_cast<DateSeparator *>(prevWidget);

        // If it's a TimelineItem add an avatar.
        if (prevItem) {
                prevItem->addAvatar();
        }

        if (nextItem) {
                nextItem->addAvatar();
        } else if (prevLabel) {
                // If there's no chat message after this, and we have a label before us, delete the
                // label.
                prevLabel->deleteLater();
        }

        // If we deleted the last item in the timeline...
        if (!nextItem && prevItem)
                saveLastMessageInfo(prevItem->descriptionMessage().userid,
                                    prevItem->descriptionMessage().datetime);

        // If we deleted the first item in the timeline...
        if (!prevItem && nextItem)
                saveFirstMessageInfo(nextItem->descriptionMessage().userid,
                                     nextItem->descriptionMessage().datetime);

        // If we deleted the only item in the timeline...
        if (!prevItem && !nextItem) {
                firstSender_.clear();
                firstMsgTimestamp_ = QDateTime();
                lastSender_.clear();
                lastMsgTimestamp_ = QDateTime();
        }

        // Finally remove the event.
        removedItem->deleteLater();
        eventIds_.remove(event_id);

        // Update the room list with a view of the last message after
        // all events have been processed.
        QTimer::singleShot(0, this, [this]() { notifyForLastEvent(); });
}

QWidget *
TimelineView::relativeWidget(QWidget *item, int dt) const
{
        int pos = scroll_layout_->indexOf(item);

        if (pos == -1)
                return nullptr;

        pos = pos + dt;

        bool isOutOfBounds = (pos < 0 || pos > scroll_layout_->count() - 1);

        return isOutOfBounds ? nullptr : scroll_layout_->itemAt(pos)->widget();
}

TimelineEvent
TimelineView::findFirstViewableEvent(const std::vector<TimelineEvent> &events)
{
        auto it = std::find_if(events.begin(), events.end(), [](const auto &event) {
                return mtx::events::EventType::RoomMessage == utils::event_type(event);
        });

        return (it == std::end(events)) ? events.front() : *it;
}

TimelineEvent
TimelineView::findLastViewableEvent(const std::vector<TimelineEvent> &events)
{
        auto it = std::find_if(events.rbegin(), events.rend(), [](const auto &event) {
                return (mtx::events::EventType::RoomMessage == utils::event_type(event)) ||
                       (mtx::events::EventType::RoomEncrypted == utils::event_type(event));
        });

        return (it == std::rend(events)) ? events.back() : *it;
}

void
TimelineView::saveMessageInfo(const QString &sender,
                              uint64_t origin_server_ts,
                              TimelineDirection direction)
{
        updateLastSender(sender, direction);

        if (direction == TimelineDirection::Bottom)
                lastMsgTimestamp_ = QDateTime::fromMSecsSinceEpoch(origin_server_ts);
        else
                firstMsgTimestamp_ = QDateTime::fromMSecsSinceEpoch(origin_server_ts);
}

bool
TimelineView::isDateDifference(const QDateTime &first, const QDateTime &second) const
{
        // Check if the dates are in a different day.
        if (std::abs(first.daysTo(second)) != 0)
                return true;

        const uint64_t diffInSeconds   = std::abs(first.msecsTo(second)) / 1000;
        constexpr uint64_t fifteenMins = 15 * 60;

        return diffInSeconds > fifteenMins;
}

void
TimelineView::sendRoomMessageHandler(const std::string &txn_id,
                                     const mtx::responses::EventId &res,
                                     mtx::http::RequestErr err)
{
        if (err) {
                const int status_code = static_cast<int>(err->status_code);
                nhlog::net()->warn("[{}] failed to send message: {} {}",
                                   txn_id,
                                   err->matrix_error.error,
                                   status_code);
                emit messageFailed(txn_id);
                return;
        }

        emit messageSent(txn_id, QString::fromStdString(res.event_id.to_string()));
}

template<>
mtx::events::msg::Audio
toRoomMessage<mtx::events::msg::Audio>(const PendingMessage &m)
{
        mtx::events::msg::Audio audio;
        audio.info.mimetype = m.mime.toStdString();
        audio.info.size     = m.media_size;
        audio.body          = m.filename.toStdString();
        audio.url           = m.body.toStdString();
        return audio;
}

template<>
mtx::events::msg::Image
toRoomMessage<mtx::events::msg::Image>(const PendingMessage &m)
{
        mtx::events::msg::Image image;
        image.info.mimetype = m.mime.toStdString();
        image.info.size     = m.media_size;
        image.body          = m.filename.toStdString();
        image.url           = m.body.toStdString();
        image.info.h        = m.dimensions.height();
        image.info.w        = m.dimensions.width();
        return image;
}

template<>
mtx::events::msg::Video
toRoomMessage<mtx::events::msg::Video>(const PendingMessage &m)
{
        mtx::events::msg::Video video;
        video.info.mimetype = m.mime.toStdString();
        video.info.size     = m.media_size;
        video.body          = m.filename.toStdString();
        video.url           = m.body.toStdString();
        return video;
}

template<>
mtx::events::msg::Emote
toRoomMessage<mtx::events::msg::Emote>(const PendingMessage &m)
{
        auto html = utils::markdownToHtml(m.body);

        mtx::events::msg::Emote emote;
        emote.body = m.body.trimmed().toStdString();

        if (html != m.body.trimmed().toHtmlEscaped())
                emote.formatted_body = html.toStdString();

        return emote;
}

template<>
mtx::events::msg::File
toRoomMessage<mtx::events::msg::File>(const PendingMessage &m)
{
        mtx::events::msg::File file;
        file.info.mimetype = m.mime.toStdString();
        file.info.size     = m.media_size;
        file.body          = m.filename.toStdString();
        file.url           = m.body.toStdString();
        return file;
}

template<>
mtx::events::msg::Text
toRoomMessage<mtx::events::msg::Text>(const PendingMessage &m)
{
        auto html = utils::markdownToHtml(m.body);

        mtx::events::msg::Text text;
        text.body = m.body.trimmed().toStdString();

        if (html != m.body.trimmed().toHtmlEscaped())
                text.formatted_body = html.toStdString();

        return text;
}

void
TimelineView::prepareEncryptedMessage(const PendingMessage &msg)
{
        const auto room_id = room_id_.toStdString();

        using namespace mtx::events;
        using namespace mtx::identifiers;

        json content;

        // Serialize the message to the plaintext that will be encrypted.
        switch (msg.ty) {
        case MessageType::Audio: {
                content = json(toRoomMessage<msg::Audio>(msg));
                break;
        }
        case MessageType::Emote: {
                content = json(toRoomMessage<msg::Emote>(msg));
                break;
        }
        case MessageType::File: {
                content = json(toRoomMessage<msg::File>(msg));
                break;
        }
        case MessageType::Image: {
                content = json(toRoomMessage<msg::Image>(msg));
                break;
        }
        case MessageType::Text: {
                content = json(toRoomMessage<msg::Text>(msg));
                break;
        }
        case MessageType::Video: {
                content = json(toRoomMessage<msg::Video>(msg));
                break;
        }
        default:
                break;
        }

        json doc{{"type", "m.room.message"}, {"content", content}, {"room_id", room_id}};

        try {
                // Check if we have already an outbound megolm session then we can use.
                if (cache::client()->outboundMegolmSessionExists(room_id)) {
                        auto data = olm::encrypt_group_message(
                          room_id, http::client()->device_id(), doc.dump());

                        http::client()->send_room_message<msg::Encrypted, EventType::RoomEncrypted>(
                          room_id,
                          msg.txn_id,
                          data,
                          std::bind(&TimelineView::sendRoomMessageHandler,
                                    this,
                                    msg.txn_id,
                                    std::placeholders::_1,
                                    std::placeholders::_2));
                        return;
                }

                nhlog::ui()->debug("creating new outbound megolm session");

                // Create a new outbound megolm session.
                auto outbound_session  = olm::client()->init_outbound_group_session();
                const auto session_id  = mtx::crypto::session_id(outbound_session.get());
                const auto session_key = mtx::crypto::session_key(outbound_session.get());

                // TODO: needs to be moved in the lib.
                auto megolm_payload = json{{"algorithm", "m.megolm.v1.aes-sha2"},
                                           {"room_id", room_id},
                                           {"session_id", session_id},
                                           {"session_key", session_key}};

                // Saving the new megolm session.
                // TODO: Maybe it's too early to save.
                OutboundGroupSessionData session_data;
                session_data.session_id    = session_id;
                session_data.session_key   = session_key;
                session_data.message_index = 0; // TODO Update me
                cache::client()->saveOutboundMegolmSession(
                  room_id, session_data, std::move(outbound_session));

                const auto members = cache::client()->roomMembers(room_id);
                nhlog::ui()->info("retrieved {} members for {}", members.size(), room_id);

                auto keeper = std::make_shared<StateKeeper>(
                  [megolm_payload, room_id, doc, txn_id = msg.txn_id, this]() {
                          try {
                                  auto data = olm::encrypt_group_message(
                                    room_id, http::client()->device_id(), doc.dump());

                                  http::client()
                                    ->send_room_message<msg::Encrypted, EventType::RoomEncrypted>(
                                      room_id,
                                      txn_id,
                                      data,
                                      std::bind(&TimelineView::sendRoomMessageHandler,
                                                this,
                                                txn_id,
                                                std::placeholders::_1,
                                                std::placeholders::_2));

                          } catch (const lmdb::error &e) {
                                  nhlog::db()->critical(
                                    "failed to save megolm outbound session: {}", e.what());
                          }
                  });

                mtx::requests::QueryKeys req;
                for (const auto &member : members)
                        req.device_keys[member] = {};

                http::client()->query_keys(
                  req,
                  [keeper = std::move(keeper), megolm_payload, this](
                    const mtx::responses::QueryKeys &res, mtx::http::RequestErr err) {
                          if (err) {
                                  nhlog::net()->warn("failed to query device keys: {} {}",
                                                     err->matrix_error.error,
                                                     static_cast<int>(err->status_code));
                                  // TODO: Mark the event as failed. Communicate with the UI.
                                  return;
                          }

                          for (const auto &user : res.device_keys) {
                                  // Mapping from a device_id with valid identity keys to the
                                  // generated room_key event used for sharing the megolm session.
                                  std::map<std::string, std::string> room_key_msgs;
                                  std::map<std::string, DevicePublicKeys> deviceKeys;

                                  room_key_msgs.clear();
                                  deviceKeys.clear();

                                  for (const auto &dev : user.second) {
                                          const auto user_id   = UserId(dev.second.user_id);
                                          const auto device_id = DeviceId(dev.second.device_id);

                                          const auto device_keys = dev.second.keys;
                                          const auto curveKey    = "curve25519:" + device_id.get();
                                          const auto edKey       = "ed25519:" + device_id.get();

                                          if ((device_keys.find(curveKey) == device_keys.end()) ||
                                              (device_keys.find(edKey) == device_keys.end())) {
                                                  nhlog::net()->debug(
                                                    "ignoring malformed keys for device {}",
                                                    device_id.get());
                                                  continue;
                                          }

                                          DevicePublicKeys pks;
                                          pks.ed25519    = device_keys.at(edKey);
                                          pks.curve25519 = device_keys.at(curveKey);

                                          try {
                                                  if (!mtx::crypto::verify_identity_signature(
                                                        json(dev.second), device_id, user_id)) {
                                                          nhlog::crypto()->warn(
                                                            "failed to verify identity keys: {}",
                                                            json(dev.second).dump(2));
                                                          continue;
                                                  }
                                          } catch (const json::exception &e) {
                                                  nhlog::crypto()->warn(
                                                    "failed to parse device key json: {}",
                                                    e.what());
                                                  continue;
                                          } catch (const mtx::crypto::olm_exception &e) {
                                                  nhlog::crypto()->warn(
                                                    "failed to verify device key json: {}",
                                                    e.what());
                                                  continue;
                                          }

                                          auto room_key = olm::client()
                                                            ->create_room_key_event(
                                                              user_id, pks.ed25519, megolm_payload)
                                                            .dump();

                                          room_key_msgs.emplace(device_id, room_key);
                                          deviceKeys.emplace(device_id, pks);
                                  }

                                  std::vector<std::string> valid_devices;
                                  valid_devices.reserve(room_key_msgs.size());
                                  for (auto const &d : room_key_msgs) {
                                          valid_devices.push_back(d.first);

                                          nhlog::net()->info("{}", d.first);
                                          nhlog::net()->info("  curve25519 {}",
                                                             deviceKeys.at(d.first).curve25519);
                                          nhlog::net()->info("  ed25519 {}",
                                                             deviceKeys.at(d.first).ed25519);
                                  }

                                  nhlog::net()->info(
                                    "sending claim request for user {} with {} devices",
                                    user.first,
                                    valid_devices.size());

                                  http::client()->claim_keys(
                                    user.first,
                                    valid_devices,
                                    std::bind(&TimelineView::handleClaimedKeys,
                                              this,
                                              keeper,
                                              room_key_msgs,
                                              deviceKeys,
                                              user.first,
                                              std::placeholders::_1,
                                              std::placeholders::_2));

                                  // TODO: Wait before sending the next batch of requests.
                                  std::this_thread::sleep_for(std::chrono::milliseconds(500));
                          }
                  });

                // TODO: Let the user know about the errors.
        } catch (const lmdb::error &e) {
                nhlog::db()->critical(
                  "failed to open outbound megolm session ({}): {}", room_id, e.what());
        } catch (const mtx::crypto::olm_exception &e) {
                nhlog::crypto()->critical(
                  "failed to open outbound megolm session ({}): {}", room_id, e.what());
        }
}

void
TimelineView::handleClaimedKeys(std::shared_ptr<StateKeeper> keeper,
                                const std::map<std::string, std::string> &room_keys,
                                const std::map<std::string, DevicePublicKeys> &pks,
                                const std::string &user_id,
                                const mtx::responses::ClaimKeys &res,
                                mtx::http::RequestErr err)
{
        if (err) {
                nhlog::net()->warn("claim keys error: {} {} {}",
                                   err->matrix_error.error,
                                   err->parse_error,
                                   static_cast<int>(err->status_code));
                return;
        }

        nhlog::net()->debug("claimed keys for {}", user_id);

        if (res.one_time_keys.size() == 0) {
                nhlog::net()->debug("no one-time keys found for user_id: {}", user_id);
                return;
        }

        if (res.one_time_keys.find(user_id) == res.one_time_keys.end()) {
                nhlog::net()->debug("no one-time keys found for user_id: {}", user_id);
                return;
        }

        auto retrieved_devices = res.one_time_keys.at(user_id);

        // Payload with all the to_device message to be sent.
        json body;
        body["messages"][user_id] = json::object();

        for (const auto &rd : retrieved_devices) {
                const auto device_id = rd.first;
                nhlog::net()->debug("{} : \n {}", device_id, rd.second.dump(2));

                // TODO: Verify signatures
                auto otk = rd.second.begin()->at("key");

                if (pks.find(device_id) == pks.end()) {
                        nhlog::net()->critical("couldn't find public key for device: {}",
                                               device_id);
                        continue;
                }

                auto id_key = pks.at(device_id).curve25519;
                auto s      = olm::client()->create_outbound_session(id_key, otk);

                if (room_keys.find(device_id) == room_keys.end()) {
                        nhlog::net()->critical("couldn't find m.room_key for device: {}",
                                               device_id);
                        continue;
                }

                auto device_msg = olm::client()->create_olm_encrypted_content(
                  s.get(), room_keys.at(device_id), pks.at(device_id).curve25519);

                try {
                        cache::client()->saveOlmSession(id_key, std::move(s));
                } catch (const lmdb::error &e) {
                        nhlog::db()->critical("failed to save outbound olm session: {}", e.what());
                } catch (const mtx::crypto::olm_exception &e) {
                        nhlog::crypto()->critical("failed to pickle outbound olm session: {}",
                                                  e.what());
                }

                body["messages"][user_id][device_id] = device_msg;
        }

        nhlog::net()->info("send_to_device: {}", user_id);

        http::client()->send_to_device(
          "m.room.encrypted", body, [keeper](mtx::http::RequestErr err) {
                  if (err) {
                          nhlog::net()->warn("failed to send "
                                             "send_to_device "
                                             "message: {}",
                                             err->matrix_error.error);
                  }

                  (void)keeper;
          });
}
